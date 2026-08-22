#!/usr/bin/env python3
"""bench/jsonl_check.py -- Phase 8 W8.7 (phase-8-plan.md): the harness's pure
record logic, shared by bench/measure.sh (emit/segments/config), the ctest
gates bench.Schema/Counts/Segments (bench/test_jsonl_check.py), and HSTx3.
Stdlib only. Every assertion message is prefixed "HST:" / "CFG:" and names
the offending key so each is reachable by a one-field mutation (R6-NOTAUTO).

Schema authority: .handoff/sympsica-plan.md lines 29-35 + controller
amendments A1 (Codex round 1) and A3 (Codex round 2): env gains "DERIVED";
DERIVED rows have r_out = s_out = 0 and total == external_total == published
bytes, and carry the source's VERBATIM network label ("200 Mbps", with the
space, never WAN200/50/5); measured LOCAL/AWS rows keep total == r_out + s_out
== external_total; scenario gains exactly selftest | calib | derived.
"""
import argparse
import datetime
import hashlib
import json
import re
import sys

TOP_KEYS = {"ts", "env", "scenario", "config", "trial", "seed", "status",
            "time_s", "bytes", "counters", "notes"}
DERIVED_EXTRA_KEYS = {"measured", "source"}
CONFIG_KEYS = {"n", "u", "network", "protocol", "variant"}
TIME_KEYS = {"online", "offline", "total"}
BYTES_KEYS = {"r_out", "s_out", "total", "external_total"}
COUNTER_KEYS = {"triples", "rounds", "announced"}

NETWORKS = {"LAN", "WAN200", "WAN50", "WAN5"}                 # measured rows
NETWORKS_DERIVED = {"LAN", "200 Mbps", "50 Mbps", "5 Mbps"}    # DERIVED rows: VERBATIM source column labels (A1)

# A10 (controller amendment, 2026-08-22): BMS+24 runs both parties in ONE
# shared-loopback netns (gRPC "local credentials" accept only the literal
# address 127.0.0.1 -- no per-party netns pair can satisfy that on both ends
# at once). A loopback pair cannot yield per-party OUTGOING counts on
# separate interfaces, so these rows carry a THIRD byte regime: r_out=s_out=0,
# total=external_total measured from the shared `lo`'s tx delta. Gated on the
# mandatory token below so a mislabeled row can never silently pass the
# strict total==r_out+s_out check meant for real per-party measurements.
COMBINED_LOOPBACK_TOKEN = "bytes=combined-loopback"
COMBINED_TOTAL_RE = re.compile(r"^combined_total_b=([0-9]+)$")
# The veth-pair deltas measure.sh computes generically for EVERY docker-mode
# trial (r_out/s_out passed into build_record) are not exactly zero for
# combined-loopback rows, even though bms24's traffic never touches that pair.
#
# M1 (gate fix round 1, Task 7): a prior comment here attributed this to
# `bench/netem.sh apply`'s 20 calibration pings. That is impossible --
# measure.sh takes the "before" byte snapshot AFTER apply/verify complete
# (step 8 finishes, then step 10 snapshots), so pings that already finished
# cannot appear in a delta measured starting after them. tcpdump on veth_r
# during the actual bracketed window identified the real source: IPv6 router
# solicitation / neighbor solicitation / multicast-listener-report traffic
# the kernel sends when the veth interface transitions up. Measured (colima,
# 3x30s trials): 140/252/210 B without mitigation.
#
# `bench/netem.sh cmd_netns_up` now disables IPv6 on both veth ends before
# bringing them up (0/0/42 B across 3 trials WITH that fix) -- the traffic IS
# avoidable in principle. That fix, however, reaches only the native-exec_mode
# path; `bench/measure.sh`'s own separate docker-mode veth setup (its step 8,
# ~line 342) creates an unshaped duplicate of the same veth pair and is
# OUTSIDE this fix round's authorized file list, so it does not yet carry the
# same sysctl -- and bms24 (the only combined-loopback protocol) always runs
# exec_mode=docker. The ceiling below therefore still has to bound the
# UNMITIGATED docker-mode noise (same 140-252 B range measured above), so it
# is kept at 8192 B rather than lowered, until bench/measure.sh's duplicate
# setup gets the same fix in a future round. 8192 B is ~9x the observed
# unmitigated noise and ~3000x below real bms24 traffic (2.68 MB in a real
# colima smoke run), so a genuine leak of bms24 traffic onto the veth pair --
# which would be MB-scale by construction -- still trips this check.
VETH_CALIBRATION_NOISE_MAX_B = 8192
SCENARIOS = {"S1", "S2", "S3", "U", "M0", "smoke",               # master plan line 30
             "selftest", "calib", "derived"}                      # controller amendment A3 -- exactly these three additions
STATUSES = {"ok", "timeout", "dnf", "error"}
HSTX3_STATUSES = {"ok", "timeout", "dnf"}                     # master plan W8.7
ENVS = {"LOCAL", "AWS", "DERIVED"}
SEGMENTS = ("setup_once", "preprocessing", "online")
MARKER_RE = re.compile(r"^PHASE:(total_workload|setup_once|preprocessing|online)_(begin|end):([0-9]+)$")
MARKER_ORDER = ["total_workload_begin", "setup_once_begin", "setup_once_end",
                "preprocessing_begin", "preprocessing_end", "online_begin",
                "online_end", "total_workload_end"]


def _is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def _is_num(v):
    return (isinstance(v, (int, float))) and not isinstance(v, bool)


def validate_record(rec):
    assert isinstance(rec, dict), "HST: record is not a JSON object"
    keys = set(rec.keys())
    env = rec.get("env")
    allowed = TOP_KEYS | (DERIVED_EXTRA_KEYS if env == "DERIVED" else set())
    for k in sorted((TOP_KEYS | (DERIVED_EXTRA_KEYS if env == "DERIVED" else set())) - keys):
        raise AssertionError("HST: missing top-level key '%s'" % k)
    for k in sorted(keys - allowed):
        raise AssertionError("HST: unknown top-level key '%s' (only DERIVED rows carry measured/source)" % k)
    assert isinstance(rec["ts"], str) and len(rec["ts"]) >= 19, "HST: ts must be an iso8601 string"
    assert env in ENVS, "HST: env '%s' not in %s" % (env, sorted(ENVS))
    assert rec["scenario"] in SCENARIOS, "HST: scenario '%s' not in %s (A3)" % (rec["scenario"], sorted(SCENARIOS))
    cfg = rec["config"]
    assert isinstance(cfg, dict) and set(cfg.keys()) == CONFIG_KEYS, \
        "HST: config keys must be exactly %s" % sorted(CONFIG_KEYS)
    assert _is_int(cfg["n"]) and cfg["n"] > 0, "HST: config.n must be a positive int"
    assert _is_int(cfg["u"]) and cfg["u"] >= 0, "HST: config.u must be a non-negative int"
    if env == "DERIVED":
        assert cfg["network"] in NETWORKS_DERIVED, \
            "HST: DERIVED rows carry the source's verbatim network label %s, never a relabel onto an 80 ms profile (got '%s')" % (sorted(NETWORKS_DERIVED), cfg["network"])
    else:
        assert cfg["network"] in NETWORKS, "HST: config.network '%s' not in %s" % (cfg["network"], sorted(NETWORKS))
    assert isinstance(cfg["protocol"], str) and cfg["protocol"], "HST: config.protocol must be a non-empty string"
    assert isinstance(cfg["variant"], str) and cfg["variant"], "HST: config.variant must be a non-empty string"
    assert _is_int(rec["trial"]) and rec["trial"] >= 0, "HST: trial must be a non-negative int"
    assert _is_int(rec["seed"]) and rec["seed"] >= 0, "HST: seed must be a non-negative int"
    assert rec["status"] in STATUSES, "HST: status '%s' not in %s" % (rec["status"], sorted(STATUSES))
    t = rec["time_s"]
    assert isinstance(t, dict) and set(t.keys()) == TIME_KEYS, "HST: time_s keys must be exactly %s" % sorted(TIME_KEYS)
    for k in sorted(TIME_KEYS):
        assert _is_num(t[k]), "HST: time_s.%s must be a number" % k
        assert t[k] >= 0, "HST: time_s.%s must be >= 0" % k
    b = rec["bytes"]
    assert isinstance(b, dict) and set(b.keys()) == BYTES_KEYS, "HST: bytes keys must be exactly %s" % sorted(BYTES_KEYS)
    for k in sorted(BYTES_KEYS):
        assert _is_int(b[k]) and b[k] >= 0, "HST: bytes.%s must be a non-negative int" % k
    # notes' own type is asserted further below (P25); guard here so a
    # malformed (non-string) notes field still reaches THAT assertion instead
    # of crashing on .split() first.
    notes_list = rec["notes"].split(";") if isinstance(rec.get("notes"), str) else []
    combined_flag = COMBINED_LOOPBACK_TOKEN in notes_list
    combined_vals = [m.group(1) for tok in notes_list for m in (COMBINED_TOTAL_RE.match(tok),) if m]
    # I3 (gate fix round 1, Task 6): the combined-loopback regime was granted
    # to bms24 alone (A10 -- BMS+24's gRPC "local credentials" force both
    # parties into one shared netns, which no other wrapper does). Scope the
    # tokens to that protocol at the schema level so a row from any other
    # protocol carrying them (accidentally or otherwise) can never be pooled
    # with bms24's combined totals by a Phase-9 consumer that only checks the
    # notes tokens.
    if (combined_flag or combined_vals) and env != "DERIVED":
        assert cfg["protocol"] == "bms24", \
            "HST: combined-loopback regime is granted to bms24 only (A10); protocol=%s" % cfg["protocol"]
    if env == "DERIVED":
        assert b["r_out"] == 0 and b["s_out"] == 0, \
            "HST: DERIVED rows have no directional split: r_out and s_out must be 0 (A1)"
        assert b["total"] == b["external_total"], \
            "HST: DERIVED rows require bytes.total == external_total == published bytes (A1)"
    elif combined_flag or combined_vals:
        # I3 (Codex review, Task 34 fix round): the FILE-LEVEL validator must enforce the
        # same two-token bridge build_record does, not just notice the flag -- otherwise a
        # damaged/edited bridge (flag with no numeric token, a numeric token with no flag,
        # or a numeric token that no longer matches bytes.total) passes `jsonl_check.py
        # validate` even though it could never have come out of build_record honestly.
        assert combined_flag and combined_vals, \
            "HST: bytes=combined-loopback and combined_total_b=<int> must be present together (A10); got flag=%s combined_total_b=%s" % (combined_flag, combined_vals)
        assert len(combined_vals) == 1, \
            "HST: exactly one combined_total_b=<int> token is required (A10), got %s" % (combined_vals,)
        assert b["r_out"] == 0 and b["s_out"] == 0, \
            "HST: combined-loopback rows have no directional split: r_out and s_out must be 0 (A10)"
        assert b["total"] == b["external_total"], \
            "HST: combined-loopback rows require bytes.total == external_total (A10)"
        assert b["total"] > 0, "HST: combined-loopback rows require bytes.total > 0 (A10)"
        assert int(combined_vals[0]) == b["total"], \
            "HST: combined_total_b=%s must equal bytes.total %d (A10)" % (combined_vals[0], b["total"])
    else:
        assert b["total"] == b["r_out"] + b["s_out"], \
            "HST: bytes.total %d != r_out+s_out %d" % (b["total"], b["r_out"] + b["s_out"])
        assert b["external_total"] == b["total"], \
            "HST: bytes.external_total %d != bytes.total %d" % (b["external_total"], b["total"])
    c = rec["counters"]
    assert isinstance(c, dict) and set(c.keys()) == COUNTER_KEYS, "HST: counters keys must be exactly %s" % sorted(COUNTER_KEYS)
    for k in sorted(COUNTER_KEYS):
        assert _is_int(c[k]) and c[k] >= 0, "HST: counters.%s must be a non-negative int" % k
    assert isinstance(rec["notes"], str), "HST: notes must be a string"
    if env == "DERIVED":
        assert rec["measured"] is False, "HST: DERIVED row requires 'measured': false"
        assert rec["source"] == "ACNS26-Tables3-4", "HST: DERIVED row requires source 'ACNS26-Tables3-4'"


def record_key(rec):
    c = rec["config"]
    return (rec["scenario"], c["protocol"], c["variant"], c["n"], c["u"], c["network"])


def check_regime_consistency(records):
    """I3 (gate fix round 1, Task 6): within ONE file, a single protocol may
    not mix combined-loopback rows with ordinary two-sided rows -- that would
    let a table builder pool bytes.total across two different byte-accounting
    semantics for what looks like one protocol column. Different protocols
    coexisting (each internally consistent) is fine -- that is the normal
    Phase-9 multi-protocol table. DERIVED rows are a third, unrelated regime
    and are excluded from this bridge entirely."""
    seen = {}
    for rec in records:
        if rec.get("env") == "DERIVED":
            continue
        cfg = rec.get("config", {})
        proto = cfg.get("protocol")
        notes_list = rec["notes"].split(";") if isinstance(rec.get("notes"), str) else []
        combined = COMBINED_LOOPBACK_TOKEN in notes_list or any(COMBINED_TOTAL_RE.match(t) for t in notes_list)
        seen.setdefault(proto, set()).add("combined" if combined else "two-sided")
    for proto, regimes in seen.items():
        assert len(regimes) <= 1, "HST: mixed byte regimes for protocol %s in one file" % proto


def check_counts(records, expected_keys, trials):
    """HSTx3: every expected (scenario, protocol, variant, n, u, network) has
    exactly `trials` records with trial ids 0..trials-1 each exactly once, and
    every status is in {ok, timeout, dnf}."""
    seen = {}
    for rec in records:
        assert rec["status"] in HSTX3_STATUSES, \
            "HST: status '%s' not allowed by HSTx3 (ok|timeout|dnf)" % rec["status"]
        k = record_key(rec)
        assert k in expected_keys, "HST: unexpected cell %s not in the expected set" % (k,)
        slot = seen.setdefault(k, set())
        assert rec["trial"] not in slot, "HST: duplicate record for %s trial %d" % (k, rec["trial"])
        slot.add(rec["trial"])
    for k in expected_keys:
        got = seen.get(k, set())
        assert got == set(range(trials)), \
            "HST: expected cell %s has trials %s, wanted 0..%d exactly once" % (k, sorted(got), trials - 1)


def parse_segments(stderr_text, allow_incomplete=False):
    """Eight PHASE markers from ONE owner (the wrapper): returns
    {"total": s, "setup_once": s, "preprocessing": s, "online": s, "_ns": {...}}.
    With allow_incomplete=True (timeout/error synthesis) returns the present
    marker ns values under "_ns" and sets absent segments to None."""
    seen = {}
    for line in stderr_text.splitlines():
        m = MARKER_RE.match(line.strip())
        if not m:
            continue
        name = "%s_%s" % (m.group(1), m.group(2))
        assert name not in seen, "HST: marker %s appears more than once (markers have ONE owner: run.sh)" % name
        seen[name] = int(m.group(3))
    if not allow_incomplete:
        for name in MARKER_ORDER:
            assert name in seen, "HST: marker %s absent" % name
        last = None
        for name in MARKER_ORDER:
            if last is not None:
                assert seen[name] >= seen[last], "HST: markers not monotonic: %s (%d) < %s (%d)" % (name, seen[name], last, seen[last])
            last = name
        # (begin <= end per pair is implied by the monotonic check over MARKER_ORDER,
        #  which lists every begin before its end -- no separate assertion.)
    out = {"_ns": seen}
    for seg, key in (("total_workload", "total"), ("setup_once", "setup_once"),
                     ("preprocessing", "preprocessing"), ("online", "online")):
        b, e = seen.get(seg + "_begin"), seen.get(seg + "_end")
        out[key] = (e - b) / 1e9 if (b is not None and e is not None) else None
    return out


def divergence_flag(internal_b, external_b):
    """FC: internal-vs-external divergence strictly greater than 5% flags the record.
    I2 (gate fix round 1, Task 5): a zero external denominator with a real
    (nonzero) internal counter is NOT "no divergence" -- that silent False was
    exactly the combined-loopback bypass the review found (A10 sets r_out=0,
    so the old zero-denominator branch discarded any BMS internal/external
    discrepancy of any size). Only a genuinely empty pair (both zero) is
    still uninformative and returns False."""
    if external_b <= 0:
        if internal_b > 0:
            raise ValueError("divergence: external denominator is zero while an internal counter is present")
        return False
    return abs(internal_b - external_b) / float(external_b) > 0.05


# --- wrapper config schemas (argv/keys are the wrapper contract, § G) -------
COMMON_REQUIRED = {"scenario": str, "protocol": str, "variant": str, "n": int, "u": int,
                   "network": str, "seed": int, "queries": int, "exec_mode": str,
                   "image": str, "state_dir": str, "budget_s": int, "thread_regime": str}
PER_PROTOCOL_REQUIRED = {
    "bms24": {"func": str, "days": int, "daily_size": int, "start_size": int, "del_size": int},
    "fastupsi": {"prot": str, "days": int, "add_size": int, "del_size": int, "start_size": int,
                 "del": bool, "protocol_avg": str},
    "kunlun": {"log_item_num": int},
    "volepsi": {"inter": int},
    "minisketch": {"inter": int, "capacity": int},
    "selftest": {},
}
OPTIONAL_KEYS = {"delete_mode": str, "mem": str, "warmup": int}


def validate_config(cfg):
    assert isinstance(cfg, dict), "CFG: config is not a JSON object"
    proto = cfg.get("protocol")
    assert proto in PER_PROTOCOL_REQUIRED, "CFG: protocol '%s' not one of %s" % (proto, sorted(PER_PROTOCOL_REQUIRED))
    required = dict(COMMON_REQUIRED)
    required.update(PER_PROTOCOL_REQUIRED[proto])
    for k in sorted(required):
        assert k in cfg, "CFG: missing key '%s'" % k
    for k in sorted(cfg):
        assert k in required or k in OPTIONAL_KEYS, "CFG: unknown key '%s'" % k
    for k, typ in sorted(list(required.items()) + [(k, v) for k, v in OPTIONAL_KEYS.items() if k in cfg]):
        v = cfg[k]
        if typ is int:
            ok = _is_int(v)
        elif typ is bool:
            ok = isinstance(v, bool)
        else:
            ok = isinstance(v, str)
        assert ok, "CFG: key '%s' must be %s" % (k, typ.__name__)
    assert cfg["scenario"] in SCENARIOS, "CFG: scenario '%s' not in %s (A3)" % (cfg["scenario"], sorted(SCENARIOS))
    assert cfg["network"] in NETWORKS, "CFG: network '%s' not in %s" % (cfg["network"], sorted(NETWORKS))
    assert cfg["exec_mode"] in ("docker", "native"), "CFG: exec_mode must be docker|native"
    assert cfg["thread_regime"] in ("strict", "runtime-helpers"), "CFG: thread_regime must be strict|runtime-helpers"
    assert cfg["queries"] >= 1, "CFG: queries must be >= 1"
    assert cfg["budget_s"] >= 1, "CFG: budget_s must be >= 1"
    img = cfg["image"]
    if cfg["exec_mode"] == "docker":
        assert "@sha256:" in img or img.startswith("sha256:"), \
            "CFG: docker image must be a registry digest ref (...@sha256:) or a local image ID (sha256:...), not a tag"


def prepare_key(cfg):
    """sha256 of the prepare-relevant config (network/budget/warmup/queries/seed removed)."""
    sub = {k: v for k, v in cfg.items() if k not in ("network", "budget_s", "warmup", "queries", "seed")}
    return hashlib.sha256(json.dumps(sub, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def complete_segments(segments, kill_ns):
    """Timeout/error synthesis (C6): every segment whose begin was observed but
    whose end was not gets end := kill_ns (the harness's kill/observe instant);
    segments never begun stay 0.0 via build_record. Mutates and returns."""
    ns = segments.get("_ns", {})
    for seg, key in (("total_workload", "total"), ("setup_once", "setup_once"),
                     ("preprocessing", "preprocessing"), ("online", "online")):
        b, e = ns.get(seg + "_begin"), ns.get(seg + "_end")
        if b is not None and e is None:
            assert kill_ns >= b, "HST: kill_ns %d precedes %s_begin %d" % (kill_ns, seg, b)
            segments[key] = (kill_ns - b) / 1e9
            segments.setdefault("_synth", []).append(seg)
    return segments


# A6: the sampler's single output channel. Per side: {"state": "observed"|"unsampled",
# "threads_max": int|None, "rss_kb": int|None}. /proc/<pid>/status line regexes (pinned):
THREADS_RE = re.compile(r"^Threads:\s+([0-9]+)$")
VMHWM_RE = re.compile(r"^VmHWM:\s+([0-9]+)\s+kB$")
OBS_STATES = {"observed", "unsampled"}


def validate_observations(obs):
    assert isinstance(obs, dict) and set(obs.keys()) == {"r", "s"}, \
        "HST: observations must have exactly sides r and s"
    for side in ("r", "s"):
        o = obs[side]
        assert isinstance(o, dict) and set(o.keys()) == {"state", "threads_max", "rss_kb"}, \
            "HST: observation keys must be exactly state/threads_max/rss_kb (side %s)" % side
        assert o["state"] in OBS_STATES, "HST: observation state '%s' not in observed|unsampled" % o["state"]
        if o["state"] == "observed":
            assert _is_int(o["threads_max"]) and o["threads_max"] >= 1, \
                "HST: observed side %s must carry threads_max >= 1" % side
        else:
            assert o["threads_max"] is None and o["rss_kb"] is None, \
                "HST: unsampled side %s must carry null threads_max and rss_kb" % side
        if o["rss_kb"] is not None:
            assert _is_int(o["rss_kb"]) and o["rss_kb"] >= 0, "HST: rss_kb must be a non-negative int"


def build_record(cfg, env, status, segments, r_out, s_out, obs, notes_tokens, trial, ts=None):
    if ts is None:
        ts = datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")
    notes = list(notes_tokens)
    # Thread policy = controller amendments A2 + A5 + A6 (post-hoc invalidation at emit;
    # ONE channel: the per-side observation objects; null rss is OMITTED, never zero-filled).
    validate_observations(obs)
    regime = cfg["thread_regime"]
    observed = {k: o for k, o in obs.items() if o["state"] == "observed"}
    if observed:
        notes.append("threads=" + ",".join("%s:%d" % (k, observed[k]["threads_max"]) for k in ("r", "s") if k in observed))
    rss_parts = ["%s:%d" % (k, obs[k]["rss_kb"]) for k in ("r", "s") if obs[k]["rss_kb"] is not None]
    if rss_parts:
        notes.append("rss_kb=" + ",".join(rss_parts))
    for side in ("r", "s"):
        if obs[side]["state"] == "unsampled":
            notes.append("party-pid-unsampled=%s" % side)
            status = "error"
    if regime == "strict":
        bad = {k: o["threads_max"] for k, o in observed.items() if o["threads_max"] != 1}
        if bad:
            notes.append("threads_violation=" + ",".join("%s:%d" % (k, bad[k]) for k in ("r", "s") if k in bad))
            status = "error"
    else:
        if any(o["threads_max"] > 1 for o in observed.values()):
            notes.append("threads>1:runtime-helpers")
    if segments.get("setup_once") is not None:
        notes.append("setup_once_s=%.6f" % segments["setup_once"])
    if segments.get("_synth"):
        notes.append("segments-synthesized=" + ",".join(segments["_synth"]))
        assert status in ("timeout", "error"), "HST: synthesized segments require status timeout|error"
    # A10: combined-loopback byte regime (BMS+24's single shared-loopback
    # topology has no per-party split). Both tokens are required TOGETHER --
    # either one without the other is an assertion failure, never a silent
    # fallback to r_out+s_out (a regime that can half-apply eventually
    # mislabels a row). The veth-pair deltas measure.sh computed generically
    # must genuinely be zero in this regime (no traffic was meant to cross
    # the veth pair); a non-zero delta is a real bug signal, not something to
    # silently override.
    combined_flag = COMBINED_LOOPBACK_TOKEN in notes_tokens
    combined_vals = [m.group(1) for tok in notes_tokens for m in (COMBINED_TOTAL_RE.match(tok),) if m]
    assert combined_flag == bool(combined_vals), \
        "HST: bytes=combined-loopback and combined_total_b=<int> must be present together (A10); got flag=%s combined_total_b=%s" % (combined_flag, combined_vals)
    if combined_flag:
        assert r_out + s_out <= VETH_CALIBRATION_NOISE_MAX_B, \
            "HST: combined-loopback regime requires r_out+s_out <= %d B (harness calibration-noise ceiling) from the veth pair (observed r_out=%d s_out=%d, sum=%d) (A10)" % (VETH_CALIBRATION_NOISE_MAX_B, r_out, s_out, r_out + s_out)
        combined_total = int(combined_vals[0])
        # I2 (gate fix round 1, Task 5): compare BMS+24's OWN internal
        # comm-byte counter against the external combined total, comparing
        # ONLY like with like. Two counter SCOPES are distinguished by token
        # name (established by real measurement, task report): add-only's
        # party 1 prints "Total Comm (B)" -- measured 2,681,284 B against a
        # same-run combined_total_b of 2,689,437 B (ratio 0.997), i.e. it IS
        # already the combined total, wired here as `internal_comm_b`.
        # add-del's party 0 prints "Total Comm Sent(B)" -- the label says
        # "Sent", i.e. that PARTY's own directional half, not a total; only
        # one directional half is ever printed (party 1's is not), so it can
        # never be safely compared alone -- summing two real per-party
        # halves is required before a like-with-like comparison exists.
        internal_combined = next((int(t.split("=", 1)[1]) for t in notes_tokens if t.startswith("internal_comm_b=")), None)
        internal_r = next((int(t.split("=", 1)[1]) for t in notes_tokens if t.startswith("internal_b_r=")), None)
        internal_s = next((int(t.split("=", 1)[1]) for t in notes_tokens if t.startswith("internal_b_s=")), None)
        if internal_combined is not None:
            if divergence_flag(internal_combined, combined_total):
                notes.append("bytes-divergence")
        elif internal_r is not None and internal_s is not None:
            if divergence_flag(internal_r + internal_s, combined_total):
                notes.append("bytes-divergence")
        elif internal_r is not None or internal_s is not None:
            notes.append("internal-comparison=unavailable(scope-mismatch)")
        bytes_obj = {"r_out": 0, "s_out": 0, "total": combined_total, "external_total": combined_total}
    else:
        bytes_obj = {"r_out": r_out, "s_out": s_out, "total": r_out + s_out, "external_total": r_out + s_out}
    rec = {
        "ts": ts, "env": env, "scenario": cfg["scenario"],
        "config": {"n": cfg["n"], "u": cfg["u"], "network": cfg["network"],
                   "protocol": cfg["protocol"], "variant": cfg["variant"]},
        "trial": trial, "seed": cfg["seed"], "status": status,
        "time_s": {"online": float(segments.get("online") or 0.0),
                   "offline": float(segments.get("preprocessing") or 0.0),
                   "total": float(segments.get("total") or 0.0)},
        "bytes": bytes_obj,
        "counters": {"triples": 0, "rounds": 0, "announced": 0},
        "notes": ";".join(notes),
    }
    validate_record(rec)
    return rec


def notes_tokens(rec):
    return [t for t in rec["notes"].split(";") if t]


def accept_record(rec, status="ok", expect=(), flags=(), expect_re=(), bytes_within=None,
                  online_within=None, total_within=None, min_r_out=None, min_s_out=None):
    """Executable acceptance gate for smokes/calibration (round-3 C2). Every
    predicate is a fixed 'ACC:' message; `expect` = exact notes tokens that
    must be present; `flags` = tokens that must be present (same check, named
    separately in the message); ranges are inclusive."""
    validate_record(rec)
    assert rec["status"] == status, "ACC: status '%s' != required '%s'" % (rec["status"], status)
    toks = notes_tokens(rec)
    for e in expect:
        assert e in toks, "ACC: required notes token '%s' absent (notes=%r)" % (e, rec["notes"])
    for f in flags:
        assert f in toks, "ACC: required flag '%s' absent" % f
    for rx in expect_re:
        assert any(re.fullmatch(rx, t) for t in toks), \
            "ACC: no notes token matches /%s/ (notes=%r)" % (rx, rec["notes"])
    b = rec["bytes"]; t = rec["time_s"]
    if bytes_within is not None:
        lo, hi = bytes_within
        assert lo <= b["total"] <= hi, "ACC: bytes.total %d outside [%d, %d]" % (b["total"], lo, hi)
    if online_within is not None:
        lo, hi = online_within
        assert lo <= t["online"] <= hi, "ACC: time_s.online %s outside [%s, %s]" % (t["online"], lo, hi)
    if total_within is not None:
        lo, hi = total_within
        assert lo <= t["total"] <= hi, "ACC: time_s.total %s outside [%s, %s]" % (t["total"], lo, hi)
    assert t["total"] >= t["online"], "ACC: time_s.total %s < time_s.online %s" % (t["total"], t["online"])
    if min_r_out is not None:
        assert b["r_out"] >= min_r_out, "ACC: bytes.r_out %d < %d" % (b["r_out"], min_r_out)
    if min_s_out is not None:
        assert b["s_out"] >= min_s_out, "ACC: bytes.s_out %d < %d" % (b["s_out"], min_s_out)


def _range(text):
    lo, hi = text.split(",")
    return (float(lo), float(hi)) if ("." in lo or "." in hi) else (int(lo), int(hi))


def main(argv=None):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("validate"); p.add_argument("--file", required=True)
    p = sub.add_parser("counts"); p.add_argument("--file", required=True)
    p.add_argument("--expect", action="append", required=True,
                   help="scenario;protocol;variant;n;u;network")
    p.add_argument("--trials", type=int, required=True)
    p = sub.add_parser("segments"); p.add_argument("--stderr", required=True)
    p.add_argument("--allow-incomplete", action="store_true")
    p = sub.add_parser("config"); p.add_argument("--file", required=True)
    p.add_argument("--get", default=None)
    p.add_argument("--prepare-key", action="store_true")
    p = sub.add_parser("accept"); p.add_argument("--file", required=True)
    p.add_argument("--line", type=int, default=-1, help="1-based record line; default: last")
    p.add_argument("--status", default="ok"); p.add_argument("--expect", action="append", default=[])
    p.add_argument("--flag", action="append", default=[]); p.add_argument("--expect-re", action="append", default=[])
    p.add_argument("--bytes-within", default=None); p.add_argument("--online-within", default=None)
    p.add_argument("--total-within", default=None)
    p.add_argument("--min-r-out", type=int, default=None); p.add_argument("--min-s-out", type=int, default=None)
    p.add_argument("--count", type=int, default=None, help="require exactly this many records in the file")
    p = sub.add_parser("emit")
    p.add_argument("--config", required=True); p.add_argument("--env", required=True)
    p.add_argument("--status", required=True); p.add_argument("--segments", required=True)
    p.add_argument("--r-out", type=int, required=True); p.add_argument("--s-out", type=int, required=True)
    p.add_argument("--observations", required=True, help="path to the sampler's threads.json (A6)")
    p.add_argument("--partial", default=None); p.add_argument("--trial", type=int, required=True)
    p.add_argument("--ts", default=None); p.add_argument("--out", required=True)
    p.add_argument("--token", action="append", default=[],
                   help="extra notes token added by the harness (e.g. delete_mode=...; thread tokens are NOT passed here -- build_record derives them from --observations)")
    p.add_argument("--kill-ns", type=int, default=None,
                   help="timeout/error only: monotonic ns at which the harness killed/observed the wrapper")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "validate":
            recs = []
            with open(a.file) as f:
                for i, line in enumerate(f, 1):
                    if not line.strip():
                        continue
                    rec = json.loads(line)
                    try:
                        validate_record(rec)
                    except AssertionError as e:
                        raise AssertionError("%s (line %d)" % (e, i))
                    recs.append(rec)
            check_regime_consistency(recs)   # I3 (Task 6): file-level mixed-regime refusal
            print("validate OK: %s" % a.file)
        elif a.cmd == "counts":
            with open(a.file) as f:
                recs = [json.loads(l) for l in f if l.strip()]
            keys = []
            for e in a.expect:
                s, pr, va, n, u, nw = e.split(";")
                keys.append((s, pr, va, int(n), int(u), nw))
            check_counts(recs, keys, a.trials)
            print("counts OK: %d records, %d cells x %d trials" % (len(recs), len(keys), a.trials))
        elif a.cmd == "segments":
            with open(a.stderr) as f:
                seg = parse_segments(f.read(), allow_incomplete=a.allow_incomplete)
            print(json.dumps(seg))
        elif a.cmd == "config":
            with open(a.file) as f:
                cfg = json.load(f)
            validate_config(cfg)
            if a.prepare_key:
                print(prepare_key(cfg))
            elif a.get is not None:
                v = cfg.get(a.get, "")
                print(str(v).lower() if isinstance(v, bool) else v)
            else:
                print("config OK: %s" % a.file)
        elif a.cmd == "accept":
            with open(a.file) as f:
                recs = [json.loads(l) for l in f if l.strip()]
            assert recs, "ACC: no records in %s" % a.file
            if a.count is not None:
                assert len(recs) == a.count, "ACC: %d records in %s, required exactly %d" % (len(recs), a.file, a.count)
            check_regime_consistency(recs)   # I3 (Task 6): file-level mixed-regime refusal
            rec = recs[a.line - 1] if a.line > 0 else recs[-1]
            accept_record(rec, a.status, a.expect, a.flag, a.expect_re,
                          _range(a.bytes_within) if a.bytes_within else None,
                          _range(a.online_within) if a.online_within else None,
                          _range(a.total_within) if a.total_within else None,
                          a.min_r_out, a.min_s_out)
            print("accept OK: %s (status=%s, %d expects, %d flags)" % (a.file, a.status, len(a.expect), len(a.flag)))
        elif a.cmd == "emit":
            with open(a.config) as f:
                cfg = json.load(f)
            validate_config(cfg)
            with open(a.segments) as f:
                segments = json.load(f)
            if a.kill_ns is not None:
                segments = complete_segments(segments, a.kill_ns)
            tokens = []
            if a.partial:
                try:
                    with open(a.partial) as f:
                        tokens = list(json.load(f).get("notes_tokens", []))
                except (OSError, ValueError):
                    tokens = ["partial-missing"]
            else:
                tokens = ["partial-missing"]
            if cfg["protocol"] != "selftest":   # every live baseline: counters are not externally observable
                tokens.append("counters=na")
            tokens.extend(a.token)
            with open(a.observations) as f:
                obs = json.load(f)
            rec = build_record(cfg, a.env, a.status, segments, a.r_out, a.s_out,
                               obs, tokens, a.trial, a.ts)
            with open(a.out, "a") as f:
                f.write(json.dumps(rec, separators=(",", ":")) + "\n")
            print("emit OK: trial %d status %s -> %s" % (a.trial, a.status, a.out))
    except AssertionError as e:
        sys.stderr.write("%s\n" % e)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())

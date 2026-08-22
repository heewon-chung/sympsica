#!/usr/bin/env python3
"""bench/r6_matrix.py -- Phase 8 R6-NOTAUTO matrix: 89 pure-function/policy
rows over bench/jsonl_check.py (phase-8-plan.md; 79 through Task 33, +10 for
Task 34/A10's combined-loopback byte regime, incl. the Codex-review fix
round's file-level validate_record bridge checks). One row per function-level
assertion; each row is a ONE-FIELD mutation of a valid fixture and must raise
AssertionError whose message contains the pinned substring. The two CLI-level
assertions (accept's "no records" and "--count") are covered by the LIVE rows
31-B.6 (l)/(v), not here -- the full evidence is "89 rows plus the live CLI
table". Run all:
    python3 bench/r6_matrix.py            (prints "<id> -> <real message>"; exit 2 if any
                                           row did NOT fail as pinned)
Run one:  python3 bench/r6_matrix.py --only P7
This helper is NOT a ctest entry (negatives never enter the green path); the
task report pastes its full output. Stdlib only."""
import copy
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jsonl_check as j
from test_jsonl_check import GOOD_RECORD, GOOD_DERIVED, GOOD_COMBINED, GOOD_KEY, GOOD_MARKERS

OBS_OK = {"state": "observed", "threads_max": 1, "rss_kb": 1000}
OBS_2T = {"state": "observed", "threads_max": 2, "rss_kb": 1000}
SEG = {"total": 1.0, "online": 0.5, "preprocessing": 0.0, "setup_once": 0.0}
GOOD_CONFIG = {"scenario": "selftest", "protocol": "selftest", "variant": "harness", "n": 1, "u": 0,
               "network": "LAN", "seed": 0, "queries": 1, "exec_mode": "native", "thread_regime": "strict",
               "image": "none", "state_dir": "/tmp/sympsica-bench/cache/selftest", "budget_s": 60, "warmup": 1}
# A VALID docker-mode config (validate_config passes on it as-is); C12 mutates ONLY its image.
GOOD_CONFIG_DOCKER = dict(GOOD_CONFIG, exec_mode="docker",
                          image="ghcr.io/example/image@sha256:0000000000000000000000000000000000000000000000000000000000000000")
# protocol=bms24 variant of GOOD_CONFIG for build_record's combined-loopback
# rows (B3-B6): I3 (Task 6) scopes the combined-loopback regime to bms24 at
# the schema level, so exercising it through build_record needs a bms24
# config, not GOOD_CONFIG's protocol="selftest".
GOOD_CONFIG_BMS24 = dict(GOOD_CONFIG, protocol="bms24", variant="add-only")


def must(cond, msg):
    assert cond, msg
def no_raise(fn):  # True iff fn() does NOT raise AssertionError (boundary rows, A10)
    try:
        fn()
        return True
    except AssertionError:
        return False
def R(): return copy.deepcopy(GOOD_RECORD)
def D(): return copy.deepcopy(GOOD_DERIVED)
def CB(): return copy.deepcopy(GOOD_COMBINED)
def C(): return copy.deepcopy(GOOD_CONFIG)
def setk(d, path, v):
    cur = d
    for k in path[:-1]:
        cur = cur[k]
    cur[path[-1]] = v
    return d
def delk(d, path):
    cur = d
    for k in path[:-1]:
        cur = cur[k]
    del cur[path[-1]]
    return d


ROWS = [
    # ---- validate_record: measured rows (one field each) ----
    ("P1",  "record is not a JSON object",                 lambda: j.validate_record(["not", "a", "dict"])),
    ("P2",  "missing top-level key 'bytes'",               lambda: j.validate_record(delk(R(), ["bytes"]))),
    ("P3",  "unknown top-level key 'measured'",            lambda: j.validate_record(setk(R(), ["measured"], False))),
    ("P4",  "ts must be an iso8601 string",                lambda: j.validate_record(setk(R(), ["ts"], "noon"))),
    ("P5",  "env 'COLIMA' not in",                         lambda: j.validate_record(setk(R(), ["env"], "COLIMA"))),
    ("P6",  "scenario 'S1-calib' not in",                  lambda: j.validate_record(setk(R(), ["scenario"], "S1-calib"))),
    ("P7",  "config keys must be exactly",                 lambda: j.validate_record(setk(R(), ["config", "extra"], 1))),
    ("P8",  "config.n must be a positive int",             lambda: j.validate_record(setk(R(), ["config", "n"], 0))),
    ("P9",  "config.u must be a non-negative int",         lambda: j.validate_record(setk(R(), ["config", "u"], -1))),
    ("P10", "config.network 'WAN100' not in",              lambda: j.validate_record(setk(R(), ["config", "network"], "WAN100"))),
    ("P11", "config.protocol must be a non-empty string",  lambda: j.validate_record(setk(R(), ["config", "protocol"], ""))),
    ("P12", "config.variant must be a non-empty string",   lambda: j.validate_record(setk(R(), ["config", "variant"], 7))),
    ("P13", "trial must be a non-negative int",            lambda: j.validate_record(setk(R(), ["trial"], -1))),
    ("P14", "seed must be a non-negative int",             lambda: j.validate_record(setk(R(), ["seed"], "0"))),
    ("P15", "status 'crashed' not in",                     lambda: j.validate_record(setk(R(), ["status"], "crashed"))),
    ("P16", "time_s keys must be exactly",                 lambda: j.validate_record(delk(R(), ["time_s", "offline"]))),
    ("P17", "time_s.online must be a number",              lambda: j.validate_record(setk(R(), ["time_s", "online"], "fast"))),
    ("P18", "time_s.total must be >= 0",                   lambda: j.validate_record(setk(R(), ["time_s", "total"], -1.0))),
    ("P19", "bytes keys must be exactly",                  lambda: j.validate_record(setk(R(), ["bytes", "rx"], 1))),
    ("P20", "bytes.r_out must be a non-negative int",      lambda: j.validate_record(setk(R(), ["bytes", "r_out"], -5))),
    ("P21", "bytes.total 999 != r_out+s_out 1500",         lambda: j.validate_record(setk(R(), ["bytes", "total"], 999))),
    ("P22", "bytes.external_total 1499 != bytes.total 1500", lambda: j.validate_record(setk(R(), ["bytes", "external_total"], 1499))),
    ("P23", "counters keys must be exactly",               lambda: j.validate_record(delk(R(), ["counters", "rounds"]))),
    ("P24", "counters.triples must be a non-negative int", lambda: j.validate_record(setk(R(), ["counters", "triples"], 1.5))),
    ("P25", "notes must be a string",                      lambda: j.validate_record(setk(R(), ["notes"], ["a"]))),
    # ---- validate_record: DERIVED rows (A1/A3) ----
    ("P26", "missing top-level key 'source'",              lambda: j.validate_record(delk(D(), ["source"]))),
    ("P27", "verbatim network label",                      lambda: j.validate_record(setk(D(), ["config", "network"], "WAN200"))),
    ("P28", "verbatim network label",                      lambda: j.validate_record(setk(D(), ["config", "network"], "200Mbps"))),
    ("P29", "r_out and s_out must be 0",                   lambda: j.validate_record(setk(D(), ["bytes", "r_out"], 5))),
    ("P30", "total == external_total == published",        lambda: j.validate_record(setk(D(), ["bytes", "external_total"], 1))),
    ("P31", "requires 'measured': false",                  lambda: j.validate_record(setk(D(), ["measured"], True))),
    ("P32", "requires source 'ACNS26-Tables3-4'",          lambda: j.validate_record(setk(D(), ["source"], "x"))),
    ("P33", "scenario 'derived-T3' not in",                lambda: j.validate_record(setk(D(), ["scenario"], "derived-T3"))),
    # ---- validate_record: combined-loopback rows (A10, Task 34) ----
    ("B1",  "r_out and s_out must be 0 (A10)",              lambda: j.validate_record(setk(CB(), ["bytes", "r_out"], 5))),
    ("B2",  "total == external_total (A10)",                lambda: j.validate_record(setk(CB(), ["bytes", "external_total"], 1))),
    # I3 (Codex review, Task 34 fix round): validate_record enforces the two-token bridge
    # at the file level too (build_record already did; the FILE validator did not).
    ("B7",  "must be present together (A10)",                lambda: j.validate_record(setk(CB(), ["notes"], CB()["notes"].replace(";combined_total_b=45700000", "")))),
    ("B8",  "must be present together (A10)",                lambda: j.validate_record(setk(R(), ["notes"], R()["notes"] + ";combined_total_b=999"))),
    ("B9",  "must equal bytes.total 45700000 (A10)",         lambda: j.validate_record(setk(CB(), ["notes"], CB()["notes"].replace("combined_total_b=45700000", "combined_total_b=1")))),
    # ---- validate_record / check_regime_consistency: I3 protocol scoping (gate fix round 1, Task 6) ----
    ("B10", "combined-loopback regime is granted to bms24 only (A10); protocol=fastupsi",
            lambda: j.validate_record(setk(CB(), ["config", "protocol"], "fastupsi"))),
    ("B11", "mixed byte regimes for protocol bms24 in one file",
            lambda: j.check_regime_consistency([CB(), setk(R(), ["config", "protocol"], "bms24")])),
    ("B12", "bms24-combined + fastupsi-two-sided in one file must be ACCEPTED: the inverse claim fails",
            lambda: must(no_raise(lambda: j.check_regime_consistency([CB(), setk(R(), ["config", "protocol"], "fastupsi")])) is False,
                         "bms24-combined + fastupsi-two-sided in one file must be ACCEPTED: the inverse claim fails")),
    # ---- build_record: I2 combined-loopback byte-divergence (gate fix round 1, Task 5) ----
    ("B13", "combined-scope 6% must flag bytes-divergence: the inverse claim fails",
            lambda: must("bytes-divergence" not in j.build_record(dict(GOOD_CONFIG_BMS24), "AWS", "ok", dict(SEG), 0, 0, {"r": OBS_OK, "s": OBS_OK}, ["bytes=combined-loopback", "combined_total_b=100", "internal_comm_b=106"], 0)["notes"],
                         "combined-scope 6% must flag bytes-divergence: the inverse claim fails")),
    ("B14", "combined-scope exactly 5% must NOT flag bytes-divergence: the inverse claim fails",
            lambda: must("bytes-divergence" in j.build_record(dict(GOOD_CONFIG_BMS24), "AWS", "ok", dict(SEG), 0, 0, {"r": OBS_OK, "s": OBS_OK}, ["bytes=combined-loopback", "combined_total_b=100", "internal_comm_b=105"], 0)["notes"],
                         "combined-scope exactly 5% must NOT flag bytes-divergence: the inverse claim fails")),
    ("B15", "directional both-present summed 6% must flag bytes-divergence: the inverse claim fails",
            lambda: must("bytes-divergence" not in j.build_record(dict(GOOD_CONFIG_BMS24), "AWS", "ok", dict(SEG), 0, 0, {"r": OBS_OK, "s": OBS_OK}, ["bytes=combined-loopback", "combined_total_b=100", "internal_b_r=53", "internal_b_s=53"], 0)["notes"],
                         "directional both-present summed 6% must flag bytes-divergence: the inverse claim fails")),
    ("B16", "directional only-one-present must be unavailable(scope-mismatch), not compared: the inverse claim fails",
            lambda: must("internal-comparison=unavailable(scope-mismatch)" not in j.build_record(dict(GOOD_CONFIG_BMS24), "AWS", "ok", dict(SEG), 0, 0, {"r": OBS_OK, "s": OBS_OK}, ["bytes=combined-loopback", "combined_total_b=100", "internal_b_r=53"], 0)["notes"],
                         "directional only-one-present must be unavailable(scope-mismatch), not compared: the inverse claim fails")),
    # ---- check_counts (HSTx3) ----
    ("K1",  "duplicate record for",                        lambda: j.check_counts([R(), R()], [GOOD_KEY], 1)),
    ("K2",  "has trials [0], wanted 0..1",                 lambda: j.check_counts([R()], [GOOD_KEY], 2)),
    ("K3",  "not allowed by HSTx3",                        lambda: j.check_counts([setk(R(), ["status"], "error")], [GOOD_KEY], 1)),
    ("K4",  "unexpected cell",                             lambda: j.check_counts([setk(R(), ["config", "u"], 65)], [GOOD_KEY], 1)),
    # ---- parse_segments ----
    ("S1",  "marker total_workload_end absent",            lambda: j.parse_segments(GOOD_MARKERS.replace("PHASE:total_workload_end:9300\n", ""))),
    ("S2",  "appears more than once",                      lambda: j.parse_segments(GOOD_MARKERS + "PHASE:online_end:9250\n")),
    ("S3",  "markers not monotonic",                       lambda: j.parse_segments(GOOD_MARKERS.replace("online_begin:4200", "online_begin:9250"))),
    ("S4",  "not monotonic: setup_once_end (1200) < setup_once_begin (1300)", lambda: j.parse_segments(GOOD_MARKERS.replace("setup_once_begin:1100", "setup_once_begin:1300"))),
    # ---- complete_segments / build_record synthesis guards ----
    ("G1",  "kill_ns 500 precedes total_workload_begin 1000",      lambda: j.complete_segments(j.parse_segments(GOOD_MARKERS.replace("PHASE:online_end:9200\n", "").replace("PHASE:total_workload_end:9300\n", ""), allow_incomplete=True), 500)),
    ("G2",  "synthesized segments require status timeout|error",
            lambda: j.build_record(C(), "LOCAL", "ok", j.complete_segments(j.parse_segments(GOOD_MARKERS.replace("PHASE:online_end:9200\n", "").replace("PHASE:total_workload_end:9300\n", ""), allow_incomplete=True), 9999), 1, 1, {"r": OBS_OK, "s": OBS_OK}, [], 0)),
    # ---- divergence_flag (strict > 5%) ----
    ("V1",  "6% must flag",                                lambda: must(j.divergence_flag(106, 100) is False, "6% must flag (strict >5%): the inverse claim fails")),
    ("V2",  "exactly 5% must NOT flag",                    lambda: must(j.divergence_flag(105, 100) is True, "exactly 5% must NOT flag (strict >5%): the inverse claim fails")),
    # ---- observation objects + build_record thread policy (A2+A5+A6) ----
    ("T1",  "observations must have exactly sides r and s", lambda: j.validate_observations({"r": OBS_OK})),
    ("T2",  "observation keys must be exactly state/threads_max/rss_kb", lambda: j.validate_observations({"r": OBS_OK, "s": {"state": "observed", "threads_max": 1}})),
    ("T3",  "observation state 'aborted' not in observed|unsampled", lambda: j.validate_observations({"r": OBS_OK, "s": {"state": "aborted", "threads_max": 1, "rss_kb": 1}})),
    ("T4",  "observed side s must carry threads_max >= 1",   lambda: j.validate_observations({"r": OBS_OK, "s": {"state": "observed", "threads_max": None, "rss_kb": 1}})),
    ("T5",  "unsampled side s must carry null threads_max and rss_kb", lambda: j.validate_observations({"r": OBS_OK, "s": {"state": "unsampled", "threads_max": 1, "rss_kb": None}})),
    ("T6",  "rss_kb must be a non-negative int",             lambda: j.validate_observations({"r": OBS_OK, "s": {"state": "observed", "threads_max": 1, "rss_kb": -1}})),
    ("T7",  "status 'error' != required 'ok'",               lambda: j.accept_record(j.build_record(C(), "LOCAL", "ok", SEG, 1, 1, {"r": OBS_OK, "s": OBS_2T}, [], 0))),
    ("T8",  "required notes token 'threads_violation=s:2' absent", lambda: j.accept_record(j.build_record(C(), "LOCAL", "ok", SEG, 1, 1, {"r": OBS_OK, "s": OBS_OK}, [], 0), expect=["threads_violation=s:2"])),
    ("T9",  "required notes token 'party-pid-unsampled=s' absent", lambda: j.accept_record(j.build_record(C(), "LOCAL", "ok", SEG, 1, 1, {"r": OBS_OK, "s": OBS_OK}, [], 0), expect=["party-pid-unsampled=s"])),
    ("T10", "required flag 'threads>1:runtime-helpers' absent", lambda: j.accept_record(j.build_record(setk(C(), ["thread_regime"], "runtime-helpers"), "LOCAL", "ok", SEG, 1, 1, {"r": OBS_OK, "s": OBS_OK}, [], 0), flags=["threads>1:runtime-helpers"])),
    ("T11", "Threads line regex must not match a malformed line", lambda: must(j.THREADS_RE.match("Threads: nine") is not None, "Threads line regex must not match a malformed line: the inverse claim fails")),
    ("T12", "VmHWM line regex must not match a malformed line",   lambda: must(j.VMHWM_RE.match("VmHWM:  123 MB") is not None, "VmHWM line regex must not match a malformed line: the inverse claim fails")),
    # ---- build_record: combined-loopback byte regime (A10, Task 34) ----
    ("B3",  "must be present together (A10)",                 lambda: j.build_record(dict(GOOD_CONFIG_BMS24), "LOCAL", "ok", SEG, 0, 0, {"r": OBS_OK, "s": OBS_OK}, ["bytes=combined-loopback"], 0)),
    ("B4",  "must be present together (A10)",                 lambda: j.build_record(dict(GOOD_CONFIG_BMS24), "LOCAL", "ok", SEG, 0, 0, {"r": OBS_OK, "s": OBS_OK}, ["combined_total_b=100"], 0)),
    # B5/B5b: the calibration-noise ceiling (VETH_CALIBRATION_NOISE_MAX_B=8192) is a
    # BOUNDARY, demonstrated on both sides -- one value just above it (must fail) and
    # one value AT it (must NOT fail); a one-sided negative would not show the
    # boundary is where the assertion message says it is.
    ("B5",  "r_out+s_out <= 8192 B",                          lambda: j.build_record(dict(GOOD_CONFIG_BMS24), "LOCAL", "ok", SEG, 8193, 0, {"r": OBS_OK, "s": OBS_OK}, ["bytes=combined-loopback", "combined_total_b=100"], 0)),
    ("B5b", "8192 B (the ceiling) must NOT raise: the inverse claim fails",
            lambda: must(no_raise(lambda: j.build_record(dict(GOOD_CONFIG_BMS24), "LOCAL", "ok", SEG, 8192, 0, {"r": OBS_OK, "s": OBS_OK}, ["bytes=combined-loopback", "combined_total_b=100"], 0)) is False,
                         "8192 B (the ceiling) must NOT raise: the inverse claim fails")),
    ("B6",  "bytes.total > 0 (A10)",                          lambda: j.build_record(dict(GOOD_CONFIG_BMS24), "LOCAL", "ok", SEG, 0, 0, {"r": OBS_OK, "s": OBS_OK}, ["bytes=combined-loopback", "combined_total_b=0"], 0)),
    # ---- accept_record (executable smoke/calibration gates) ----
    ("A1",  "status 'timeout' != required 'ok'",            lambda: j.accept_record(setk(R(), ["status"], "timeout"))),
    ("A2",  "required notes token 'result=65504' absent",   lambda: j.accept_record(R(), expect=["result=65504"])),
    ("A3",  "required flag 'threads>1:runtime-helpers' absent", lambda: j.accept_record(R(), flags=["threads>1:runtime-helpers"])),
    ("A4",  "bytes.total 1500 outside [41130000, 50270000]", lambda: j.accept_record(R(), bytes_within=(41130000, 50270000))),
    ("A5",  "time_s.online 12.5 outside [11.6, 12.0]",      lambda: j.accept_record(R(), online_within=(11.6, 12.0))),
    ("A6",  "time_s.total 15.75 outside [0, 1]",            lambda: j.accept_record(R(), total_within=(0, 1))),
    ("A7",  "time_s.total 1.0 < time_s.online 12.5",        lambda: j.accept_record(setk(R(), ["time_s", "total"], 1.0))),
    ("A8",  "bytes.r_out 1000 < 2000",                      lambda: j.accept_record(R(), min_r_out=2000)),
    ("A9",  "bytes.s_out 500 < 501",                        lambda: j.accept_record(R(), min_s_out=501)),
    ("A10", "no notes token matches /result=[0-9]+/",       lambda: j.accept_record(setk(R(), ["notes"], GOOD_RECORD["notes"].replace("result=32", "result=na")), expect_re=["result=[0-9]+"])),
    # ---- validate_config ----
    ("C1",  "config is not a JSON object",                 lambda: j.validate_config([])),
    ("C2",  "protocol 'psi-magic' not one of",             lambda: j.validate_config(setk(C(), ["protocol"], "psi-magic"))),
    ("C3",  "missing key 'budget_s'",                      lambda: j.validate_config(delk(C(), ["budget_s"]))),
    ("C4",  "unknown key 'extra'",                         lambda: j.validate_config(setk(C(), ["extra"], 1))),
    ("C5",  "key 'budget_s' must be int",                  lambda: j.validate_config(setk(C(), ["budget_s"], "60"))),
    ("C6",  "scenario 'S1-calib' not in",                  lambda: j.validate_config(setk(C(), ["scenario"], "S1-calib"))),
    ("C7",  "network 'WAN100' not in",                     lambda: j.validate_config(setk(C(), ["network"], "WAN100"))),
    ("C8",  "exec_mode must be docker|native",             lambda: j.validate_config(setk(C(), ["exec_mode"], "podman"))),
    ("C9",  "thread_regime must be strict|runtime-helpers", lambda: j.validate_config(setk(C(), ["thread_regime"], "lenient"))),
    ("C10", "queries must be >= 1",                        lambda: j.validate_config(setk(C(), ["queries"], 0))),
    ("C11", "budget_s must be >= 1",                       lambda: j.validate_config(setk(C(), ["budget_s"], 0))),
    ("C12", "docker image must be a registry digest ref",  lambda: j.validate_config(setk(copy.deepcopy(GOOD_CONFIG_DOCKER), ["image"], "sympsica/volepsi:ec76012"))),   # ONE field: the base fixture is already a valid docker config
]


def main(argv):
    only = None
    if len(argv) > 2 and argv[1] == "--only":
        only = argv[2]
    bad = 0
    for rid, expect, fn in ROWS:
        if only and rid != only:
            continue
        try:
            fn()
            print("%s -> NO FAILURE (expected: %s)" % (rid, expect))
            bad += 1
        except AssertionError as e:
            msg = str(e)
            ok = expect in msg
            print("%s -> %s%s" % (rid, "AssertionError: " + msg, "" if ok else "   [UNEXPECTED TEXT; wanted: %s]" % expect))
            bad += 0 if ok else 1
        except Exception as e:  # a non-AssertionError is NOT the pinned failure
            print("%s -> WRONG EXCEPTION %s: %s (expected AssertionError: %s)" % (rid, type(e).__name__, e, expect))
            bad += 1
    print("r6_matrix: %d rows, %d not as pinned" % (len([r for r in ROWS if not only or r[0] == only]), bad))
    return 2 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

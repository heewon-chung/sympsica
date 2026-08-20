#!/usr/bin/env python3
"""test/e2e/run_e2e_gate.py — Task 19's shared two-OS-process E2E driver
(task-19-brief.md W5.8), used in TWO modes:

  * SC2 [E2E-1..4]: normal mode (default) -- launches the party binary
    TWICE (real subprocesses, 127.0.0.1 loopback), asserts BOTH parties
    agree with each other AND with ref/reference.py's Ref.simulate_days()
    at EVERY query day, at every seed-fixed schedule (n ~ 2^10/party).

  * FC1/FC2/FC5 [TV-F8/TV-F9-at-E2E-scale/stale-sizes]: `--expect-mismatch`
    mode -- run against a party binary built from a WRONG-CONSTRUCTION
    build dir (R-FCFLAGS: -DSYMPSICA_WRONG_SIGN / -DSYMPSICA_WRONG_CONVERT /
    -DSYMPSICA_STALE_SIZES, second build dirs, SYMPSICA_NO_FILTER
    precedent) and assert that AT LEAST ONE query day's count DISAGREES
    with reference.py -- "a wrong construction that silently passes is
    itself a suite failure" (task-19-brief.md R-FCFLAGS quoting the TV-F1
    discipline), so this script's own exit code FAILS if no mismatch is
    observed.

Stdlib only, same shape as run_smoke.py: hard timeout, fresh workdir,
distinct --out paths per run (party_main.cpp's own --out truncates --
task-19-brief.md carried item (b): this driver's deliberate choice is to
NEVER re-invoke party_main against a pre-existing --out path, so trunc-vs-
append is moot here; documented in task-19-report.md).
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time
from types import ModuleType


def load_reference(path: str) -> ModuleType:
    spec = importlib.util.spec_from_file_location("sympsica_reference", path)
    assert spec is not None and spec.loader is not None, f"could not load reference.py from {path}"
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def read_jsonl(path: str) -> list[dict]:
    records = []
    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            records.append(json.loads(line))
    return records


def extract_u64(stderr_text: str, key: str) -> int | None:
    """task-23-brief.md R6-CGB-PROMOTE: pull a `<key><digits>` line's value
    out of a party's captured stderr (apps/party_main.cpp emits
    `pkop_counter_after_setup=<N>` right after Setup::run and
    `pkop_counter=<N>` at process exit -- two DISTINCT literal keys, neither
    a prefix collision of the other since `pkop_counter=` requires the `=`
    immediately after `pkop_counter`, which `pkop_counter_after_setup=`
    never has)."""
    m = re.search(re.escape(key) + r"(\d+)", stderr_text)
    return int(m.group(1)) if m else None


def audit_pool(party_name: str, pool_name: str, data: dict, next_corr_id: int) -> dict:
    """task-23-brief.md W6.4/W6.6(ii): R6-AUDIT-TRANSCRIPT's three
    INDEPENDENTLY-CHECKED properties, at E2E scale, against `data` -- a raw
    {"generated": int, "remaining": int, "consumed_ids": [int, ...]} dict
    read straight from apps/party_main.cpp's --audit-out JSON dump (the
    SAME raw facts test/protocols/kat_pool_audit.cpp's SC1 audits
    in-process, via test/utils/audit_pool_transcript.hpp's C++ twin of this
    function).

    Property (4) ("ids stay globally unique across refills") is NOT checked
    here and cannot be, from this data alone: (2) only inspects the
    CONSUMED subset, so a duplicate corr_id confined to items that were
    generated but NEVER consumed (legal high-water stock, W3.4) would be
    structurally invisible to it -- the full generated-id SET is not part
    of this dump (party_main.cpp's audit_json only emits generated() as a
    plain count, matching CorrelationPool<T>'s own public API, which has no
    accessor for that set). Property (4) is instead enforced as a
    PRECONDITION, at insertion time, by CorrelationPool<T>::refill itself
    (src/core/pools.cpp): every incoming corr_id is checked against both
    the available and already-consumed sets, in the same loop that inserts
    it, so a duplicate -- within one refill() batch, across two calls, or
    against an already-consumed id -- aborts the process before it is ever
    added. This function (and the whole E2E run reaching this point) RELIES
    ON that enforcement rather than re-verifying it: see
    test/utils/audit_pool_transcript.hpp's header for the full argument."""
    generated = data["generated"]
    consumed = data["consumed_ids"]
    remaining = data["remaining"]

    # (1) reconciliation.
    accounted = len(consumed) + remaining
    assert generated == accounted, (
        f"{party_name}/{pool_name}: reconciliation failed: generated={generated} "
        f"consumed={len(consumed)} remaining={remaining} (consumed+remaining={accounted})"
    )

    # (2) no duplicates among CONSUMED ids across the whole transcript
    # (every refill). NOT property (4) -- see this function's own
    # docstring: this only covers the consumed subset.
    seen = set(consumed)
    assert len(seen) == len(consumed), (
        f"{party_name}/{pool_name}: duplicate consumed id(s): {len(consumed)} entries, "
        f"{len(seen)} distinct"
    )

    # (3) no id from nowhere.
    bad = [i for i in consumed if i >= next_corr_id]
    assert not bad, (
        f"{party_name}/{pool_name}: consumed id(s) >= next_corr_id={next_corr_id}: {bad[:5]}"
    )

    return {
        "generated": generated,
        "consumed": len(consumed),
        "remaining": remaining,
        "highest_consumed_id": max(consumed) if consumed else None,
        "next_corr_id": next_corr_id,
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(prog="run_e2e_gate.py")
    parser.add_argument("--party-bin", required=True)
    parser.add_argument("--reference", required=True)
    parser.add_argument("--schedule-r", required=True)
    parser.add_argument("--schedule-s", required=True)
    parser.add_argument("--workdir", default=None)
    parser.add_argument("--timeout-s", type=float, default=900.0)
    parser.add_argument("--expect-mismatch", action="store_true",
                         help="FC mode: PASS iff at least one query day disagrees with reference.py")
    parser.add_argument("--require-asymmetric", action="store_true",
                         help="FC1: assert the schedule pair is a genuinely asymmetric set "
                              "(|A\\B| != |B\\A|) before running anything")
    parser.add_argument("--expect-crash-stderr-substring", default=None,
                         help="task-20-brief.md M2: when --expect-mismatch AND at least one party "
                              "exits nonzero, this substring must appear in a party's stderr for the "
                              "crash to be POSITIVELY identified as the expected wrong-construction "
                              "failure. Without this flag, a nonzero exit under --expect-mismatch is "
                              "now a hard FAILURE (previously ANY nonzero exit silently read as "
                              "\"the negative fired\", which would also accept an unrelated crash -- "
                              "a missing file, a bad argv -- as success)")
    args = parser.parse_args(argv)

    ref = load_reference(args.reference)

    with open(args.schedule_r) as f:
        schedule_r = json.load(f)
    with open(args.schedule_s) as f:
        schedule_s = json.load(f)

    if args.require_asymmetric:
        ids_r: set[int] = set()
        ids_s: set[int] = set()
        for day in schedule_r:
            ids_r |= set(day["insert"])
            ids_r -= set(day["delete"])
        for day in schedule_s:
            ids_s |= set(day["insert"])
            ids_s -= set(day["delete"])
        a_minus_b = len(ids_r - ids_s)
        b_minus_a = len(ids_s - ids_r)
        assert a_minus_b != b_minus_a, (
            f"--require-asymmetric: schedule pair is NOT asymmetric (|A\\B|={a_minus_b} == "
            f"|B\\A|={b_minus_a}) -- FC1 needs a genuinely asymmetric set pair"
        )
        print(f"[run_e2e_gate] asymmetry OK: |A\\B|={a_minus_b}, |B\\A|={b_minus_a}")

    expected_counts = ref.Ref.simulate_days(schedule_r, schedule_s)
    print(f"[run_e2e_gate] expected counts (reference.py simulate_days): {expected_counts}")

    # R6-PATHASSERT (task-22-brief.md): the expected `path` label for every
    # query day, from the SAME single source of truth as expected_counts --
    # ref/reference.py's Ref.expected_paths(), a Python replica of
    # src/protocols/query.cpp's SwitchRule::decide (R-EXPECTSRC).
    expected_paths = ref.Ref.expected_paths(schedule_r, schedule_s)
    print(f"[run_e2e_gate] expected paths (reference.py expected_paths): {expected_paths}")
    assert len(expected_paths) == len(expected_counts), (
        f"expected_paths ({len(expected_paths)} labels) and simulate_days "
        f"({len(expected_counts)} counts) disagree on query-day count -- internal reference.py bug"
    )

    if args.workdir is not None and os.path.isdir(args.workdir):
        import shutil
        shutil.rmtree(args.workdir)
    workdir = args.workdir or tempfile.mkdtemp(prefix="sympsica_e2e_")
    os.makedirs(workdir, exist_ok=True)
    state_r = os.path.join(workdir, "state_r")
    state_s = os.path.join(workdir, "state_s")
    out_r = os.path.join(workdir, "out_r.jsonl")
    out_s = os.path.join(workdir, "out_s.jsonl")
    stderr_r_path = os.path.join(workdir, "stderr_r.log")
    stderr_s_path = os.path.join(workdir, "stderr_s.log")
    # task-23-brief.md R6-AUDIT-SCALE: always requested (party_main.cpp's
    # --audit-out is a no-op file write, never a business-logic assertion --
    # see that flag's own comment); only READ/asserted in the normal
    # (non-expect-mismatch) success path below, same discipline as every
    # other SC/FC check this driver makes.
    audit_r_path = os.path.join(workdir, "audit_r.json")
    audit_s_path = os.path.join(workdir, "audit_s.json")

    port = find_free_port()
    receiver_cmd = [
        args.party_bin, "--role", "r", "--listen", str(port),
        "--schedule", args.schedule_r, "--state", state_r, "--out", out_r, "--seed", "41001",
        "--audit-out", audit_r_path,
    ]
    sender_cmd = [
        args.party_bin, "--role", "s", "--connect", f"127.0.0.1:{port}",
        "--schedule", args.schedule_s, "--state", state_s, "--out", out_s, "--seed", "42002",
        "--audit-out", audit_s_path,
    ]

    print(f"[run_e2e_gate] receiver: {' '.join(receiver_cmd)}")
    print(f"[run_e2e_gate] sender:   {' '.join(sender_cmd)}")

    t0 = time.monotonic()
    with open(stderr_r_path, "wb") as err_r, open(stderr_s_path, "wb") as err_s:
        proc_r = subprocess.Popen(receiver_cmd, stdout=subprocess.DEVNULL, stderr=err_r)
        proc_s = subprocess.Popen(sender_cmd, stdout=subprocess.DEVNULL, stderr=err_s)

        deadline = time.monotonic() + args.timeout_s
        procs = {"receiver": proc_r, "sender": proc_s}
        exit_codes: dict[str, int | None] = {"receiver": None, "sender": None}
        while time.monotonic() < deadline and any(c is None for c in exit_codes.values()):
            for name, p in procs.items():
                if exit_codes[name] is None:
                    rc = p.poll()
                    if rc is not None:
                        exit_codes[name] = rc
            if any(c is None for c in exit_codes.values()):
                time.sleep(0.2)

        timed_out = any(c is None for c in exit_codes.values())
        if timed_out:
            for name, p in procs.items():
                if exit_codes[name] is None:
                    p.kill()
                    p.wait()
            print(f"[run_e2e_gate] FAIL: timed out after {args.timeout_s}s; exit_codes so far="
                  f"{exit_codes}", file=sys.stderr)
            return 1

    elapsed = time.monotonic() - t0
    print(f"[run_e2e_gate] both parties exited after {elapsed:.1f}s: {exit_codes}")

    # Always read both parties' stderr (task-23-brief.md R6-CGB-PROMOTE
    # needs it in the NORMAL success path too, not just the expect-mismatch
    # crash-identification path below, which already read it here).
    with open(stderr_r_path, "rb") as f:
        stderr_r = f.read().decode("utf-8", "replace")
    with open(stderr_s_path, "rb") as f:
        stderr_s = f.read().decode("utf-8", "replace")

    if args.expect_mismatch and (exit_codes["receiver"] != 0 or exit_codes["sender"] != 0):
        # A wrong construction is allowed to manifest as a hard protocol
        # abort/crash instead of a graceful wrong count -- e.g. FC5's
        # stale-sizes build trips a wire-level buffer-size desync (coproto
        # cancels the connection) rather than reaching JSONL output at all.
        # This is STILL "observed failing" (R-FCFLAGS/TV-F1 discipline: the
        # wrong construction must not silently pass) -- arguably a more
        # dramatic demonstration than a silent miscount. See
        # task-19-report.md for this run's own captured stderr.
        #
        # M2 (task-20-brief.md, carried minor from the Phase-5 gate review):
        # a bare nonzero exit is NOT enough -- an unrelated crash (a missing
        # file, a bad argv, a segfault in something this negative never
        # touches) would ALSO read as "the negative fired" under the old
        # unconditional `return 0` here. Positively identify the crash via a
        # required stderr substring instead. (stderr_r/stderr_s already read
        # above, unconditionally.)
        if args.expect_crash_stderr_substring is None:
            print(f"[run_e2e_gate] FAIL: nonzero exit under --expect-mismatch "
                  f"(exit_codes={exit_codes}) but no --expect-crash-stderr-substring was given to "
                  f"positively identify this as the EXPECTED wrong-construction crash -- an "
                  f"unrelated crash must not silently read as \"the negative fired\" "
                  f"(task-20-brief.md M2)", file=sys.stderr)
            print(f"[run_e2e_gate] receiver stderr tail: {stderr_r[-2000:]}", file=sys.stderr)
            print(f"[run_e2e_gate] sender stderr tail:   {stderr_s[-2000:]}", file=sys.stderr)
            return 1

        if args.expect_crash_stderr_substring in stderr_r or \
                args.expect_crash_stderr_substring in stderr_s:
            print(f"[run_e2e_gate] FC OK: wrong construction crashed at least one party "
                  f"(exit_codes={exit_codes}) with the EXPECTED signature "
                  f"'{args.expect_crash_stderr_substring}' -- this IS the identified failure")
            return 0

        print(f"[run_e2e_gate] FAIL: nonzero exit under --expect-mismatch (exit_codes={exit_codes}) "
              f"but neither party's stderr contains the expected signature "
              f"'{args.expect_crash_stderr_substring}' -- this crash did NOT match the expected "
              f"wrong-construction failure (an unrelated crash, per task-20-brief.md M2)",
              file=sys.stderr)
        print(f"[run_e2e_gate] receiver stderr tail: {stderr_r[-2000:]}", file=sys.stderr)
        print(f"[run_e2e_gate] sender stderr tail:   {stderr_s[-2000:]}", file=sys.stderr)
        return 1

    assert exit_codes["receiver"] == 0, f"receiver exited {exit_codes['receiver']} (expected 0)"
    assert exit_codes["sender"] == 0, f"sender exited {exit_codes['sender']} (expected 0)"

    records_r = read_jsonl(out_r)
    records_s = read_jsonl(out_s)
    print(f"[run_e2e_gate] receiver JSONL: {records_r}")
    print(f"[run_e2e_gate] sender JSONL:   {records_s}")

    assert len(records_r) == len(expected_counts), (
        f"receiver produced {len(records_r)} query records, expected {len(expected_counts)}"
    )
    assert len(records_s) == len(expected_counts)

    mismatch_days = []
    maintenance_days_seen = 0
    delete_bearing_query_days = sum(
        1 for d in schedule_r if d["query"] and not d["maintenance"] and len(d["delete"]) > 0
    )
    path_mismatch_days = []
    for idx, (rec_r, rec_s, expected, expected_path) in enumerate(
            zip(records_r, records_s, expected_counts, expected_paths)):
        assert rec_r["day"] == rec_s["day"], f"query {idx}: day mismatch"
        assert rec_r["count"] == rec_s["count"], (
            f"query {idx} ({rec_r['day']}): the two REAL parties disagree with EACH OTHER "
            f"(receiver={rec_r['count']} sender={rec_s['count']}) -- this is a protocol-level "
            f"desync, not the wrong-construction effect under test, and is always a hard failure "
            f"regardless of --expect-mismatch"
        )
        # R6-PATHASSERT: the two REAL parties must agree with EACH OTHER on
        # which path they took, unconditionally -- independent of
        # --expect-mismatch, same discipline as the count-desync check right
        # above (a path desync is the exact failure mode the Phase-5 FC4
        # negative exposed: SwitchRule::decide is symmetric by construction
        # via announce-bit propagation, so real parties disagreeing on path
        # is always a protocol-level bug, never an expected wrong-
        # construction effect).
        assert rec_r["path"] == rec_s["path"], (
            f"query {idx} ({rec_r['day']}): the two REAL parties disagree with EACH OTHER on "
            f"`path` (receiver={rec_r['path']!r} sender={rec_s['path']!r}) -- a path desync, "
            f"always a hard failure regardless of --expect-mismatch (R6-PATHASSERT)"
        )
        if rec_r["path"] == "maintenance_full":
            maintenance_days_seen += 1
        if rec_r["count"] != expected:
            mismatch_days.append((idx, rec_r["day"], rec_r["count"], expected))
        if not args.expect_mismatch and rec_r["path"] != expected_path:
            path_mismatch_days.append((idx, rec_r["day"], rec_r["path"], expected_path))

    if args.expect_mismatch:
        assert mismatch_days, (
            "FC mode: expected at least one query day to DISAGREE with reference.py under this "
            "wrong-construction build, but ALL counts matched -- the wrong construction silently "
            "passed, which is itself a suite failure (R-FCFLAGS/TV-F1 discipline)"
        )
        print(f"[run_e2e_gate] FC OK: wrong construction observed FAILING at {len(mismatch_days)} "
              f"query day(s): {mismatch_days}")
    else:
        assert not mismatch_days, f"SC2: count mismatch(es) vs reference.py: {mismatch_days}"
        # R6-PATHASSERT/SC3: every query day's observed `path` label must
        # match reference.py's Ref.expected_paths() -- the single source of
        # truth (R6-EXPECTSRC). FC1 demonstrates this assertion can actually
        # fail (see task-22-report.md).
        assert not path_mismatch_days, (
            f"SC3: path-label mismatch(es) vs reference.py Ref.expected_paths(): "
            f"{[(i, d, f'observed={o!r}', f'expected={e!r}') for i, d, o, e in path_mismatch_days]}"
        )
        assert maintenance_days_seen >= 1, "SC2/SC5: expected >= 1 salt-refresh (maintenance) query day"
        assert delete_bearing_query_days >= 1, "SC2: expected >= 1 delete-bearing normal query day"
        print(f"[run_e2e_gate] SC2 OK: {len(expected_counts)} query days, all counts agree "
              f"(receiver == sender == reference.py), {maintenance_days_seen} maintenance-day "
              f"event(s), {delete_bearing_query_days} delete-bearing normal query day(s).")
        print(f"[run_e2e_gate] SC3 OK: all {len(expected_paths)} query-day `path` labels agree "
              f"(receiver == sender == reference.py Ref.expected_paths()): {expected_paths}")

        # ---------------------------------------------------------------
        # task-23-brief.md SC3[CGB-E2E]/SC4[CGB-NONVACUOUS]: R6-CGB-PROMOTE.
        # (These SC numbers are task-23-brief.md's own -- distinct from this
        # script's pre-existing SC2/SC3 labels above, which are task-19-
        # brief.md/task-22-brief.md's numbering.)
        # ---------------------------------------------------------------
        snap_r = extract_u64(stderr_r, "pkop_counter_after_setup=")
        final_r = extract_u64(stderr_r, "pkop_counter=")
        snap_s = extract_u64(stderr_s, "pkop_counter_after_setup=")
        final_s = extract_u64(stderr_s, "pkop_counter=")
        assert snap_r is not None and final_r is not None, (
            "SC3[CGB-E2E]: receiver stderr is missing a pkop_counter_after_setup=/pkop_counter= line"
        )
        assert snap_s is not None and final_s is not None, (
            "SC3[CGB-E2E]: sender stderr is missing a pkop_counter_after_setup=/pkop_counter= line"
        )
        # SC4[CGB-NONVACUOUS]: nonzero -- a counter stuck at 0 would satisfy
        # "never increments" vacuously (Task-16's FC3 precedent).
        assert snap_r != 0, f"SC4[CGB-NONVACUOUS]: receiver PkOpCounter snapshot is ZERO (got {snap_r})"
        assert snap_s != 0, f"SC4[CGB-NONVACUOUS]: sender PkOpCounter snapshot is ZERO (got {snap_s})"
        # SC3[CGB-E2E]: snapshot == final, i.e. base OTs ran ONLY inside
        # Setup::run -- never again across every refill_offline call AND
        # the full multi-day schedule this seed just ran.
        assert snap_r == final_r, (
            f"SC3[CGB-E2E]: receiver PkOpCounter changed after Setup (snapshot={snap_r} final={final_r})"
        )
        assert snap_s == final_s, (
            f"SC3[CGB-E2E]: sender PkOpCounter changed after Setup (snapshot={snap_s} final={final_s})"
        )
        print(f"[run_e2e_gate] SC3[CGB-E2E]/SC4[CGB-NONVACUOUS] OK: PkOpCounter snapshot == final, "
              f"both nonzero (receiver snapshot={snap_r} final={final_r}; "
              f"sender snapshot={snap_s} final={final_s})")

        # ---------------------------------------------------------------
        # task-23-brief.md SC2[AUDIT-E2E]: R6-AUDIT-TRANSCRIPT's three
        # independently-checked properties, for both pools, on both
        # parties, at E2E scale, read from apps/party_main.cpp's
        # --audit-out dump (see audit_pool() above for the checks, why
        # property 4 is relied-on rather than re-checked here, and
        # R6-AUDIT-SCALE for why this dump exists).
        # ---------------------------------------------------------------
        with open(audit_r_path) as f:
            audit_r_data = json.load(f)
        with open(audit_s_path) as f:
            audit_s_data = json.load(f)

        audit_summary = {
            "receiver": {
                "triples": audit_pool("receiver", "triples", audit_r_data["triples"],
                                       audit_r_data["next_corr_id"]),
                "gates": audit_pool("receiver", "gates", audit_r_data["gates"],
                                     audit_r_data["next_corr_id"]),
            },
            "sender": {
                "triples": audit_pool("sender", "triples", audit_s_data["triples"],
                                       audit_s_data["next_corr_id"]),
                "gates": audit_pool("sender", "gates", audit_s_data["gates"],
                                     audit_s_data["next_corr_id"]),
            },
        }
        print(f"[run_e2e_gate] SC2[AUDIT-E2E] OK: R6-AUDIT-TRANSCRIPT's three independently-checked "
              f"properties hold for both pools on both parties (property 4, cross-refill global "
              f"uniqueness, is relied on via CorrelationPool::refill's own enforcement -- not "
              f"re-checked here, see audit_pool()'s docstring): {json.dumps(audit_summary)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

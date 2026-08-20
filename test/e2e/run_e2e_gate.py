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

    port = find_free_port()
    receiver_cmd = [
        args.party_bin, "--role", "r", "--listen", str(port),
        "--schedule", args.schedule_r, "--state", state_r, "--out", out_r, "--seed", "41001",
    ]
    sender_cmd = [
        args.party_bin, "--role", "s", "--connect", f"127.0.0.1:{port}",
        "--schedule", args.schedule_s, "--state", state_s, "--out", out_s, "--seed", "42002",
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
        # required stderr substring instead.
        with open(stderr_r_path, "rb") as f:
            stderr_r = f.read().decode("utf-8", "replace")
        with open(stderr_s_path, "rb") as f:
            stderr_s = f.read().decode("utf-8", "replace")

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

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

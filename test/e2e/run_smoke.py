#!/usr/bin/env python3
"""test/e2e/run_smoke.py — R-E2E-DRIVER (task-18-brief.md SC6): the two-OS-
process smoke E2E for apps/party_main.cpp. Stdlib only (no pytest/requests/
etc. -- this runs as a plain `add_test(COMMAND python3 run_smoke.py ...)`
ctest entry, task-18-brief.md requirement 1).

What it does:
  1. Reads a small, COMMITTED schedule pair from test/e2e/fixtures/
     (schedule_r.json / schedule_s.json -- ~49 ids across 6 days, 3 query
     days, 1 maintenance day; see that directory's own README for the
     hand-verified expected-count derivation).
  2. Computes the expected per-query-day intersection counts via
     ref/reference.py's Ref.simulate_days() (R-ORACLE-AGNOSTIC: pure set
     arithmetic, independent of the real BucketOracle).
  3. Launches the built `party` binary TWICE as separate OS processes
     (subprocess, distinct --state directories under one temp workdir,
     127.0.0.1 loopback, role r listens / role s connects) with a HARD
     timeout -- both processes are killed and the test FAILS on expiry,
     never hangs ctest.
  4. Parses both parties' JSONL output files and asserts:
       (a) both parties report the SAME count at every query day,
       (b) those counts match reference.py's simulate_days() exactly,
       (c) each party's stderr carries its JSON config-echo line.

CLI:
  --party-bin PATH     the built `party` executable (CMake:
                        $<TARGET_FILE:party>, never a hardcoded path)
  --reference PATH      path to ref/reference.py
  --fixtures-dir PATH   path to test/e2e/fixtures/
  --workdir PATH         optional; a fresh temp dir is used if omitted
  --timeout-s FLOAT      hard wall-clock budget for BOTH processes to exit
                          (default 90s -- generous but real, per
                          task-18-brief.md R-E2E-DRIVER)
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import shutil
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
    spec.loader.exec_module(module)  # runs reference.py's own import-time self-tests too
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
    parser = argparse.ArgumentParser(prog="run_smoke.py")
    parser.add_argument("--party-bin", required=True)
    parser.add_argument("--reference", required=True)
    parser.add_argument("--fixtures-dir", required=True)
    parser.add_argument("--workdir", default=None)
    parser.add_argument("--timeout-s", type=float, default=90.0)
    args = parser.parse_args(argv)

    ref = load_reference(args.reference)

    schedule_r_path = os.path.join(args.fixtures_dir, "schedule_r.json")
    schedule_s_path = os.path.join(args.fixtures_dir, "schedule_s.json")
    with open(schedule_r_path) as f:
        schedule_r = json.load(f)
    with open(schedule_s_path) as f:
        schedule_s = json.load(f)

    expected_counts = ref.Ref.simulate_days(schedule_r, schedule_s)
    print(f"[run_smoke] expected counts (reference.py simulate_days): {expected_counts}")

    # A fixed --workdir (e.g. the one CMakeLists.txt passes, under the build
    # dir) is REUSED across ctest invocations, not a fresh temp dir every
    # time -- so it must be wiped here, unconditionally, before this run's
    # own state files are written. A stale state.bin left behind by an
    # earlier (e.g. failed) run would otherwise be silently loaded by
    # party_main.cpp on this run, corrupting its results in a way that has
    # nothing to do with the code actually under test.
    if args.workdir is not None and os.path.isdir(args.workdir):
        import shutil
        shutil.rmtree(args.workdir)
    workdir = args.workdir or tempfile.mkdtemp(prefix="sympsica_smoke_")
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
        "--schedule", schedule_r_path, "--state", state_r, "--out", out_r, "--seed", "1001",
    ]
    sender_cmd = [
        args.party_bin, "--role", "s", "--connect", f"127.0.0.1:{port}",
        "--schedule", schedule_s_path, "--state", state_s, "--out", out_s, "--seed", "2002",
    ]

    print(f"[run_smoke] receiver: {' '.join(receiver_cmd)}")
    print(f"[run_smoke] sender:   {' '.join(sender_cmd)}")

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
                time.sleep(0.05)

        # Hard timeout: kill anything still alive and FAIL loudly rather
        # than let ctest hang (task-18-brief.md R-E2E-DRIVER).
        timed_out = any(c is None for c in exit_codes.values())
        if timed_out:
            for name, p in procs.items():
                if exit_codes[name] is None:
                    p.kill()
                    p.wait()
            print("[run_smoke] FAIL: timed out waiting for both party processes to exit "
                  f"(timeout-s={args.timeout_s}); killed. exit_codes so far={exit_codes}",
                  file=sys.stderr)
            return 1

    print(f"[run_smoke] exit codes: {exit_codes}")
    assert exit_codes["receiver"] == 0, f"receiver exited {exit_codes['receiver']} (expected 0)"
    assert exit_codes["sender"] == 0, f"sender exited {exit_codes['sender']} (expected 0)"

    # (c) config echo appeared on stderr: each party's stderr must carry its
    # own JSON config-echo line (party_main.cpp's config_json()).
    with open(stderr_r_path) as f:
        stderr_r_text = f.read()
    with open(stderr_s_path) as f:
        stderr_s_text = f.read()
    assert '"role":"r"' in stderr_r_text, "receiver stderr must contain its JSON config echo"
    assert '"role":"s"' in stderr_s_text, "sender stderr must contain its JSON config echo"
    assert "sympsica params:" in stderr_r_text, "receiver stderr must contain Params::echo's text dump"
    assert "sympsica params:" in stderr_s_text, "sender stderr must contain Params::echo's text dump"

    # (a)/(b): parse both JSONL outputs, compare per-query-day counts
    # against each other AND against reference.py's simulate_days().
    records_r = read_jsonl(out_r)
    records_s = read_jsonl(out_s)
    print(f"[run_smoke] receiver JSONL records: {records_r}")
    print(f"[run_smoke] sender JSONL records:   {records_s}")

    required_fields = {"day", "query_no", "path", "announce", "count", "online_bytes", "online_rounds",
                        "wall_s"}
    for label, records in (("receiver", records_r), ("sender", records_s)):
        for rec in records:
            missing = required_fields - set(rec.keys())
            assert not missing, f"{label} JSONL record missing fields {missing}: {rec}"

    assert len(records_r) == len(expected_counts), (
        f"receiver produced {len(records_r)} query records, expected {len(expected_counts)}"
    )
    assert len(records_s) == len(expected_counts), (
        f"sender produced {len(records_s)} query records, expected {len(expected_counts)}"
    )

    maintenance_days_seen = 0
    for idx, (rec_r, rec_s, expected) in enumerate(zip(records_r, records_s, expected_counts)):
        assert rec_r["day"] == rec_s["day"], f"query {idx}: day mismatch {rec_r['day']!r} vs {rec_s['day']!r}"
        assert rec_r["count"] == rec_s["count"], (
            f"query {idx} ({rec_r['day']}): count mismatch receiver={rec_r['count']} "
            f"sender={rec_s['count']}"
        )
        assert rec_r["count"] == expected, (
            f"query {idx} ({rec_r['day']}): count={rec_r['count']} != reference.py simulate_days "
            f"expected={expected}"
        )
        if rec_r["path"] == "maintenance_full":
            maintenance_days_seen += 1

    assert maintenance_days_seen >= 1, (
        "expected at least one maintenance-day query event (path == 'maintenance_full') in this "
        "fixture pair -- schedule_r.json/schedule_s.json's 2023-01-03 row"
    )

    print(f"[run_smoke] OK: {len(expected_counts)} query days, all counts agree "
          f"(receiver == sender == reference.py), {maintenance_days_seen} maintenance-day "
          "query event(s), config echo present on both sides.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

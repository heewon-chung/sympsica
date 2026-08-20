#!/usr/bin/env python3
"""test/e2e/run_cga_gate.py — task-24-brief.md W6.5 CG-A (SC1/SC2): the
zero-communication Update claim gate. Stdlib only (same idiom as
run_smoke.py/run_e2e_gate.py -- a plain `add_test(COMMAND python3 ...)`
ctest entry).

Runs the real `party --role r --update-only` binary against a 1000-EDIT
update schedule (R6-MASKSTAT-style spec fidelity: 1000 is the plan's own
count, not shrunk for convenience) as a SINGLE OS process -- NO counterpart
process is ever launched, and NO port is ever opened (this driver never
calls socket.socket() at all, unlike run_smoke.py/run_e2e_gate.py, which
both allocate a loopback port for the two real party processes they start).

Asserts:
  SC1 [CG-A-i]:  the process exits 0.
  SC2 [CG-A-ii], runtime leg: apps/party_main.cpp's --update-only mode
      prints `channel_construction_count=<N>` to stderr (Channel::
      construction_count(), utils/net.{hpp,cpp}) -- N must be EXACTLY 0.
      (SC2's STATIC leg -- the grep-guard -- is a separate ctest entry,
      core.GrepGuard_NoChannelInUpdateOnly; R6-CGA-RUNTIME requires BOTH.)

SC3 [CG-A-iii] (the EXTERNAL Linux-netns Tx-byte-delta leg) is NOT run by
this script -- macOS has no network namespaces (R6-NETNS); see
task-24-report.md for that leg's own evidence (or its DEFERRED-WITH-REASON
status), captured separately inside the colima x86_64 Linux VM.

CLI:
  --party-bin PATH   the built `party` executable ($<TARGET_FILE:party>)
  --workdir PATH      optional; a fresh temp dir is used if omitted
  --timeout-s FLOAT   hard wall-clock budget (default 60s -- an in-process,
                       no-network 1000-edit Update::apply loop is fast;
                       generous margin over that, not a measured bound)
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile


def build_1000_edit_schedule() -> list[dict]:
    """10 days, 100 distinct inserts/day, zero deletes, zero query/maintenance
    days anywhere -- 1000 total edits, all local (Update::apply only)."""
    days = []
    next_id = 1
    for d in range(10):
        ids = list(range(next_id, next_id + 100))
        next_id += 100
        days.append({
            "day": f"2024-01-{d + 1:02d}",
            "insert": ids,
            "delete": [],
            "query": False,
            "maintenance": False,
        })
    return days


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(prog="run_cga_gate.py")
    # --party-bin is required UNLESS --write-schedule-only is used (that mode
    # never invokes the binary at all).
    parser.add_argument("--party-bin", default=None)
    parser.add_argument("--workdir", default=None)
    parser.add_argument("--timeout-s", type=float, default=60.0)
    # task-24-brief.md CG-A(iii): the Linux-netns leg (test/e2e/
    # run_cga_netns_linux.sh, not runnable on macOS -- R6-NETNS) reuses this
    # SAME schedule-construction function rather than a hand-duplicated
    # copy, so both legs are provably running the identical 1000-edit
    # schedule. Writes the schedule JSON to the given path and exits 0
    # WITHOUT running party at all.
    parser.add_argument("--write-schedule-only", default=None, metavar="PATH")
    args = parser.parse_args(argv)

    schedule = build_1000_edit_schedule()
    if args.write_schedule_only is not None:
        with open(args.write_schedule_only, "w") as f:
            json.dump(schedule, f)
        print(f"[run_cga_gate] wrote schedule to {args.write_schedule_only}")
        return 0

    assert args.party_bin is not None, "--party-bin is required unless --write-schedule-only is used"

    if args.workdir is not None and os.path.isdir(args.workdir):
        shutil.rmtree(args.workdir)
    workdir = args.workdir or tempfile.mkdtemp(prefix="sympsica_cga_")
    os.makedirs(workdir, exist_ok=True)

    total_edits = sum(len(d["insert"]) + len(d["delete"]) for d in schedule)
    assert total_edits == 1000, f"schedule must carry exactly 1000 edits, got {total_edits}"
    assert all(not d["query"] for d in schedule), "CG-A schedule must have NO query days"

    schedule_path = os.path.join(workdir, "cga_schedule.json")
    with open(schedule_path, "w") as f:
        json.dump(schedule, f)

    state_dir = os.path.join(workdir, "state_r")
    stderr_path = os.path.join(workdir, "stderr_r.log")

    cmd = [
        args.party_bin, "--role", "r", "--update-only",
        "--schedule", schedule_path, "--state", state_dir, "--seed", "42",
    ]
    print(f"[run_cga_gate] command: {' '.join(cmd)}")
    print(f"[run_cga_gate] NO --listen/--connect passed; NO counterpart process launched; "
          f"NO socket opened by this driver ({total_edits} total edits across {len(schedule)} days).")

    with open(stderr_path, "wb") as err_f:
        try:
            proc = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=err_f, timeout=args.timeout_s)
        except subprocess.TimeoutExpired:
            print(f"[run_cga_gate] FAIL: party --update-only did not exit within {args.timeout_s}s "
                  "(a zero-communication local loop hanging is itself a real bug)", file=sys.stderr)
            return 1

    with open(stderr_path) as f:
        stderr_text = f.read()

    print(f"[run_cga_gate] exit code: {proc.returncode}")
    print(f"[run_cga_gate] stderr:\n{stderr_text}")

    # SC1 [CG-A-i]
    assert proc.returncode == 0, f"SC1 FAIL: party --update-only exited {proc.returncode} (expected 0)"

    # SC2 [CG-A-ii], runtime leg.
    marker = "channel_construction_count="
    count_line = next((line for line in stderr_text.splitlines() if line.startswith(marker)), None)
    assert count_line is not None, (
        f"SC2 FAIL: no '{marker}' line found in party --update-only's stderr -- "
        "apps/party_main.cpp's --update-only branch must print this before returning"
    )
    count = int(count_line[len(marker):])
    assert count == 0, (
        f"SC2 FAIL: channel_construction_count={count}, expected EXACTLY 0 -- a Channel was "
        "constructed somewhere on the --update-only path"
    )

    print(f"[run_cga_gate] SC1 OK: exit code 0. SC2 (runtime leg) OK: channel_construction_count == 0 "
          f"exactly (static leg is core.GrepGuard_NoChannelInUpdateOnly, a separate ctest entry).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

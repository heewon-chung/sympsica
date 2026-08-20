#!/usr/bin/env python3
"""test/e2e/run_restart_gate.py — task-26-brief.md SC4 [N2-E2E]: a REAL
two-OS-process schedule, split at a successfully COMMITTED maintenance
query, both processes terminated CLEANLY, then RESTARTED from both state
files (a fresh `party` invocation per party, same --state dir, so it loads
the on-disk state apps/party_main.cpp's own state_path.load() path writes).
This is the exact scenario N2 breaks (task-26-brief.md's own N2 finding):
a state committed after a maintenance day reloads with a table built under
the FRESH salt while a pre-fix restart always re-derives Params at epoch 0,
so a post-restart Update::apply/Query::run silently buckets under the wrong
oracle.

Schedule (fixed, deliberately small so EVERY query lands on the FullPublic
path regardless of J bookkeeping -- SwitchRule::decide's own
`firstQuery || 2*u_max >= nA+nB` branch, R+S combined size here is 7 << 2048
-- so the plaintext-full-union evaluation always includes every occupied
bucket, guaranteeing a stale post-restart bucket residue is NOT skipped by
J-based incremental padding):

  Phase 1 (BEFORE restart):
    day 1 (non-query): R inserts {1,2,3,999999}; S inserts {2,3,4}.
    day 2 (query, MAINTENANCE): triggers SaltManager::refresh -- new salt,
        table rebuilt under it, forced-full query. True count = |{2,3}| = 2.
  [restart: both processes exit cleanly; a FRESH `party` process is
   launched per party against the SAME --state dir]
  Phase 2 (AFTER restart):
    day 3 (non-query): R deletes {999999} -- an update whose CANCELLATION
        (the delete must land in the SAME bucket its day-1 insert used) only
        works if the restored oracle equals the one the table was rebuilt
        under on day 2. Under the pre-fix bug, party_main reconstructs
        epoch-0 Params regardless, so this delete edits a DIFFERENT bucket,
        leaving a stale nonzero row behind (id 999999's insert) instead of
        cancelling it.
    day 4 (query): True count = |{1,2,3} meet {2,3,4}| = |{2,3}| = 2, same
        as day 2's (999999 was never in S). Under the bug this diverges: the
        stale bucket contributes a syndrome for an id that is no longer
        (and, from S's side, never was) in the true symmetric difference.

SC4's own requirement is that BOTH day 2 and day 4's counts equal
reference.py's ground truth AND equal each other across the restart --
run_e2e_gate.py's existing single-process driver structurally cannot
exercise this (it never restarts party_main mid-schedule), hence this
separate, deliberately minimal driver (stdlib only, same shape as
run_e2e_gate.py/run_smoke.py).
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


def day(d: str, insert: list[int], delete: list[int], query: bool, maintenance: bool) -> dict:
    return {"day": d, "insert": insert, "delete": delete, "query": query, "maintenance": maintenance}


# Phase 1: day 1 (non-query inserts) + day 2 (query, maintenance).
SCHEDULE_R_PHASE1 = [
    day("2030-01-01", [1, 2, 3, 999999], [], False, False),
    day("2030-01-02", [], [], True, True),
]
SCHEDULE_S_PHASE1 = [
    day("2030-01-01", [2, 3, 4], [], False, False),
    day("2030-01-02", [], [], True, True),
]

# Phase 2 (post-restart): day 3 (R deletes the bucket-cancellation-sensitive
# id) + day 4 (query, ordinary -- NOT maintenance, confirming the restart
# also correctly restored query_no so due() does not spuriously re-fire).
SCHEDULE_R_PHASE2 = [
    day("2030-01-03", [], [999999], False, False),
    day("2030-01-04", [], [], True, False),
]
SCHEDULE_S_PHASE2 = [
    day("2030-01-03", [], [], False, False),
    day("2030-01-04", [], [], True, False),
]

EXPECTED_COUNTS = [2, 2]  # day 2, day 4 -- both = |{2,3}|


def run_phase(party_bin: str, schedule_r: list[dict], schedule_s: list[dict], state_r: str, state_s: str,
              out_r: str, out_s: str, workdir: str, phase_name: str, timeout_s: float,
              seed_r: int, seed_s: int) -> dict[str, int]:
    sched_r_path = os.path.join(workdir, f"{phase_name}_r.json")
    sched_s_path = os.path.join(workdir, f"{phase_name}_s.json")
    with open(sched_r_path, "w") as f:
        json.dump(schedule_r, f)
    with open(sched_s_path, "w") as f:
        json.dump(schedule_s, f)

    stderr_r_path = os.path.join(workdir, f"stderr_r_{phase_name}.log")
    stderr_s_path = os.path.join(workdir, f"stderr_s_{phase_name}.log")

    port = find_free_port()
    receiver_cmd = [
        party_bin, "--role", "r", "--listen", str(port),
        "--schedule", sched_r_path, "--state", state_r, "--out", out_r, "--seed", str(seed_r),
    ]
    sender_cmd = [
        party_bin, "--role", "s", "--connect", f"127.0.0.1:{port}",
        "--schedule", sched_s_path, "--state", state_s, "--out", out_s, "--seed", str(seed_s),
    ]
    print(f"[run_restart_gate] {phase_name} receiver: {' '.join(receiver_cmd)}")
    print(f"[run_restart_gate] {phase_name} sender:   {' '.join(sender_cmd)}")

    t0 = time.monotonic()
    with open(stderr_r_path, "wb") as err_r, open(stderr_s_path, "wb") as err_s:
        proc_r = subprocess.Popen(receiver_cmd, stdout=subprocess.DEVNULL, stderr=err_r)
        proc_s = subprocess.Popen(sender_cmd, stdout=subprocess.DEVNULL, stderr=err_s)

        deadline = time.monotonic() + timeout_s
        procs = {"receiver": proc_r, "sender": proc_s}
        exit_codes: dict[str, int | None] = {"receiver": None, "sender": None}
        while time.monotonic() < deadline and any(c is None for c in exit_codes.values()):
            for name, p in procs.items():
                if exit_codes[name] is None:
                    rc = p.poll()
                    if rc is not None:
                        exit_codes[name] = rc
            if any(c is None for c in exit_codes.values()):
                time.sleep(0.1)

        timed_out = any(c is None for c in exit_codes.values())
        if timed_out:
            for name, p in procs.items():
                if exit_codes[name] is None:
                    p.kill()
                    p.wait()
            with open(stderr_r_path, "rb") as f:
                tail_r = f.read().decode("utf-8", "replace")[-2000:]
            with open(stderr_s_path, "rb") as f:
                tail_s = f.read().decode("utf-8", "replace")[-2000:]
            raise AssertionError(
                f"{phase_name}: timed out after {timeout_s}s; exit_codes so far={exit_codes}\n"
                f"receiver stderr tail: {tail_r}\nsender stderr tail: {tail_s}")

    elapsed = time.monotonic() - t0
    print(f"[run_restart_gate] {phase_name}: both parties exited after {elapsed:.1f}s: {exit_codes}")

    with open(stderr_r_path, "rb") as f:
        stderr_r = f.read().decode("utf-8", "replace")
    with open(stderr_s_path, "rb") as f:
        stderr_s = f.read().decode("utf-8", "replace")

    assert exit_codes["receiver"] == 0, (
        f"{phase_name}: receiver exited {exit_codes['receiver']} (expected 0); "
        f"stderr tail: {stderr_r[-2000:]}")
    assert exit_codes["sender"] == 0, (
        f"{phase_name}: sender exited {exit_codes['sender']} (expected 0); "
        f"stderr tail: {stderr_s[-2000:]}")

    return exit_codes


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(prog="run_restart_gate.py")
    parser.add_argument("--party-bin", required=True)
    parser.add_argument("--reference", required=True)
    parser.add_argument("--workdir", default=None)
    parser.add_argument("--timeout-s", type=float, default=60.0, help="per-phase timeout")
    args = parser.parse_args(argv)

    ref = load_reference(args.reference)

    schedule_r_full = SCHEDULE_R_PHASE1 + SCHEDULE_R_PHASE2
    schedule_s_full = SCHEDULE_S_PHASE1 + SCHEDULE_S_PHASE2
    expected_counts = ref.Ref.simulate_days(schedule_r_full, schedule_s_full)
    print(f"[run_restart_gate] expected counts (reference.py simulate_days): {expected_counts}")
    assert expected_counts == EXPECTED_COUNTS, (
        f"test precondition: this script's own EXPECTED_COUNTS constant {EXPECTED_COUNTS} disagrees "
        f"with a fresh reference.py computation {expected_counts} -- fix the constant, do not paper "
        f"over a real disagreement"
    )

    if args.workdir is not None and os.path.isdir(args.workdir):
        import shutil
        shutil.rmtree(args.workdir)
    workdir = args.workdir or tempfile.mkdtemp(prefix="sympsica_restart_")
    os.makedirs(workdir, exist_ok=True)
    state_r = os.path.join(workdir, "state_r")
    state_s = os.path.join(workdir, "state_s")
    out_r1 = os.path.join(workdir, "out_r_phase1.jsonl")
    out_s1 = os.path.join(workdir, "out_s_phase1.jsonl")
    out_r2 = os.path.join(workdir, "out_r_phase2.jsonl")
    out_s2 = os.path.join(workdir, "out_s_phase2.jsonl")

    # --- Phase 1: fresh parties, ends right after a COMMITTED maintenance
    # query (day 2). ---
    run_phase(args.party_bin, SCHEDULE_R_PHASE1, SCHEDULE_S_PHASE1, state_r, state_s, out_r1, out_s1,
              workdir, "phase1", args.timeout_s, seed_r=51001, seed_s=52002)

    records_r1 = read_jsonl(out_r1)
    records_s1 = read_jsonl(out_s1)
    print(f"[run_restart_gate] phase1 receiver JSONL: {records_r1}")
    print(f"[run_restart_gate] phase1 sender JSONL:   {records_s1}")
    assert len(records_r1) == 1 and len(records_s1) == 1, (
        f"phase1: expected exactly 1 query record per party (day 2 only), got "
        f"{len(records_r1)} receiver / {len(records_s1)} sender"
    )
    assert records_r1[0]["path"] == "maintenance_full", (
        f"test precondition: phase1's query day must be the maintenance path, got "
        f"{records_r1[0]['path']!r} -- SC4 needs a genuinely COMMITTED maintenance query to restart "
        f"after"
    )
    # task-28-brief.md carried finding C2 (Task 26 review): query_no after
    # the FIRST committed query is always exactly 1 (party_main.cpp's
    # jsonl_record fires AFTER Query::run's/SaltManager::refresh's atomic
    # commit, and commit() increments query_no exactly once per committed
    # query -- src/protocols/query.cpp's detail::commit()). Pinning this
    # here is what makes the phase-2 query_no check below a real
    # discriminator rather than an assumption.
    assert records_r1[0]["query_no"] == 1, (
        f"test precondition: phase1's committed query_no must be exactly 1 (first-ever query), got "
        f"{records_r1[0]['query_no']}"
    )
    assert records_r1[0]["count"] == records_s1[0]["count"] == EXPECTED_COUNTS[0], (
        f"phase1 (pre-restart) count mismatch: receiver={records_r1[0]['count']} "
        f"sender={records_s1[0]['count']} expected={EXPECTED_COUNTS[0]}"
    )
    print(f"[run_restart_gate] phase1 OK: maintenance query committed, count={records_r1[0]['count']}")

    assert os.path.exists(os.path.join(state_r, "state.bin")), "phase1: receiver state.bin missing after clean exit"
    assert os.path.exists(os.path.join(state_s, "state.bin")), "phase1: sender state.bin missing after clean exit"

    # --- RESTART: both processes already exited cleanly above (run_phase
    # asserted exit code 0 for both). Phase 2 launches BRAND-NEW `party`
    # processes against the SAME --state dirs -- this is the load() path
    # N2 breaks. ---
    run_phase(args.party_bin, SCHEDULE_R_PHASE2, SCHEDULE_S_PHASE2, state_r, state_s, out_r2, out_s2,
              workdir, "phase2", args.timeout_s, seed_r=51001, seed_s=52002)

    records_r2 = read_jsonl(out_r2)
    records_s2 = read_jsonl(out_s2)
    print(f"[run_restart_gate] phase2 receiver JSONL: {records_r2}")
    print(f"[run_restart_gate] phase2 sender JSONL:   {records_s2}")
    assert len(records_r2) == 1 and len(records_s2) == 1, (
        f"phase2: expected exactly 1 query record per party (day 4 only), got "
        f"{len(records_r2)} receiver / {len(records_s2)} sender"
    )
    assert records_r2[0]["path"] != "maintenance_full", (
        f"test precondition: phase2's query day must NOT be maintenance -- got "
        f"{records_r2[0]['path']!r}; a spurious re-trigger would mean query_no didn't restore "
        f"correctly either, a separate bug this driver isn't targeting"
    )
    # task-28-brief.md carried finding C2 (Task 26 review): `path !=
    # "maintenance_full"` alone does NOT establish query_no was restored --
    # a query_no lost to 0 on restart also yields firstQuery (`query_no ==
    # 0`), which ALSO takes the non-maintenance "full_public" path
    # (SwitchRule::decide's own `firstQuery || ...` branch), so the check
    # above would pass even under N2's exact bug. Directly observe the
    # restored query_no instead: day 4 is the party's SECOND-EVER committed
    # query, so a correctly-restored query_no must read back as exactly 2
    # (phase1's day2 committed 1, per the precondition pinned above) -- a
    # lost/reset query_no would instead commit day 4 as query_no=1, exactly
    # matching phase1's day2 value and indistinguishable from "restart threw
    # query_no away".
    assert records_r2[0]["query_no"] == 2, (
        f"SC4 [N2-E2E, C2] FAILED: phase2's committed query_no is "
        f"{records_r2[0]['query_no']}, expected 2 -- a value of 1 here means query_no was NOT "
        f"restored across the restart (day 4 was treated as this process's first-ever query, "
        f"exactly N2's failure signature for query_no)"
    )

    # --- SC4 [N2-E2E]: the actual assertion this whole driver exists for.
    # Under the pre-fix bug, day 3's delete of id 999999 buckets under the
    # WRONG (epoch-0) oracle post-restart, leaving a stale nonzero row in
    # the table under the CORRECT (post-maintenance) oracle's bucket for
    # that id -- day 4's FullPublic-path query (guaranteed by this
    # schedule's small combined size, 7 << 2*u_max) unions ALL occupied
    # buckets and would pick up that stale residue, producing a count that
    # disagrees with reference.py and/or with the OTHER real party. ---
    assert records_r2[0]["count"] == records_s2[0]["count"] == EXPECTED_COUNTS[1], (
        f"SC4 [N2-E2E] FAILED: post-restart count mismatch -- receiver={records_r2[0]['count']} "
        f"sender={records_s2[0]['count']} expected={EXPECTED_COUNTS[1]} (this is exactly N2's "
        f"failure signature: a restart after maintenance silently bucketing under the wrong oracle)"
    )
    print(f"[run_restart_gate] SC4 [N2-E2E] OK: post-restart count={records_r2[0]['count']} matches "
          f"reference.py AND both real parties agree, across a real maintenance-day restart.")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

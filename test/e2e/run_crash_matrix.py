#!/usr/bin/env python3
"""test/e2e/run_crash_matrix.py — SC3 (task-19-brief.md W5.8, controller
rulings R-CRASHDRV/R-INVCHK): the deterministic 5-point crash matrix, driven
by real OS SUBPROCESSES of apps/crash_probe.cpp's `crash_probe` binary (see
that file's own top comment for why crash_probe, not party_main, is used
here). Stdlib only, same shape/discipline as run_smoke.py (hard timeouts,
fresh workdir per hook, no hang left behind).

For each of the 5 SYMPSICA_CRASH_AT points {pre-serialize, mid-temp-write,
post-fsync, pre-rename, post-rename}:
  1. Fresh workdir. Both parties `--mode init` (no networking) with their
     own base ids.
  2. Both parties `--mode query` (a normal, un-hooked "q1") -- this becomes
     the PRE golden for q2 (captured once per hook run, freshly, rather
     than reused across hooks, so each hook's golden reflects THAT run's
     own on-disk bytes exactly).
  3. A CLEAN q2 (no crash) run against a COPY of the post-q1 state dirs
     produces the POST golden.
  4. The REAL crash run: q2 again from the original (uncopied) post-q1
     state dirs, with the R-party's env carrying SYMPSICA_CRASH_AT=<hook>.
     R is expected to die via std::_Exit(137); S is not hard-asserted (it
     typically completes normally, since the crash hooks all sit strictly
     after R's own eval_union networking has finished).
  5. R-INVCHK: R's on-disk state.bin bytes after the crash run must be
     EXACTLY ONE of {pre golden, post golden} -- never a third, torn value.
  6. Recovery: BOTH parties re-run `--mode query` (crash env unset) against
     R's now-settled state dir. Per crash_probe.cpp's own doc comment, this
     is safe/idempotent at this tiny, always-full-path scale regardless of
     which golden R's state matched -- and the recovered count must equal
     the true (independently known) plaintext intersection size.

Assertions per hook: >= 3 (byte-match-one-of-two, R exit code 137, replay
count correct) -- >= 15 total across all 5 hooks, comfortably over SC3's
">= 10 assertions minimum".

Scope (task-24-brief.md R6-I2-WAIVED, docs-only, zero behavior change): this
matrix proves SINGLE-PARTY atomicity (R's own state.bin is never torn) and
FULL-PATH replay recovery only, both at the always-full-path scale crash_
probe.cpp runs at. Pair-level divergence under INCREMENTAL replay (recovery
after a crash mid-incremental-query, across both parties, at production
scale) is out of PoC scope -- waived by the user as the Phase-5 gate's I2
finding, not attempted anywhere in this repo.
"""

from __future__ import annotations

import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time


HOOKS = ["pre-serialize", "mid-temp-write", "post-fsync", "pre-rename", "post-rename"]

# Base ids (tiny scale, R-CRASHDRV doesn't mandate E2E scale for the crash
# matrix -- see crash_probe.cpp's own doc comment). True intersection at q1:
# R={1..6}, S={4..9} -> {4,5,6} = 3. At q2 (R gets +{10,11}, S gets +{12}):
# still {4,5,6} = 3 (the day-2 inserts are disjoint from the intersection).
R_BASE = [1, 2, 3, 4, 5, 6]
S_BASE = [4, 5, 6, 7, 8, 9]
R_EXTRA = [10, 11]
S_EXTRA = [12]
TRUE_COUNT = 3


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def run_pair(bin_path: str, state_r: str, state_s: str, mode: str, extra_r: list[int] | None,
             extra_s: list[int] | None, timeout_s: float, crash_role: str | None = None,
             crash_hook: str | None = None) -> dict:
    """Launches both parties once, in `mode`, with a hard timeout. Returns
    {"r": {"rc": int|None, "out": str, "err": str}, "s": {...}}. `crash_role`
    ('r'/'s'/None) gets SYMPSICA_CRASH_AT=crash_hook in its own env only.
    """
    port = find_free_port()
    seed_r, seed_s = 31001, 32002

    def cmd(role: str, is_listen: bool, state_dir: str, extra: list[int] | None) -> list[str]:
        c = [bin_path, "--role", role]
        c += ["--listen", str(port)] if is_listen else ["--connect", f"127.0.0.1:{port}"]
        c += ["--state", state_dir, "--mode", mode, "--seed", str(seed_r if role == "r" else seed_s)]
        if extra:
            c += ["--extra-ids", ",".join(str(x) for x in extra)]
        return c

    env_r = dict(os.environ)
    env_s = dict(os.environ)
    env_r.pop("SYMPSICA_CRASH_AT", None)
    env_s.pop("SYMPSICA_CRASH_AT", None)
    if crash_role == "r" and crash_hook:
        env_r["SYMPSICA_CRASH_AT"] = crash_hook
    if crash_role == "s" and crash_hook:
        env_s["SYMPSICA_CRASH_AT"] = crash_hook

    cmd_r = cmd("r", True, state_r, extra_r)
    cmd_s = cmd("s", False, state_s, extra_s)

    proc_r = subprocess.Popen(cmd_r, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env_r, text=True)
    proc_s = subprocess.Popen(cmd_s, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env_s, text=True)

    deadline = time.monotonic() + timeout_s
    results: dict[str, dict] = {"r": {"rc": None}, "s": {"rc": None}}
    procs = {"r": proc_r, "s": proc_s}
    while time.monotonic() < deadline and any(results[k]["rc"] is None for k in ("r", "s")):
        for k, p in procs.items():
            if results[k]["rc"] is None:
                rc = p.poll()
                if rc is not None:
                    out, err = p.communicate()
                    results[k] = {"rc": rc, "out": out, "err": err}
        if any(results[k]["rc"] is None for k in ("r", "s")):
            time.sleep(0.05)

    timed_out = any(results[k]["rc"] is None for k in ("r", "s"))
    if timed_out:
        for k, p in procs.items():
            if results[k]["rc"] is None:
                p.kill()
                out, err = p.communicate()
                results[k] = {"rc": None, "out": out, "err": err, "timed_out": True}
        raise TimeoutError(f"run_pair({mode}): timed out after {timeout_s}s; results so far={results}")

    return results


def init_pair(bin_path: str, state_r: str, state_s: str) -> None:
    subprocess.run([bin_path, "--role", "r", "--state", state_r, "--mode", "init",
                     "--ids", ",".join(str(x) for x in R_BASE)], check=True, capture_output=True)
    subprocess.run([bin_path, "--role", "s", "--state", state_s, "--mode", "init",
                     "--ids", ",".join(str(x) for x in S_BASE)], check=True, capture_output=True)


def state_bin_bytes(state_path: str) -> bytes:
    # crash_probe.cpp's --state names a FILE directly (PartyState::save()'s
    # own path argument), not a directory -- unlike party_main.cpp's
    # --state, which names a directory containing state.bin.
    with open(state_path, "rb") as f:
        return f.read()


def inspect_fields(bin_path: str, state_path: str) -> dict:
    """Parses crash_probe --mode inspect's one-line structural-field
    output ("query_no=X J=Y cache=Z my_size=W") into a dict. See
    apps/crash_probe.cpp's own doc comment for why the POST comparison
    uses these DETERMINISTIC structural fields rather than raw file bytes
    (Setup::run's OT randomness is genuinely non-deterministic across
    independent process runs, even with identical --seed/ids -- a real,
    correct production property, not a defect).
    """
    out = subprocess.run([bin_path, "--role", "r", "--state", state_path, "--mode", "inspect"],
                          check=True, capture_output=True, text=True).stdout.strip()
    fields = {}
    for tok in out.split():
        k, v = tok.split("=")
        fields[k] = int(v)
    return fields


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(prog="run_crash_matrix.py")
    parser.add_argument("--probe-bin", required=True)
    parser.add_argument("--workdir", default=None)
    parser.add_argument("--timeout-s", type=float, default=60.0)
    args = parser.parse_args(argv)

    if args.workdir is not None and os.path.isdir(args.workdir):
        shutil.rmtree(args.workdir)
    root = args.workdir or tempfile.mkdtemp(prefix="sympsica_crashmatrix_")
    os.makedirs(root, exist_ok=True)

    total_assertions = 0

    for hook in HOOKS:
        print(f"[crash-matrix] === hook={hook} ===")
        hookdir = os.path.join(root, hook.replace("-", "_"))
        os.makedirs(hookdir, exist_ok=True)

        state_r = os.path.join(hookdir, "state_r")
        state_s = os.path.join(hookdir, "state_s")
        init_pair(args.probe_bin, state_r, state_s)

        # q1: normal, un-hooked -- establishes the base state both q2
        # goldens are derived from.
        run_pair(args.probe_bin, state_r, state_s, "query", None, None, args.timeout_s)

        # POST golden: a clean q2 over a COPY of the post-q1 state files.
        # Structural fields only (query_no/J/cache/my_size), NOT raw bytes
        # -- see inspect_fields()'s own doc comment: Setup::run's OT
        # randomness is genuinely non-deterministic across independent
        # process runs, so two independently-completed q2 runs never
        # byte-match even though both are equally CORRECT.
        post_r = os.path.join(hookdir, "state_r_post")
        post_s = os.path.join(hookdir, "state_s_post")
        shutil.copy2(state_r, post_r)
        shutil.copy2(state_s, post_s)
        run_pair(args.probe_bin, post_r, post_s, "query", R_EXTRA, S_EXTRA, args.timeout_s)
        post_golden_fields = inspect_fields(args.probe_bin, post_r)
        assert post_golden_fields["J"] == 0, "POST golden precondition: J must be empty after a committed query"
        assert post_golden_fields["cache"] > 0, "POST golden precondition: cache must be non-empty"

        # PRE golden: the ORIGINAL post-q1 state file, untouched, is
        # exactly q2's pre-query snapshot by construction (q2 hasn't run
        # against it yet) -- THIS comparison stays a full bit-exact byte
        # comparison (achievable: an untouched file's bytes are trivially
        # stable across re-reads, unlike a freshly-committed one's).
        pre_golden = state_bin_bytes(state_r)
        pre_golden_fields = inspect_fields(args.probe_bin, state_r)

        # The crash run: q2 for real, R's env carries SYMPSICA_CRASH_AT.
        try:
            results = run_pair(args.probe_bin, state_r, state_s, "query", R_EXTRA, S_EXTRA,
                                args.timeout_s, crash_role="r", crash_hook=hook)
        except TimeoutError as e:
            print(f"[crash-matrix] FAIL hook={hook}: {e}", file=sys.stderr)
            return 1

        r_rc = results["r"]["rc"]
        assert r_rc == 137, f"hook={hook}: receiver must exit 137 (std::_Exit(137)), got {r_rc}"
        total_assertions += 1
        print(f"[crash-matrix] hook={hook}: receiver exited 137 as expected")

        crashed_bytes = state_bin_bytes(state_r)
        matches_pre = crashed_bytes == pre_golden
        crashed_fields = inspect_fields(args.probe_bin, state_r)
        matches_post_structurally = (
            crashed_fields["query_no"] == post_golden_fields["query_no"]
            and crashed_fields["J"] == post_golden_fields["J"]
            and crashed_fields["cache"] == post_golden_fields["cache"]
            and crashed_fields["my_size"] == post_golden_fields["my_size"]
        )
        # R-INVCHK "never a mixture": EXACTLY one of {bit-exact PRE, the
        # complete POST structural shape} -- a torn state (some but not all
        # of commit()'s mutations landed) would satisfy NEITHER: it would
        # differ from the untouched PRE bytes (so matches_pre is False) AND
        # fail to have query_no/J/cache/my_size ALL simultaneously equal to
        # the fully-committed POST shape (test/protocols/
        # kat_crash_invariant.cpp's FC3 demonstrates this directly for a
        # deliberately wrong-order construction; this crash matrix confirms
        # the REAL, correctly-ordered commit() never produces it, at any of
        # the 5 real kill points).
        assert matches_pre != matches_post_structurally, (
            f"hook={hook}: R-INVCHK FAILED -- recovered state must match EXACTLY ONE of "
            f"{{pre golden (bytes), post golden (structural fields)}}, never both and never "
            f"neither (matches_pre={matches_pre}, matches_post_structurally="
            f"{matches_post_structurally}, crashed_fields={crashed_fields}, "
            f"pre_golden_fields={pre_golden_fields}, post_golden_fields={post_golden_fields})"
        )
        total_assertions += 1
        outcome = "PRE" if matches_pre else "POST"
        print(f"[crash-matrix] hook={hook}: recovered state matches {outcome} golden (R-INVCHK OK) "
              f"fields={crashed_fields}")

        # Recovery: redo q2 (crash env unset) -- safe/idempotent at this
        # scale regardless of PRE/POST outcome (crash_probe.cpp's own doc
        # comment). S's own state is already q2-post-committed from the
        # crash run above (S never crashes); redoing S's own query too is
        # symmetric and equally safe.
        recover = run_pair(args.probe_bin, state_r, state_s, "query", R_EXTRA, S_EXTRA, args.timeout_s)
        assert recover["r"]["rc"] == 0, f"hook={hook}: recovery query must succeed, rc={recover['r']['rc']}"
        assert recover["s"]["rc"] == 0, f"hook={hook}: recovery query must succeed, rc={recover['s']['rc']}"
        r_count = int(recover["r"]["out"].strip().splitlines()[-1])
        s_count = int(recover["s"]["out"].strip().splitlines()[-1])
        assert r_count == TRUE_COUNT, f"hook={hook}: replayed count mismatch (receiver): {r_count} != {TRUE_COUNT}"
        assert s_count == TRUE_COUNT, f"hook={hook}: replayed count mismatch (sender): {s_count} != {TRUE_COUNT}"
        total_assertions += 1
        print(f"[crash-matrix] hook={hook}: replay yields correct count ({TRUE_COUNT}) -- OK")

    print(f"[crash-matrix] ALL {len(HOOKS)} hooks OK, {total_assertions} assertions total "
          f"(SC3 needs >= 10)")
    assert total_assertions >= 10
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

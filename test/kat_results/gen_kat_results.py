#!/usr/bin/env python3
"""test/kat_results/gen_kat_results.py — task-23-brief.md R6-KATRES:
generates docs/kat_results.md, the paste-ready results table for the user's
own `.handoff/` section 5 (`.handoff/` itself is USER-OWNED and read-only to
this project's agents -- R6-KATRES resolves the plan's "fill kat_results in
handoff §5" by generating THIS file instead, for the user to paste
verbatim).

R6-KATRES is explicit: "Generate it from real recorded output -- never
hand-type a number you did not observe." Every number in the generated file
comes from one of:

  1. A captured ctest log of ONE full serial suite run (`--ctest-log`) --
     this task's own SC6 gate run. Parsed for the overall pass/fail/time
     summary and every individual test's own PASS/FAIL/time line.
  2. Files left on disk, in each E2E seed's own ctest workdir, by that SAME
     run: apps/party_main.cpp's --audit-out JSON dumps (this task's SC2
     [AUDIT-E2E]), both parties' captured stderr (SC3/SC4 [CGB-E2E/
     CGB-NONVACUOUS]), and both parties' JSONL query records (the REAL,
     freshly-observed `path` labels -- re-confirming task-22-report.md's
     finding with THIS run's own output, not a copy of the old numbers).
     test/e2e/run_e2e_gate.py's own audit_pool()/extract_u64() are imported
     directly and reused here (single source of truth, not a re-derivation)
     -- see that script for the three of R6-AUDIT-TRANSCRIPT's four
     properties that are checked; the fourth (cross-refill global
     uniqueness) is relied on via CorrelationPool::refill's own enforcement,
     not independently re-checked (audit_pool_transcript.hpp explains why).
  3. A quick, SEQUENTIAL re-invocation of the already-built
     `sympsica_tests_protocols` binary (`--protocols-test-bin`), filtered to
     ONLY this task's own unit-scale tests (`PoolAudit.SC1*:PoolAudit.FC4*`)
     -- their real stdout JSON summary lines (test/protocols/
     kat_pool_audit.cpp), captured fresh. Not run concurrently with any
     other test process (this project's MACHINE INVARIANT: one ctest run --
     or ANY port-binding test binary -- at a time); this happens strictly
     AFTER the ctest run item 1/2 above already finished.
  4. Task 20's own committed TV-F registry table (task-20-report.md),
     parsed directly out of that file's real markdown table (13 COVERED /
     4 DEFERRED) -- Task 20's wrong-construction legs (build-wrongsign/,
     build-wrongconvert/, etc.) are NOT part of a normal `ctest --test-dir
     build` run, so this task does not re-run them; it cites the
     already-recorded, already-verified table programmatically instead of
     hand-typing its numbers.

Invocation (run ONCE, after the SC6 gate has already passed with the
machine clear -- see task-23-report.md for the exact commands and captured
output this run used):

  python3 test/kat_results/gen_kat_results.py \\
      --ctest-log <path to the captured SC6 ctest run's stdout+stderr> \\
      --commit <the commit that run was captured at -- REQUIRED> \\
      --build-dir build \\
      --protocols-test-bin build/sympsica_tests_protocols \\
      --task20-report .superpowers/sdd/sympsica-plan/task-20-report.md \\
      --out docs/kat_results.md
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import re
import subprocess
import sys
from types import ModuleType


def load_module(path: str, name: str) -> ModuleType:
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None, f"could not load {name} from {path}"
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# ---------------------------------------------------------------------------
# 1. ctest log parsing.
# ---------------------------------------------------------------------------

_TEST_LINE_RE = re.compile(
    r"Test\s*#\d+:\s*(\S+)\s+\.+\s+(Passed|\*\*\*Failed|\*\*\*Timeout|\*\*\*Not Run)\s+([\d.]+)\s*sec"
)
# task-23-report.md fix round (this task, real-log verification): ctest 4.4.2's summary
# line DROPS the ", N tests failed" clause entirely when N == 0 -- it prints
# "100% tests passed out of 154", not "100% tests passed, 0 tests failed out of 154". The
# original regex required that clause unconditionally, so a fully-green run (this task's own
# real SC6 log) silently fell back to "could not parse the overall ctest summary line" instead
# of reporting the real 154/154. Made optional via `(?:, (\d+) tests failed)?`, defaulting to 0
# when absent -- verified against BOTH this task's real all-pass log and a synthetic
# some-failed line matching the old format (see task-23-report.md for both).
_SUMMARY_RE = re.compile(r"(\d+)% tests passed(?:, (\d+) tests failed)? out of (\d+)")
_TOTAL_TIME_RE = re.compile(r"Total Test time \(real\)\s*=\s*([\d.]+)\s*sec")


def parse_ctest_log(text: str) -> dict:
    tests = []
    for m in _TEST_LINE_RE.finditer(text):
        tests.append({"name": m.group(1), "status": m.group(2), "seconds": float(m.group(3))})
    summary_m = _SUMMARY_RE.search(text)
    total_time_m = _TOTAL_TIME_RE.search(text)
    return {
        "tests": tests,
        "pct_passed": int(summary_m.group(1)) if summary_m else None,
        "n_failed": int(summary_m.group(2)) if summary_m and summary_m.group(2) is not None else
                    (0 if summary_m else None),
        "n_total": int(summary_m.group(3)) if summary_m else None,
        "total_time_s": float(total_time_m.group(1)) if total_time_m else None,
    }


def group_by_suite(tests: list[dict]) -> dict[str, dict]:
    """Groups ctest's flat test list by the name prefix before the first
    '.' (e.g. `core.Xyz` -> suite `core`, `heavy.GoldenFull.Abc` -> suite
    `heavy`) -- the SAME per-directory-target convention CMakeLists.txt's
    sympsica_add_test_target/TEST_PREFIX already uses."""
    suites: dict[str, dict] = {}
    for t in tests:
        suite = t["name"].split(".", 1)[0]
        s = suites.setdefault(suite, {"n": 0, "n_failed": 0, "seconds": 0.0})
        s["n"] += 1
        s["seconds"] += t["seconds"]
        if t["status"] != "Passed":
            s["n_failed"] += 1
    return suites


# ---------------------------------------------------------------------------
# 4. Task 20's TV-F registry table.
# ---------------------------------------------------------------------------

_TVF_ROW_RE = re.compile(
    r"^\|\s*(TV-F\d+|EXTRA[^|]*)\s*\|\s*([^|]+?)\s*\|\s*([^|]+?)\s*\|.*\|\s*([^|]+?)\s*\|\s*$"
)


def parse_tvf_table(task20_report_text: str) -> list[dict]:
    rows = []
    for line in task20_report_text.splitlines():
        m = _TVF_ROW_RE.match(line)
        if not m:
            continue
        row_id, status, subject, evidence = m.groups()
        rows.append({"id": row_id.strip(), "status": status.strip(), "subject": subject.strip(),
                     "evidence": evidence.strip()})
    return rows


def main(argv: list[str]) -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.abspath(os.path.join(here, "..", ".."))

    parser = argparse.ArgumentParser()
    parser.add_argument("--ctest-log", required=True,
                         help="captured stdout+stderr of ONE full `ctest --test-dir <build-dir> "
                              "--no-tests=error` run (this task's SC6 gate)")
    parser.add_argument("--build-dir", default=os.path.join(repo_root, "build"))
    parser.add_argument("--protocols-test-bin",
                         default=None,
                         help="path to the built sympsica_tests_protocols binary; default "
                              "<build-dir>/sympsica_tests_protocols")
    parser.add_argument("--task20-report",
                         default=os.path.join(repo_root, ".superpowers", "sdd", "sympsica-plan",
                                               "task-20-report.md"))
    parser.add_argument("--commit", required=True,
                         help="commit the --ctest-log run was captured at. REQUIRED: this script "
                              "cannot infer it, and an unpinned number is not citable (Phase-6 gate "
                              "review, Minor 1). Append ' (note ...)' to carry a caveat.")
    parser.add_argument("--e2e-seeds", default="0,1,2,3")
    parser.add_argument("--out", default=os.path.join(repo_root, "docs", "kat_results.md"))
    args = parser.parse_args(argv)

    protocols_bin = args.protocols_test_bin or os.path.join(args.build_dir, "sympsica_tests_protocols")

    e2e_gate = load_module(os.path.join(repo_root, "test", "e2e", "run_e2e_gate.py"), "run_e2e_gate")

    with open(args.ctest_log) as f:
        ctest_text = f.read()
    ctest = parse_ctest_log(ctest_text)
    suites = group_by_suite(ctest["tests"])

    with open(args.task20_report) as f:
        tvf_rows = parse_tvf_table(f.read())
    # R6-KATRES names "13 COVERED / 4 DEFERRED" -- the TV-F1..TV-F17 rows
    # only; task-20-report.md's table also carries one additional non-TV-F
    # row (SYMPSICA_STALE_SIZES), counted separately below so this summary
    # matches that report's own wording exactly.
    tvf_only = [r for r in tvf_rows if r["id"].startswith("TV-F")]
    tvf_covered = sum(1 for r in tvf_only if r["status"].upper().startswith("COVERED"))
    tvf_deferred = sum(1 for r in tvf_only if "DEFERRED" in r["status"].upper())

    # SC1/FC4 [pool-audit unit-scale]: a quick, sequential re-invocation of
    # the already-built binary, capturing its own stdout JSON summary lines
    # fresh (see this file's own header, item 3).
    sc1_summary = None
    fc4_summary = None
    if os.path.exists(protocols_bin):
        proc = subprocess.run(
            [protocols_bin, "--gtest_filter=PoolAudit.SC1_*:PoolAudit.FC4_*"],
            capture_output=True, text=True, timeout=120)
        for line in proc.stdout.splitlines():
            line = line.strip()
            if line.startswith('{"sc1_pool_audit"'):
                sc1_summary = json.loads(line)
            elif line.startswith('{"fc4_high_water"'):
                fc4_summary = json.loads(line)

    # SC2[AUDIT-E2E]/SC3[CGB-E2E]/SC4[CGB-NONVACUOUS] + real observed `path`
    # labels, per E2E seed, from the files the SC6 run already left on disk.
    e2e_seeds = [int(s) for s in args.e2e_seeds.split(",") if s.strip()]
    e2e_results = []
    for seed in e2e_seeds:
        workdir = os.path.join(args.build_dir, f"e2e_seed{seed}_workdir")
        entry = {"seed": seed, "workdir": workdir}
        try:
            with open(os.path.join(workdir, "audit_r.json")) as f:
                audit_r = json.load(f)
            with open(os.path.join(workdir, "audit_s.json")) as f:
                audit_s = json.load(f)
            entry["audit"] = {
                "receiver": {
                    "triples": e2e_gate.audit_pool(f"seed{seed}", "triples", audit_r["triples"],
                                                    audit_r["next_corr_id"]),
                    "gates": e2e_gate.audit_pool(f"seed{seed}", "gates", audit_r["gates"],
                                                  audit_r["next_corr_id"]),
                },
                "sender": {
                    "triples": e2e_gate.audit_pool(f"seed{seed}", "triples", audit_s["triples"],
                                                    audit_s["next_corr_id"]),
                    "gates": e2e_gate.audit_pool(f"seed{seed}", "gates", audit_s["gates"],
                                                  audit_s["next_corr_id"]),
                },
            }
        except (FileNotFoundError, KeyError) as e:
            entry["audit_error"] = str(e)

        try:
            with open(os.path.join(workdir, "stderr_r.log"), "rb") as f:
                stderr_r = f.read().decode("utf-8", "replace")
            with open(os.path.join(workdir, "stderr_s.log"), "rb") as f:
                stderr_s = f.read().decode("utf-8", "replace")
            entry["cgb"] = {
                "receiver_snapshot": e2e_gate.extract_u64(stderr_r, "pkop_counter_after_setup="),
                "receiver_final": e2e_gate.extract_u64(stderr_r, "pkop_counter="),
                "sender_snapshot": e2e_gate.extract_u64(stderr_s, "pkop_counter_after_setup="),
                "sender_final": e2e_gate.extract_u64(stderr_s, "pkop_counter="),
            }
        except FileNotFoundError as e:
            entry["cgb_error"] = str(e)

        try:
            with open(os.path.join(workdir, "out_r.jsonl")) as f:
                recs = [json.loads(line) for line in f if line.strip()]
            entry["paths"] = [{"day": r["day"], "path": r["path"], "count": r["count"]} for r in recs]
        except FileNotFoundError as e:
            entry["paths_error"] = str(e)

        e2e_results.append(entry)

    md = render_markdown(ctest, suites, tvf_rows, tvf_covered, tvf_deferred, sc1_summary, fc4_summary,
                          e2e_results, args.commit)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        f.write(md)
    print(f"[gen_kat_results] wrote {args.out}")
    return 0


def render_markdown(ctest: dict, suites: dict, tvf_rows: list[dict], tvf_covered: int, tvf_deferred: int,
                     sc1_summary, fc4_summary, e2e_results: list[dict], commit: str) -> str:
    lines = []
    # SNAPSHOT LABEL (Phase-6 gate review, Minor 1): this generator has no
    # knowledge of which commit produced its inputs, so it must never emit a
    # phase-scoped title. The output is a SNAPSHOT of whatever run was fed
    # in; the reader is told to pin it to a commit and to take the phase
    # total from the controller ledger instead.
    lines.append("# KAT/verification snapshot -- NOT a phase gate total")
    lines.append("")
    # The sha goes inside a code span; any caller-supplied note goes OUTSIDE it,
    # so a note containing its own backticks cannot terminate the span early.
    commit_sha, _, commit_note = commit.partition(" ")
    lines.append(f"> **Read this as a snapshot, pinned to `{commit_sha}`.** Every number below comes "
                 f"from ONE recorded `ctest` run captured at that commit. Cite it with the commit "
                 f"attached; take any phase-level total from the controller ledger's gate run "
                 f"rather than restating this one against a different tree.")
    if commit_note.strip():
        lines.append(">")
        lines.append("> " + commit_note.strip().lstrip("- ").strip())
    lines.append("")
    lines.append("Reproduce (task-25-brief.md carried minor, Task 23 review) -- with the machine "
                 "clear, one `ctest` at a time, serial:")
    lines.append("")
    lines.append("```")
    lines.append("ctest --test-dir build --no-tests=error 2>&1 | tee <ctest-log>")
    lines.append("python3 test/kat_results/gen_kat_results.py \\")
    lines.append("    --ctest-log <ctest-log> \\")
    lines.append(f"    --commit {commit_sha} \\")
    lines.append("    --build-dir build \\")
    lines.append("    --protocols-test-bin build/sympsica_tests_protocols \\")
    lines.append("    --task20-report .superpowers/sdd/sympsica-plan/task-20-report.md \\")
    lines.append("    --out docs/kat_results.md")
    lines.append("```")
    lines.append("")
    lines.append("**Generated by `test/kat_results/gen_kat_results.py` (task-23-brief.md R6-KATRES) "
                  "from real recorded output -- see that script's header for exactly which file each "
                  "number below comes from. `.handoff/` section 5 is the USER's own to update; this "
                  "file is the paste-ready source for it, not a substitute for it.**")
    lines.append("")

    lines.append("## Full suite (SC6)")
    lines.append("")
    if ctest["n_total"] is not None:
        lines.append(f"- `ctest --no-tests=error`: **{ctest['n_total'] - ctest['n_failed']}/"
                      f"{ctest['n_total']}** passed ({ctest['pct_passed']}%), "
                      f"{ctest['n_failed']} failed.")
    else:
        lines.append("- **could not parse the overall ctest summary line from the supplied "
                      "--ctest-log** -- see task-23-report.md for the raw log.")
    if ctest["total_time_s"] is not None:
        lines.append(f"- Total test time (real): **{ctest['total_time_s']:.1f}s**.")
    lines.append("")
    lines.append("| suite (test-name prefix) | tests | failed | time (s) |")
    lines.append("|---|---|---|---|")
    for suite in sorted(suites):
        s = suites[suite]
        lines.append(f"| {suite} | {s['n']} | {s['n_failed']} | {s['seconds']:.2f} |")
    lines.append("")

    lines.append("## TV-F negative registry (Task 20)")
    lines.append("")
    lines.append(f"**{tvf_covered} COVERED / {tvf_deferred} DEFERRED**, parsed directly from "
                  "task-20-report.md's own committed table (not re-run here -- see that report's "
                  "wrong-construction build dirs for each COVERED row's real evidence).")
    lines.append("")
    lines.append("| id | status | subject |")
    lines.append("|---|---|---|")
    for r in tvf_rows:
        lines.append(f"| {r['id']} | {r['status']} | {r['subject']} |")
    lines.append("")

    lines.append("## Task 23: full-transcript pool audit (W6.4/W6.6(ii))")
    lines.append("")
    lines.append("R6-AUDIT-TRANSCRIPT names four properties. Three are independently checked below "
                  "(reconciliation, consumed-side single-use, no-id-from-nowhere). The fourth "
                  "(cross-refill global uniqueness) is **not** independently re-verified by this audit "
                  "-- it cannot be, from `CorrelationPool<T>`'s public API alone (no accessor exposes "
                  "the full generated-id set). It is enforced as a precondition, at insertion time, by "
                  "`CorrelationPool<T>::refill` itself (`src/core/pools.cpp`): every incoming corr_id is "
                  "checked against both the available and already-consumed sets before being added, so "
                  "any duplicate -- within one refill batch, across calls, or against an already-consumed "
                  "id -- aborts the process before it is ever inserted. The results below RELY ON that "
                  "enforcement (reaching them at all means it held throughout the run) rather than "
                  "re-deriving it.")
    lines.append("")
    lines.append("### SC1 [AUDIT-UNIT] (unit scale, in-process)")
    lines.append("")
    if sc1_summary is not None:
        lines.append("```json")
        lines.append(json.dumps(sc1_summary, indent=2))
        lines.append("```")
    else:
        lines.append("- **not available** (protocols test binary not found or "
                      "PoolAudit.SC1_* produced no summary line -- see task-23-report.md).")
    lines.append("")
    lines.append("### FC4 [high-water is legal] (unit scale)")
    lines.append("")
    if fc4_summary is not None:
        lines.append("```json")
        lines.append(json.dumps(fc4_summary, indent=2))
        lines.append("```")
    else:
        lines.append("- **not available** -- see task-23-report.md.")
    lines.append("")

    lines.append("### SC2 [AUDIT-E2E] / SC3 [CGB-E2E] / SC4 [CGB-NONVACUOUS] (E2E scale, n=2^10)")
    lines.append("")
    for entry in e2e_results:
        lines.append(f"#### seed {entry['seed']}")
        lines.append("")
        if "cgb" in entry:
            cgb = entry["cgb"]
            lines.append(f"- CG-B: receiver snapshot={cgb['receiver_snapshot']} "
                          f"final={cgb['receiver_final']}; sender snapshot={cgb['sender_snapshot']} "
                          f"final={cgb['sender_final']}.")
        else:
            lines.append(f"- CG-B: **not available** ({entry.get('cgb_error')}).")
        if "audit" in entry:
            lines.append("")
            lines.append("| party | pool | generated | consumed | remaining | highest consumed id | "
                          "next_corr_id |")
            lines.append("|---|---|---|---|---|---|---|")
            for party in ("receiver", "sender"):
                for pool in ("triples", "gates"):
                    a = entry["audit"][party][pool]
                    lines.append(f"| {party} | {pool} | {a['generated']} | {a['consumed']} | "
                                  f"{a['remaining']} | {a['highest_consumed_id']} | {a['next_corr_id']} |")
        else:
            lines.append(f"- audit: **not available** ({entry.get('audit_error')}).")
        if "paths" in entry:
            lines.append("")
            path_str = ", ".join(f"{p['day']}={p['path']} (count={p['count']})" for p in entry["paths"])
            lines.append(f"- observed query-day paths (this run, real): {path_str}")
        lines.append("")

    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

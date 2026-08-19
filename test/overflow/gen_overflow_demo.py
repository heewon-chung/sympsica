#!/usr/bin/env python3
"""test/overflow/gen_overflow_demo.py — task-21-brief.md W6.3 (SC4/SC5,
R6-OVFDEMO/R6-OVFSCOPE): loads test/fixtures/overflow_planted.fixture,
recomputes the SAME planted-5-ids-in-one-bucket construction fresh via
ref/reference.py's Ref.build_overflow_demo_data() (the SAME helper the
fixture's own generator, Ref.emit_overflow_fixture, uses), and asserts:

  (a) the fresh computation matches the committed fixture EXACTLY -- catches
      numeric drift if a future change to Ref.minors/Ref.t_of/
      Ref.detect_overflow silently changes these numbers;
  (b) the fixture's own recorded values satisfy SC4/FC3/FC4 directly. This
      is what makes this SCRIPT "a test" (task-21-brief.md SC4: "a test
      asserts detect_overflow FLAGS the bucket at T'=7 and that the flag
      DISAPPEARS after refresh") -- registered as a native ctest add_test
      (CMakeLists.txt), the SAME precedent test/e2e/run_*.py and
      test/core/grep_guard_no_open_callsites.sh already establish for a
      Python-only check with no C++ production counterpart to call.

R6-OVFSCOPE (binding, restated here because this script is one of the files
that ruling names): this demo is PLAINTEXT/REFERENCE-ONLY. No MPC overflow
detector exists anywhere in this codebase -- src/protocols/salt.cpp's
OverflowChecker::check is a [[noreturn]] stub that always aborts. The real
protocol's T=4 no-overflow assumption (Params::T) is checked HERE, in the
clear, against ref/reference.py's Ref.detect_overflow -- never against a
running MPC circuit. This script's output, and the report it emits, must
never be read as evidence of an implemented MPC overflow detector.

On success, this script ALSO (re)generates docs/overflow_demo.md
(R6-DOCSDIR) with the real numbers from this run.

Invocation (the exact ctest-registered form):
  python3 test/overflow/gen_overflow_demo.py
Optional flags (used only for local debugging, never by the registered
test): --fixture PATH --out PATH
"""

from __future__ import annotations

import argparse
import os
import sys


def load_fixture(path: str) -> dict[str, list[str]]:
    rows: dict[str, list[str]] = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            toks = line.split()
            rows[toks[0]] = toks[1:]
    return rows


def main(argv: list[str]) -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.abspath(os.path.join(here, "..", ".."))

    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture", default=os.path.join(repo_root, "test/fixtures/overflow_planted.fixture"))
    parser.add_argument("--out", default=os.path.join(repo_root, "docs/overflow_demo.md"))
    args = parser.parse_args(argv)

    sys.path.insert(0, os.path.join(repo_root, "ref"))
    import reference as ref  # noqa: E402  (path must be set up first)

    rows = load_fixture(args.fixture)
    fx_bucket = int(rows["ovf_bucket"][0])
    fx_planted_ids = [int(x) for x in rows["ovf_planted_ids"]]
    fx_pre_d = [int(x) for x in rows["ovf_pre_d"]]
    fx_pre_rank = int(rows["ovf_pre_rank"][0])
    fx_pre_flagged = int(rows["ovf_pre_flagged"][0]) == 1
    fx_t4_rank = int(rows["ovf_t4_rank"][0])
    fx_t4_flagged = int(rows["ovf_t4_flagged"][0]) == 1
    fx_post_flagged_any = int(rows["ovf_post_flagged_any"][0]) == 1
    fx_post_bucket_present = int(rows["ovf_post_bucket_present"][0]) == 1

    errors: list[str] = []

    def check(name: str, got, want) -> None:
        if got != want:
            errors.append(f"{name}: fixture says {want!r}, fresh recompute says {got!r}")

    # (a) fresh recompute (real Ref.detect_overflow call, not the fixture's
    # own stored numbers) vs the committed fixture -- catches numeric drift.
    data = ref.Ref.build_overflow_demo_data()
    check("bucket", data["bucket"], fx_bucket)
    check("planted_ids", data["planted_ids"], fx_planted_ids)
    check("pre_d", data["pre_d"], fx_pre_d)
    check("pre_rank", data["pre_rank"], fx_pre_rank)
    check("pre_flagged", data["pre_flagged"], fx_pre_flagged)
    check("t4_rank", data["t4_rank"], fx_t4_rank)
    check("t4_flagged", data["t4_flagged"], fx_t4_flagged)
    check("post_flagged_any", data["post_flagged_any"], fx_post_flagged_any)
    check("post_bucket_present", data["post_bucket_present"], fx_post_bucket_present)

    # (b) SC4/FC3/FC4 -- the qualitative properties, checked directly on the
    # FIXTURE's own recorded values (what a reader of the committed fixture
    # actually sees), not only on the fresh recompute above.
    if not fx_pre_flagged:
        errors.append("SC4: fixture's pre-refresh Tprime=7 result must be FLAGGED")
    if fx_pre_rank != 5:
        errors.append(f"SC4: fixture's pre-refresh rank must be exactly 5, got {fx_pre_rank}")
    if fx_t4_flagged:
        errors.append("FC3: fixture's Tprime=4 result must NOT be flagged")
    if fx_t4_rank > 4:
        errors.append(f"FC3: fixture's Tprime=4 rank must be <= 4 (structural Hankel-depth bound), "
                       f"got {fx_t4_rank}")
    if fx_post_flagged_any:
        errors.append("FC4: fixture's post-refresh result must have NO overflowing buckets")
    if fx_post_bucket_present:
        errors.append("FC4: fixture's post-refresh detail must not still contain the planted bucket "
                       "(the 5 planted ids should have dispersed to their own buckets under the "
                       "refreshed oracle)")
    if not (fx_pre_flagged and fx_t4_rank <= 4 and not fx_t4_flagged):
        errors.append("FC4 precondition: the PRE-refresh state must genuinely flag before the "
                       "post-refresh clearing can be a real change of state, not a vacuous check")

    if errors:
        print("gen_overflow_demo.py: FAILED", file=sys.stderr)
        for e in errors:
            print("  - " + e, file=sys.stderr)
        return 1

    disclaimer = (
        "**This is a PLAINTEXT/REFERENCE-ONLY demonstration.** No MPC overflow "
        "detector exists anywhere in this codebase; `src/protocols/salt.cpp`'s "
        "`OverflowChecker::check` is a `[[noreturn]]` stub that always aborts. "
        "The real protocol's T=4 no-overflow assumption (`Params::T`) is checked "
        "HERE, in the clear, against `ref/reference.py`'s `Ref.detect_overflow` "
        "-- never against a running MPC circuit. This report must never be read "
        "as evidence of an implemented MPC overflow detector."
    )

    md_parts = [
        "# Overflow-detection demo (W6.3)\n",
        disclaimer + "\n",
        "## Setup\n",
        (f"5 ids are planted to collide in one synthetic bucket ({fx_bucket}): "
         f"`{fx_planted_ids}`. Every other id maps to itself (no incidental "
         "collisions). This construction is `ref/reference.py`'s "
         "`Ref.build_overflow_demo_data()`, the SAME planted case "
         "`_selftest_overflow()` already proves at import time (task-18-brief.md "
         "SC5/FC3) -- committed here as a named fixture "
         "(`test/fixtures/overflow_planted.fixture`) instead of only an inline "
         "self-test.\n"),
        "## Result: deep scan (T' = 7)\n",
        f"- Depth-7 syndrome vector `d_1..d_13` (2*T'-1 = 13 terms): `{fx_pre_d}`\n",
        f"- Recovered rank: **{fx_pre_rank}**\n",
        f"- Flagged overflowing: **{'YES' if fx_pre_flagged else 'NO'}** (T_CAP = Params::T = 4)\n",
        "## Result: no deep headroom (T' = 4)\n",
        (f"- Recovered rank: **{fx_t4_rank}** (a depth-(2*4-1)=7 Hankel test cannot report a rank "
         "above 4 -- structural, not incidental)\n"),
        f"- Flagged overflowing: **{'YES' if fx_t4_flagged else 'NO'}**\n",
        ("This is the demo's whole point: the true multiplicity (5) is invisible "
         "to a scan with no headroom beyond the real protocol's own depth; only "
         "the deep T'=7 scan sees past it.\n"),
        "## Result: after salt refresh\n",
        f"- Overflowing buckets found: **{'NONE' if not fx_post_flagged_any else 'SOME REMAIN'}**\n",
        (f"- Planted bucket ({fx_bucket}) still present in the scan: "
         f"**{'YES' if fx_post_bucket_present else 'NO'}** (the 5 planted ids disperse to 5 distinct "
         f"buckets under the refreshed oracle, so bucket {fx_bucket} is no longer touched at all)\n"),
        "\n---\n",
        ("Generated by `test/overflow/gen_overflow_demo.py` from "
         "`test/fixtures/overflow_planted.fixture` (generator invocation: "
         "`python3 ref/reference.py emit-ovf --out test/fixtures/overflow_planted.fixture`).\n"),
    ]

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        f.write("\n".join(md_parts))
    print(f"gen_overflow_demo.py: OK, wrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

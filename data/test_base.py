#!/usr/bin/env python3
"""data/test_base.py -- Phase 7 W7.3 gate for data/gen_base.py (stdlib unittest).
Full-scale n=2^22 runs in the hot path: measured 4.8 s (phase-7-plan.md ruling)."""
import os, subprocess, sys, tempfile, unittest

DATA_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, DATA_DIR)
import gen_base

P = (1 << 61) - 1


def check_filler_id(x):
    assert x >> 60 == 1 and x < (1 << 61), "filler id %d outside [2^60, 2^61)" % x
    assert x != P, "filler id equals p = 2^61-1 (reserved, non-canonical)"


def check_unique(ids, n):
    assert len(set(ids)) == n, (
        "base ids not unique: %d distinct of %d generated" % (len(set(ids)), n))


def check_determinism(a, b, other_seed_seq):
    assert a == b, "base_id not deterministic: same seed gave different sequences"
    assert a != other_seed_seq, \
        "seed separation broken: different seeds gave identical sequences"


def check_cli_output(lines, n, seed):
    assert len(lines) == n, "CLI wrote %d lines, expected %d" % (len(lines), n)
    assert int(lines[0]) == gen_base.base_id(seed, 0), "CLI line 0 != base_id(seed, 0)"
    assert int(lines[-1]) == gen_base.base_id(seed, n - 1), \
        "CLI last line != base_id(seed, n-1)"


class TestGenBase(unittest.TestCase):
    def test_full_scale_partition_and_uniqueness(self):
        n = 1 << 22
        ids = []
        for i in range(n):
            b = gen_base.base_id(1, i)
            check_filler_id(b)
            ids.append(b)
        check_unique(ids, n)   # observed fact at seed=1 (plan § Ground truth)

    def test_determinism_and_seed_separation(self):
        a = [gen_base.base_id(1, i) for i in range(4096)]
        b = [gen_base.base_id(1, i) for i in range(4096)]
        c = [gen_base.base_id(2, i) for i in range(4096)]
        check_determinism(a, b, c)

    def test_cli_small_n_roundtrip(self):
        n = 1 << 16
        with tempfile.TemporaryDirectory() as td:
            out = os.path.join(td, "base.txt")
            subprocess.run(
                [sys.executable, os.path.join(DATA_DIR, "gen_base.py"),
                 "--n", str(n), "--seed", "1", "--out", out], check=True)
            check_cli_output(open(out).read().splitlines(), n, 1)


if __name__ == "__main__":
    unittest.main()

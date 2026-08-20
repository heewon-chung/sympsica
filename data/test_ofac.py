#!/usr/bin/env python3
"""data/test_ofac.py -- Phase 7 W7.4 data gates (stdlib unittest, no pytest --
controller ruling, progress.md Phase-7 pre-flight). Registered in ctest as
data.OFC1 (task 29) / data.OFC2, data.OFC3 (task 30)."""
import os, sys, unittest

DATA_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, DATA_DIR)
import parse_ofac

RAW = os.path.join(DATA_DIR, "sdnnew23.txt")
WRONG_ABBREV = False   # set by the --wrong-abbrev CLI flag below (TV-F14 negative)


class TestOFC1(unittest.TestCase):
    def test_pinned_convention_stats(self):
        dates, days = parse_ofac.parse_raw(RAW, wrong_abbrev=WRONG_ABBREV)
        got = parse_ofac.stats(days)
        self.assertEqual(
            got, (114, 11499, 42, 181, 961),
            msg="OFC-1: (days,total,median,p90,max) diverge from the pinned "
                "parse convention -- see data/PROVENANCE.md for the pinned "
                "convention and percentile rule")


if __name__ == "__main__":
    if "--wrong-abbrev" in sys.argv:
        sys.argv.remove("--wrong-abbrev")
        WRONG_ABBREV = True
    unittest.main()

#!/usr/bin/env python3
"""bench/test_jsonl_check.py -- Phase 8 W8.7 gates (stdlib unittest; ctest
bench.Schema / bench.Counts / bench.Segments). Fixtures are inline; the
module-level GOOD_RECORD / GOOD_DERIVED / GOOD_KEY / GOOD_MARKERS are the
bases every R6-NOTAUTO mutation row in phase-8-plan.md mutates by ONE field."""
import copy
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jsonl_check as j

GOOD_RECORD = {
    "ts": "2026-08-21T12:00:00+00:00", "env": "LOCAL", "scenario": "smoke",
    "config": {"n": 65536, "u": 64, "network": "LAN", "protocol": "bms24", "variant": "add-only"},
    "trial": 0, "seed": 0, "status": "ok",
    "time_s": {"online": 12.5, "offline": 0.0, "total": 15.75},
    "bytes": {"r_out": 1000, "s_out": 500, "total": 1500, "external_total": 1500},
    "counters": {"triples": 0, "rounds": 0, "announced": 0},
    "notes": "threads=r:9,s:12;rss_kb=r:104220,s:98764;result=32;counters=na",
}
GOOD_DERIVED = {
    "ts": "2026-08-21T00:00:00+00:00", "env": "DERIVED", "scenario": "derived",
    "config": {"n": 1048576, "u": 16, "network": "200 Mbps", "protocol": "acns2147", "variant": "uPSI-CA"},
    "trial": 0, "seed": 0, "status": "ok",
    "time_s": {"online": 140.11, "offline": 0.0, "total": 140.11},
    "bytes": {"r_out": 0, "s_out": 0, "total": 1451600000, "external_total": 1451600000},
    "counters": {"triples": 0, "rounds": 0, "announced": 0},
    "notes": "table=T3;comm_mb_source=1451.6;counters=na;transcribed-not-measured",
    "measured": False, "source": "ACNS26-Tables3-4",
}
# A10: BMS+24's single shared-loopback-netns byte regime (bench/jsonl_check.py
# COMBINED_LOOPBACK_TOKEN) -- no per-party split; total/external_total come
# from the shared `lo`'s measured tx delta, not r_out+s_out.
GOOD_COMBINED = {
    "ts": "2026-08-22T00:00:00+00:00", "env": "AWS", "scenario": "calib",
    "config": {"n": 1048576, "u": 1024, "network": "LAN", "protocol": "bms24", "variant": "add-only"},
    "trial": 0, "seed": 0, "status": "ok",
    "time_s": {"online": 116.0, "offline": 0.0, "total": 120.0},
    "bytes": {"r_out": 0, "s_out": 0, "total": 45700000, "external_total": 45700000},
    "counters": {"triples": 0, "rounds": 0, "announced": 0},
    "notes": "result=18;bytes=combined-loopback;combined_total_b=45700000;counters=na",
}
GOOD_KEY = ("smoke", "bms24", "add-only", 65536, 64, "LAN")
GOOD_MARKERS = "\n".join([
    "PHASE:total_workload_begin:1000", "PHASE:setup_once_begin:1100", "PHASE:setup_once_end:1200",
    "PHASE:preprocessing_begin:1200", "PHASE:preprocessing_end:1200", "PHASE:online_begin:4200",
    "PHASE:online_end:9200", "PHASE:total_workload_end:9300"]) + "\n"


def R():
    return copy.deepcopy(GOOD_RECORD)


def D():
    return copy.deepcopy(GOOD_DERIVED)


def CB():
    return copy.deepcopy(GOOD_COMBINED)


class TestSchema(unittest.TestCase):
    def test_good_measured_and_derived_pass(self):
        j.validate_record(R())
        j.validate_record(D())
        j.validate_record(CB())

    def assertFails(self, rec, substr):
        with self.assertRaises(AssertionError) as cm:
            j.validate_record(rec)
        self.assertIn(substr, str(cm.exception))

    def test_negatives(self):
        r = R(); del r["bytes"]; self.assertFails(r, "missing top-level key 'bytes'")
        r = R(); r["measured"] = False; self.assertFails(r, "unknown top-level key 'measured'")
        r = R(); r["env"] = "COLIMA"; self.assertFails(r, "env 'COLIMA' not in")
        r = R(); r["config"]["network"] = "WAN100"; self.assertFails(r, "config.network 'WAN100' not in")
        r = R(); r["status"] = "crashed"; self.assertFails(r, "status 'crashed' not in")
        r = R(); r["bytes"]["total"] = 999; self.assertFails(r, "bytes.total 999 != r_out+s_out 1500")
        r = R(); r["bytes"]["external_total"] = 1499; self.assertFails(r, "bytes.external_total 1499 != bytes.total 1500")
        r = R(); r["time_s"]["online"] = "fast"; self.assertFails(r, "time_s.online must be a number")
        d = D(); del d["source"]; self.assertFails(d, "missing top-level key 'source'")
        d = D(); d["config"]["network"] = "WAN200"; self.assertFails(d, "verbatim network label")
        d = D(); d["bytes"]["r_out"] = 5; self.assertFails(d, "r_out and s_out must be 0")
        d = D(); d["bytes"]["external_total"] = 1; self.assertFails(d, "total == external_total == published")
        d = D(); d["measured"] = True; self.assertFails(d, "requires 'measured': false")
        d = D(); d["source"] = "x"; self.assertFails(d, "requires source 'ACNS26-Tables3-4'")
        c = CB(); c["bytes"]["r_out"] = 5; self.assertFails(c, "r_out and s_out must be 0 (A10)")
        c = CB(); c["bytes"]["external_total"] = 1; self.assertFails(c, "total == external_total (A10)")
        c = CB(); c["bytes"]["total"] = 0; c["bytes"]["external_total"] = 0; self.assertFails(c, "total > 0 (A10)")
        # I3 (Codex review, Task 34 fix round): validate_record must enforce the two-token
        # bridge at the file level too, not just build_record.
        c = CB(); c["notes"] = c["notes"].replace(";combined_total_b=45700000", ""); self.assertFails(c, "must be present together (A10)")
        r = R(); r["notes"] = r["notes"] + ";combined_total_b=999"; self.assertFails(r, "must be present together (A10)")
        c = CB(); c["notes"] = c["notes"].replace("combined_total_b=45700000", "combined_total_b=1"); self.assertFails(c, "must equal bytes.total 45700000 (A10)")
        # I3 (gate fix round 1, Task 6): the combined-loopback regime is
        # granted to bms24 only (A10) -- any other protocol carrying the
        # tokens is rejected at the schema level, not just by convention.
        c = CB(); c["config"]["protocol"] = "fastupsi"; self.assertFails(c, "combined-loopback regime is granted to bms24 only (A10); protocol=fastupsi")

    # I3 (gate fix round 1, Task 6): file-level mixed-regime refusal
    # (check_regime_consistency). Folded into TestSchema, not a separate
    # unittest.TestCase, so ctest's bench.Schema (CMakeLists.txt runs exactly
    # `test_jsonl_check.py TestSchema`) actually exercises it -- a standalone
    # class here would be invisible to T1 without a CMakeLists.txt change,
    # which is outside this fix round's authorized file list.
    def test_bms24_mixed_regime_rejected(self):
        combined = CB()
        two_sided = R(); two_sided["config"]["protocol"] = "bms24"
        with self.assertRaises(AssertionError) as cm:
            j.check_regime_consistency([combined, two_sided])
        self.assertIn("mixed byte regimes for protocol bms24 in one file", str(cm.exception))

    def test_bms24_combined_plus_fastupsi_twosided_accepted(self):
        combined = CB()
        fastupsi_two_sided = R(); fastupsi_two_sided["config"]["protocol"] = "fastupsi"
        j.check_regime_consistency([combined, fastupsi_two_sided])   # must NOT raise

    def test_same_regime_repeated_accepted(self):
        a, b = CB(), CB()
        j.check_regime_consistency([a, b])   # must NOT raise

    # I2 (gate fix round 1, Task 5): build_record's combined-loopback
    # internal-vs-external divergence check, scoped by which internal-counter
    # token is present (see build_record's own comment for the measured
    # evidence backing the combined/directional split). Same ctest-visibility
    # reason as above for folding these into TestSchema.
    def _build_bms24(self, tokens):
        cfg = {"scenario": "calib", "protocol": "bms24", "variant": "add-only", "n": 1, "u": 0,
               "network": "LAN", "seed": 0, "thread_regime": "runtime-helpers"}
        obs_ok = {"state": "observed", "threads_max": 1, "rss_kb": 1000}
        seg = {"total": 1.0, "online": 0.5, "preprocessing": 0.0, "setup_once": 0.0}
        return j.build_record(cfg, "AWS", "ok", seg, 0, 0, {"r": obs_ok, "s": obs_ok}, tokens, 0)

    def test_combined_scope_6pct_flags(self):
        rec = self._build_bms24(["bytes=combined-loopback", "combined_total_b=100", "internal_comm_b=106"])
        self.assertIn("bytes-divergence", rec["notes"])

    def test_combined_scope_exactly_5pct_does_not_flag(self):
        rec = self._build_bms24(["bytes=combined-loopback", "combined_total_b=100", "internal_comm_b=105"])
        self.assertNotIn("bytes-divergence", rec["notes"])

    def test_directional_both_present_summed_6pct_flags(self):
        rec = self._build_bms24(["bytes=combined-loopback", "combined_total_b=100", "internal_b_r=53", "internal_b_s=53"])
        self.assertIn("bytes-divergence", rec["notes"])

    def test_directional_only_one_present_is_unavailable(self):
        rec = self._build_bms24(["bytes=combined-loopback", "combined_total_b=100", "internal_b_r=53"])
        self.assertIn("internal-comparison=unavailable(scope-mismatch)", rec["notes"])
        self.assertNotIn("bytes-divergence", rec["notes"])

    def test_no_internal_counter_no_comparison(self):
        rec = self._build_bms24(["bytes=combined-loopback", "combined_total_b=100"])
        self.assertNotIn("bytes-divergence", rec["notes"])
        self.assertNotIn("unavailable(scope-mismatch)", rec["notes"])


class TestCounts(unittest.TestCase):
    def test_good(self):
        a, b = R(), R(); b["trial"] = 1
        j.check_counts([a, b], [GOOD_KEY], trials=2)

    def assertFails(self, recs, keys, trials, substr):
        with self.assertRaises(AssertionError) as cm:
            j.check_counts(recs, keys, trials)
        self.assertIn(substr, str(cm.exception))

    def test_negatives(self):
        self.assertFails([R(), R()], [GOOD_KEY], 1, "duplicate record for")
        self.assertFails([R()], [GOOD_KEY], 2, "has trials [0], wanted 0..1")
        r = R(); r["status"] = "error"
        self.assertFails([r], [GOOD_KEY], 1, "not allowed by HSTx3")
        r = R(); r["config"]["u"] = 65
        self.assertFails([r], [GOOD_KEY], 1, "unexpected cell")


class TestSegments(unittest.TestCase):
    def test_good(self):
        s = j.parse_segments(GOOD_MARKERS)
        self.assertEqual(s["total"], 8.3e-6)
        self.assertEqual(s["online"], 5e-6)
        self.assertEqual(s["preprocessing"], 0.0)

    def assertFails(self, text, substr):
        with self.assertRaises(AssertionError) as cm:
            j.parse_segments(text)
        self.assertIn(substr, str(cm.exception))

    def test_negatives(self):
        self.assertFails(GOOD_MARKERS.replace("PHASE:total_workload_end:9300\n", ""), "marker total_workload_end absent")
        self.assertFails(GOOD_MARKERS.replace("PHASE:online_end:9200\n", ""), "marker online_end absent")
        self.assertFails(GOOD_MARKERS + "PHASE:online_end:9250\n", "appears more than once")
        self.assertFails(GOOD_MARKERS.replace("online_begin:4200", "online_begin:9250"), "not monotonic")

    def test_incomplete_for_timeout_synthesis(self):
        s = j.parse_segments("PHASE:total_workload_begin:1000\nPHASE:setup_once_begin:1100\n", allow_incomplete=True)
        self.assertIsNone(s["online"])
        self.assertEqual(s["_ns"]["total_workload_begin"], 1000)

    def test_divergence_strict_5pct(self):
        self.assertFalse(j.divergence_flag(105, 100))
        self.assertTrue(j.divergence_flag(106, 100))
        self.assertFalse(j.divergence_flag(0, 0))
        # I2 (gate fix round 1, Task 5): a zero external denominator with a
        # real internal counter must no longer silently return False.
        with self.assertRaises(ValueError):
            j.divergence_flag(5, 0)


if __name__ == "__main__":
    unittest.main()

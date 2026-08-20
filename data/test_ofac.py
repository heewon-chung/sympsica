#!/usr/bin/env python3
"""data/test_ofac.py -- Phase 7 W7.4 data gates (stdlib unittest, no pytest --
controller ruling, progress.md Phase-7 pre-flight). Registered in ctest as
data.OFC1 (task 29) / data.OFC2, data.OFC3 (task 30)."""
import os, sys, unittest
import json, glob

DATA_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, DATA_DIR)
import parse_ofac
import gen_base

RAW = os.path.join(DATA_DIR, "sdnnew23.txt")
WRONG_ABBREV = False   # set by the --wrong-abbrev CLI flag below (TV-F14 negative)

DELTAS_DIR = os.path.join(DATA_DIR, "deltas")
P = (1 << 61) - 1


def load_deltas():
    """Committed per-day deltas, sorted by ISO date (== filename sort)."""
    out = []
    for path in sorted(glob.glob(os.path.join(DELTAS_DIR, "2023-*.json"))):
        with open(path) as f:
            doc = json.load(f)
        out.append((doc["date"], doc["add"], doc["del"]))
    return out


def load_mapping():
    with open(os.path.join(DELTAS_DIR, "mapping.json")) as f:
        return json.load(f)


def absent_real_id(deltas):
    """Deterministic real-range id guaranteed absent from every add list:
    the smallest non-negative integer not present in any add list. (Add ids
    are ~uniform 60-bit hashes, so this is 0 in practice; the loop makes the
    guarantee unconditional. Always < 2^60: at most len(adds_all)+1.)"""
    adds_all = set()
    for _, adds, _ in deltas:
        adds_all.update(adds)
    x = 0
    while x in adds_all:
        x += 1
    return x


def check_day_count(deltas):
    assert len(deltas) == 114, (
        "OFC-2: expected 114 committed delta files, found %d -- see "
        "data/PROVENANCE.md" % len(deltas))


def ofc2_check(day_counts, u_max=1024):
    m = max(day_counts)
    assert m <= u_max, (
        "OFC-2: a day carries %d ops > u_max=%d -- the OFAC 2023 stream must fit "
        "the per-day update budget (see data/PROVENANCE.md)" % (m, u_max))


def check_max_exact(day_counts):
    m = max(day_counts)
    assert m == 961, (
        "OFC-2: observed per-day max %d drifted from the pinned 961 -- see "
        "data/PROVENANCE.md" % m)


def ofc3_replay_check(deltas, mapping_doc):
    base_seed = mapping_doc["base_seed"]
    mapped = {}   # base_id -> base_index
    for orig_id, idx, bid in mapping_doc["map"]:
        # Totality at scale: every mapped index must sit below 2^16 <= n_base,
        # so any base population with n >= 2^16 (tests) or 2^22 (full) contains
        # every mapped member. Checked FIRST so it is independently trippable.
        assert idx < (1 << 16), (
            "OFC-3: mapped base index %d >= 2^16 -- totality at test scale broken" % idx)
        assert bid == gen_base.base_id(base_seed, idx), \
            "OFC-3: mapping row disagrees with base_id"
        assert bid not in mapped, "OFC-3: base member mapped twice"
        mapped[bid] = idx
    live = set()
    used = set()
    for date, adds, dels in deltas:
        for x in dels:
            if x >> 60:            # filler/base id
                assert x in mapped, (
                    "OFC-3: base-range del %d not in mapping.json (mapping not total)" % x)
                assert x not in used, "OFC-3: base member %d deleted twice" % x
                used.add(x)
            else:                  # real id
                assert x in live, (
                    "OFC-3: delete of never-inserted real id %d on %s" % (x, date))
                live.discard(x)
        for x in adds:
            live.add(x)
    assert len(used) == len(mapped) == mapping_doc["unknown_del_count"], \
        "OFC-3: mapping count mismatch"


def ofc3_regen_check(raw_path, committed_deltas, committed_mapping_doc):
    dates, days = parse_ofac.parse_raw(raw_path)
    per_day, mapping, c = parse_ofac.map_days(dates, days, base_seed=1)
    regen = [(d["date"], d["add"], d["del"]) for d in per_day]
    assert regen == committed_deltas, (
        "OFC-3: committed data/deltas/ diverge from a fresh deterministic "
        "regeneration -- see data/PROVENANCE.md")
    assert committed_mapping_doc == {"base_seed": 1, "unknown_del_count": c,
                                     "map": mapping}, (
        "OFC-3: committed mapping.json diverges from the fresh map_days mapping "
        "(orig_id column and assignment order included) -- see data/PROVENANCE.md")
    assert c == 1800, "OFC-3: unknown-del count %d != pinned 1800" % c


class TestOFC1(unittest.TestCase):
    def test_pinned_convention_stats(self):
        dates, days = parse_ofac.parse_raw(RAW, wrong_abbrev=WRONG_ABBREV)
        got = parse_ofac.stats(days)
        self.assertEqual(
            got, (114, 11499, 42, 181, 961),
            msg="OFC-1: (days,total,median,p90,max) diverge from the pinned "
                "parse convention -- see data/PROVENANCE.md for the pinned "
                "convention and percentile rule")


class TestOFC2(unittest.TestCase):
    def test_no_day_exceeds_u_max_1024(self):
        deltas = load_deltas()
        check_day_count(deltas)
        counts = [len(a) + len(d) for (_, a, d) in deltas]
        ofc2_check(counts)          # the <= u_max claim (headroom: 961 <= 1024)
        check_max_exact(counts)     # drift guard: exact pinned observed max


class TestOFC3(unittest.TestCase):
    def test_committed_deltas_and_mapping_equal_regenerated(self):
        ofc3_regen_check(RAW, load_deltas(), load_mapping())

    def test_replay_consistent_and_mapping_total(self):
        ofc3_replay_check(load_deltas(), load_mapping())


if __name__ == "__main__":
    if "--wrong-abbrev" in sys.argv:
        sys.argv.remove("--wrong-abbrev")
        WRONG_ABBREV = True
    unittest.main()

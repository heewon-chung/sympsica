#!/usr/bin/env python3
"""baselines/acns2147/test_derived.py -- W8.6 (phase-8-plan.md § 32.6): the
2025/2147 Tables-3/4 transcription gate. Validates derived.py's 48 DERIVED
rows against the harness schema (bench/jsonl_check.py) and pins six cells
plus the output's SHA-256, so a future edit to derived.py's tables is caught
immediately. check_line_count/check_cell/check_sha are plain module-level
functions (not wrapped in TestCase methods) precisely so R6-NOTAUTO's N6/N7/N8
negatives can call them directly, unwrapped, from the command line."""
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(THIS_DIR))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(REPO_ROOT, "bench"))
import derived            # noqa: E402  (this dir)
import jsonl_check as j   # noqa: E402  (<repo>/bench)

DERIVED_PY = os.path.join(THIS_DIR, "derived.py")
PINNED_SHA256 = "ef4b2465c4952a6da628dab32e4e3fbc3a59b22a35a46e80f4b7bc9ea5159539"


def check_line_count(lines):
    assert len(lines) == 48, "2147: expected 48 rows, got %d" % len(lines)


def _field(rec, field):
    if field == "online":
        return rec["time_s"]["online"]
    if field == "offline":
        return rec["time_s"]["offline"]
    if field == "bytes.total":
        return rec["bytes"]["total"]
    raise AssertionError("2147: unknown field '%s'" % field)


def check_cell(rows, table, variant, u, network, field, expected):
    """`table` is read from the row's `table=` notes token (scenario is
    always "derived" -- amendment A3 keeps the table OUT of scenario)."""
    matches = [r for r in rows
               if r["config"]["variant"] == variant and r["config"]["u"] == u
               and r["config"]["network"] == network and ("table=%s" % table) in r["notes"]]
    assert len(matches) == 1, \
        "2147: cell (%s,%s,%s,%s,%s) matched %d rows, want 1" % (table, variant, u, network, field, len(matches))
    got = _field(matches[0], field)
    assert got == expected, \
        "2147: cell (%s,%s,%s,%s,%s) = %s != pinned %s" % (table, variant, u, network, field, got, expected)


def check_sha(path):
    with open(path, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    assert digest == PINNED_SHA256, "2147: output sha256 %s != pinned %s" % (digest, PINNED_SHA256)


class TestDerived(unittest.TestCase):
    def test_line_count_schema_and_sha(self):
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "derived.jsonl")
            subprocess.run([sys.executable, DERIVED_PY, "--out", out], check=True)
            with open(out) as f:
                lines = [line for line in f if line.strip()]
            check_line_count(lines)
            for line in lines:
                j.validate_record(json.loads(line))
            check_sha(out)

    def test_pinned_cells(self):
        rows = derived.rows()
        check_cell(rows, "T3", "uPSI(not CA)", 16, "LAN", "online", 96.52)
        check_cell(rows, "T3", "uPSI-CA", 64, "5 Mbps", "online", 2458.83)
        check_cell(rows, "T4", "uPSI-CA", 1024, "LAN", "offline", 33.45)
        check_cell(rows, "T3", "uPSI(not CA)", 16, "LAN", "bytes.total", 1525530000)
        check_cell(rows, "T3", "uPSI-CA", 16, "LAN", "bytes.total", 1451600000)
        check_cell(rows, "T4", "uPSI(not CA)", 1024, "LAN", "online", 108.6)


if __name__ == "__main__":
    unittest.main()

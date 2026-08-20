#!/usr/bin/env python3
"""data/parse_ofac.py -- Phase 7 W7.1/W7.2/W7.4: the OFAC SDN 2023 changes-
file parser, pinned stats gate, and committed-delta emitter.

Pinned parse convention (.handoff/sympsica-plan.md W7.1, verbatim):

    an entry ends at a line whose last non-whitespace char is '.'; count
    every entry under add/remove/changed action-block headers (headers
    accumulate wrapped lines until the terminating ':'); every `changed`
    entry counts as delete + add (2 ops); NO abbreviation filtering.

See data/PROVENANCE.md for the full pinned convention (percentile rule,
entity-id construction, changed-entry splitting, unknown-del mapping) and
the source file's provenance (URL, access date, SHA-256).
"""
import argparse
import hashlib
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_base import base_id

DATE_RE = re.compile(r"^\s*(\d{2}/\d{2}/\d{2}):\s*$")   # e.g. "01/05/23:"

ABBREV = {'a.k.a.', 'f.k.a.', 'n.k.a.', 'No.', 'Ltd.', 'Co.', 'Inc.', 'St.',
          'Jr.', 'U.S.', 'S.A.', 'LLC.', 'Corp.'}       # wrong-construction ONLY


def last_nonws(s):
    t = s.rstrip()
    return t[-1] if t else ""


def classify(header_text):            # precedence order is load-bearing
    hl = header_text.lower()
    if "changed" in hl:
        return "chg"
    if "removed" in hl or "deleted" in hl:
        return "del"
    if "added" in hl:
        return "add"
    return None


def entity_id(entry_text):
    return int.from_bytes(
        hashlib.blake2b(entry_text.encode('utf-8'), digest_size=8).digest(),
        'little') & ((1 << 60) - 1)


def parse_raw(path, wrong_abbrev=False):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    lines = text.split("\n")

    cur_date = None
    mode = None
    in_header = False
    hbuf = []
    ebuf = []
    dates = []
    days = {}

    for raw in lines:
        m = DATE_RE.match(raw)
        if m:
            mm, dd, yy = m.group(1).split("/")
            if yy != "23":
                sys.stderr.write("parse_ofac: unexpected year '%s' in date header %r\n" % (yy, raw))
                sys.exit(1)
            iso = "20%s-%s-%s" % (yy, mm, dd)
            if iso not in days:
                dates.append(iso)
                days[iso] = []
            cur_date = iso
            mode = None
            in_header = False
            hbuf = []
            ebuf = []
            continue

        if not raw.strip():
            continue

        if cur_date is None:
            continue

        if not ebuf and not in_header and raw.lstrip().lower().startswith("the following"):
            hbuf = [raw.strip()]
            in_header = True
            if last_nonws(raw) == ":":
                mode = classify(" ".join(hbuf))
                in_header = False
                hbuf = []
            continue

        if in_header:
            hbuf.append(raw.strip())
            if last_nonws(raw) == ":":
                mode = classify(" ".join(hbuf))
                in_header = False
                hbuf = []
            continue

        if mode is None:
            continue

        # Entry accumulation.
        ebuf.append(raw.strip())
        terminates = (last_nonws(raw) == ".")
        if wrong_abbrev and terminates:
            tokens = raw.strip().split()
            if tokens and tokens[-1] in ABBREV:
                terminates = False
        if terminates:
            entry_text = " ".join(ebuf)
            ebuf = []
            if mode == 'chg':
                if " -to- " in entry_text:
                    old, new = entry_text.split(" -to- ", 1)
                else:
                    old = new = entry_text
                days[cur_date].append(("del", entity_id(old)))
                days[cur_date].append(("add", entity_id(new)))
            else:
                days[cur_date].append((mode, entity_id(entry_text)))

    if dates != sorted(dates):
        sys.stderr.write("parse_ofac: date headers are not in ascending first-seen order\n")
        sys.exit(1)

    return dates, days


def pinned_median(counts):
    s = sorted(counts)
    n = len(s)
    mid = s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2
    return round(mid)          # Python round(): banker's rounding


def pinned_p90(counts):
    s = sorted(counts)
    n = len(s)
    x = 0.9 * (n - 1)
    lo = int(x)
    frac = x - lo
    v = s[lo] + frac * (s[lo + 1] - s[lo]) if lo + 1 < n else s[lo]
    return round(v)


def stats(days):
    counts = [len(v) for v in days.values()]
    return (len(days), sum(counts), pinned_median(counts), pinned_p90(counts), max(counts))


def map_days(dates, days, base_seed=1):
    live = set()
    mapping = []
    c = 0
    per_day = []
    for d in dates:
        ops = days[d]
        dels = [i for (o, i) in ops if o == 'del']
        adds = [i for (o, i) in ops if o == 'add']
        emitted_dels = []
        for x in dels:
            if x in live:
                live.discard(x)
                emitted_dels.append(x)
            else:
                b = base_id(base_seed, c)
                mapping.append([x, c, b])
                c += 1
                emitted_dels.append(b)
        for x in adds:
            live.add(x)
        per_day.append({"date": d, "add": adds, "del": emitted_dels})
    return per_day, mapping, c


def write_deltas(per_day, mapping, c, base_seed, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    for day in per_day:
        path = os.path.join(out_dir, "%s.json" % day["date"])
        with open(path, "w") as f:
            f.write(json.dumps(day) + "\n")
    mapping_path = os.path.join(out_dir, "mapping.json")
    with open(mapping_path, "w") as f:
        f.write(json.dumps({"base_seed": base_seed, "unknown_del_count": c, "map": mapping}) + "\n")


def main():
    ap = argparse.ArgumentParser(description="Parse the OFAC SDN 2023 changes file.")
    ap.add_argument("--raw", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "sdnnew23.txt"))
    ap.add_argument("--out-dir")
    ap.add_argument("--base-seed", type=int, default=1)
    ap.add_argument("--wrong-abbrev", action="store_true")
    args = ap.parse_args()

    if args.wrong_abbrev and args.out_dir:
        sys.stderr.write("refusing to emit deltas from the wrong-construction parse\n")
        sys.exit(2)

    dates, days = parse_raw(args.raw, wrong_abbrev=args.wrong_abbrev)
    d, t, med, p90, mx = stats(days)
    print("days=%d total=%d median=%d p90=%d max=%d" % (d, t, med, p90, mx))

    if args.out_dir:
        per_day, mapping, c = map_days(dates, days, base_seed=args.base_seed)
        write_deltas(per_day, mapping, c, args.base_seed, args.out_dir)
        print("wrote %d delta files + mapping.json to %s (unknown_dels=%d)" % (len(dates), args.out_dir, c))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""baselines/acns2147/derived.py -- W8.6: transcription of eprint 2025/2147
Tables 3 and 4 (ACNS'26, Alborch et al.) into DERIVED JSONL rows. Source of
every number: ACNS2147-EVIDENCE.md + acns2147-extracted.txt lines 918-947
(PDF sha256 d479dccb9a7cee809321bce6c076e1d1d8fadaf015838222338ba1580686d172,
fetched 2026-08-21T01:49:40Z, 701,656 B; see PROVENANCE.md). Protocol 2 =
Pi_uPSI-card (the CA variant); Protocol 1 = full uPSI, labeled "uPSI(not CA)".
Network labels are the source's VERBATIM column labels ("200 Mbps" with the
space; bandwidth-only for WAN; the tables never restate an RTT) -- controller
amendment A1; scenario is "derived" with the table in notes -- amendment A3.
Table 2 is OUT of Phase-8 scope. Stdlib only; output is byte-deterministic."""
import argparse
import json

TS = "2026-08-21T00:00:00+00:00"
NETS = ("LAN", "200 Mbps", "50 Mbps", "5 Mbps")   # VERBATIM source column labels (A1)
VARIANT = {"P1": "uPSI(not CA)", "P2": "uPSI-CA"}

# (protocol, alpha, comm_mb_str, online LAN, 200 Mbps, 50 Mbps, 5 Mbps)  -- n = 2^20
TABLE3 = (
    ("P1", 16, "1525.53", 96.52, 150.50, 319.10, 2489.80),
    ("P1", 32, "1526.5", 98.67, 140.44, 307.39, 2492.97),
    ("P1", 64, "1528.72", 100.66, 157.01, 309.14, 2495.41),
    ("P2", 16, "1451.6", 90.54, 140.11, 305.08, 2370.96),
    ("P2", 32, "1487", 93.74, 136.93, 295.56, 2432.86),
    ("P2", 64, "1506.98", 96.32, 147.65, 302.41, 2458.83),
)
# (protocol, alpha, comm_mb_str, offline_s_str, online LAN, 200 Mbps, 50 Mbps, 5 Mbps) -- n ~ 2^18 (prose)
TABLE4 = (
    ("P1", 64, "380.85", "9.29", 20.51, 35.4, 77.92, 616.73),
    ("P1", 256, "393.96", "14.1", 39.43, 53.96, 98.39, 656.47),
    ("P1", 1024, "443.1", "33.45", 108.6, 126.13, 170.59, 796.25),
    ("P2", 64, "374.23", "9.29", 14.27, 26.93, 70.12, 601.85),
    ("P2", 256, "387.85", "14.1", 14.31, 32.12, 71.52, 624.39),
    ("P2", 1024, "426.48", "33.45", 14.67, 32.35, 76.97, 686.11),
)
NOTES_COMMON = ("counters=na;transcribed-not-measured;"
                "substrate=i7-7800X-128GB-Ubuntu20.04-two-THREADS-over-localhost-tc;"
                "rtt_caveat=source-table-states-bandwidth-only")


def rows():
    out = []
    for proto, alpha, comm, *online in TABLE3:
        for net, t in zip(NETS, online):
            out.append(_rec("T3", 1 << 20, proto, alpha, net, comm, None, t))
    for proto, alpha, comm, off, *online in TABLE4:
        for net, t in zip(NETS, online):
            out.append(_rec("T4", 1 << 18, proto, alpha, net, comm, off, t))
    return out


def _rec(table, n, proto, alpha, net, comm_mb, offline_s, online_s):
    byts = int(round(float(comm_mb) * 1e6))
    off = float(offline_s) if offline_s is not None else 0.0
    notes = "table=%s;comm_mb_source=%s;offline_s_source=%s;%s" % (table, comm_mb, offline_s if offline_s is not None else "na", NOTES_COMMON)
    if table == "T4":
        notes += ";n_source=prose-approx"
    return {"ts": TS, "env": "DERIVED", "scenario": "derived",   # A3: the table rides in notes, never in scenario
            "config": {"n": n, "u": alpha, "network": net, "protocol": "acns2147", "variant": VARIANT[proto]},
            "trial": 0, "seed": 0, "status": "ok",
            "time_s": {"online": online_s, "offline": off, "total": round(online_s + off, 6)},
            "bytes": {"r_out": 0, "s_out": 0, "total": byts, "external_total": byts},
            "counters": {"triples": 0, "rounds": 0, "announced": 0},
            "notes": notes, "measured": False, "source": "ACNS26-Tables3-4"}


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--out", required=True)
    a = ap.parse_args()
    with open(a.out, "w") as f:
        for r in rows():
            f.write(json.dumps(r, separators=(",", ":")) + "\n")
    print("wrote %d derived rows to %s" % (len(rows()), a.out))


if __name__ == "__main__":
    main()

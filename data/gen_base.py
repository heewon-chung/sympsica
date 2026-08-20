#!/usr/bin/env python3
"""data/gen_base.py -- Phase 7 W7.3: the synthetic base-population generator.

Authority: .handoff/sympsica-plan.md W7.3 + phase-7-plan.md section D
("Base id (filler ids, bit 60 = 1)"). Filler ids are counter-based
(index-addressable, no global state): base_id(seed, i) is deterministic from
(seed, i) alone, so data/parse_ofac.py can compute unknown-del mappings
without ever generating the full population. Range partition: filler ids
have bit 60 SET and low 60 bits uniform over [0, 2^60-1) -- the single value
low60 == 2^60-1 is rejected because (1<<60) | (2^60-1) == 2^61-1 == p (the
field modulus; non-canonical on the wire). Stdlib hashlib.blake2b only (no
BLAKE3 module available -- same ruling as the entity-id convention; no
cross-language equality is required for base ids).
"""
import argparse
import hashlib
import sys


def base_id(seed, i):
    j = 0
    while True:
        h = hashlib.blake2b(b"sympsica-base:%d:%d:%d" % (seed, i, j),
                             digest_size=8).digest()
        low60 = int.from_bytes(h, 'little') & ((1 << 60) - 1)
        if low60 != (1 << 60) - 1:      # rejection: id == 2^61-1 would equal p (reserved)
            return (1 << 60) | low60
        j += 1


def main():
    parser = argparse.ArgumentParser(description="Generate the synthetic base population.")
    parser.add_argument("--n", type=int, default=4194304)  # 2^22
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    with open(args.out, "w") as f:
        for i in range(args.n):
            f.write("%d\n" % base_id(args.seed, i))

    print("wrote %d base ids (seed=%d) to %s" % (args.n, args.seed, args.out))


if __name__ == "__main__":
    main()

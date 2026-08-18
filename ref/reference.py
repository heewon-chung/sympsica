#!/usr/bin/env python3
"""ref/reference.py — plaintext reference implementation and fixture emitter
for sympsica's Phase-1 (utils) KAT suite (task-3-brief.md, W1.7).

Pure Python, bigints only (no numpy needed for cut 1). This module is the
golden source for the field/encoding test vectors documented in
.handoff/sympsica-test-vectors.md: every [SEED-FIXED] and cross-language
[IDENTITY] row in test/utils/kat_field.cpp and test/utils/kat_encoding.cpp
either compares directly against a value computed here, or against a
fixture file emitted by `emit_fixtures` (see test/fixtures/README.md for the
on-disk format and the exact generator invocation used to produce each
committed fixture).

Global constants (task-2 brief / task-3-brief.md, verbatim):
    p = 2^61 - 1 = 2305843009213693951
    all Fp canonical in [0, p)
    wire = 64-bit LE

Pinned PRNG: splitmix64 (Sebastiano Vigna, public domain), used identically
here and in test/utils/pinned_prng.hpp so that a fixture's (prng_seed, count)
pair regenerates the exact same input sequence in both languages without
having to ship the raw samples. See splitmix64_next() below; the C++ mirror
must match it bit-for-bit (state/mixing constants, 64-bit wraparound).
"""

from __future__ import annotations

import argparse
import sys
from typing import Iterable, Sequence

P = 2305843009213693951  # 2^61 - 1
MASK64 = (1 << 64) - 1

# Unique prime factors of p-1 = 2 * 3^2 * 5^2 * 7 * 11 * 13 * 31 * 41 * 61 *
# 151 * 331 * 1321 (same list as src/utils/encoding.cpp's kFpMinusOneFactors
# — fixed, given verbatim; multiplicity does not matter for the generator
# test g^((p-1)/q) != 1). Recomputed independently here in Python bigint per
# the controller ruling: reference.py must cross-check the C++-found
# generator, not just assume it.
FP_MINUS_ONE_FACTORS = (2, 3, 5, 7, 11, 13, 31, 41, 61, 151, 331, 1321)

# splitmix64 mixing constants (Vigna, public domain).
_SM64_GOLDEN = 0x9E3779B97F4A7C15
_SM64_MUL1 = 0xBF58476D1CE4E5B9
_SM64_MUL2 = 0x94D049BB133111EB

# Word-wise FNV-1a-style 64-bit digest, used to compactly summarize a
# 10^6-length sequence of expected Fp products without shipping the raw
# values (task-3-brief.md, requirement 1: "a Python-computed digest/checksum
# of expected products, so the JSON stays small"). Must match
# test/utils/pinned_prng.hpp's digest_update() bit-for-bit.
_FNV_OFFSET = 0xCBF29CE484222325
_FNV_PRIME = 0x100000001B3

FIXTURE_FORMAT_VERSION = 1


def splitmix64_next(state: int) -> tuple[int, int]:
    """One step of splitmix64: advances `state`, returns (new_state, output)."""
    state = (state + _SM64_GOLDEN) & MASK64
    z = state
    z = ((z ^ (z >> 30)) * _SM64_MUL1) & MASK64
    z = ((z ^ (z >> 27)) * _SM64_MUL2) & MASK64
    z = z ^ (z >> 31)
    return state, z


def fp_from_u64(x: int) -> int:
    """Mirrors Fp::from_u64 (field.hpp): the Mersenne double-fold reduction
    of an arbitrary 64-bit value into the canonical range [0, P).
    """
    assert 0 <= x <= MASK64
    r = (x & P) + (x >> 61)
    r = (r & P) + (r >> 61)
    if r >= P:
        r -= P
    assert r == x % P, "fp_from_u64 double-fold disagrees with bigint mod"
    return r


def digest_update(digest: int, x: int) -> int:
    digest = (digest ^ (x & MASK64)) & MASK64
    digest = (digest * _FNV_PRIME) & MASK64
    return digest


def digit_split(r: int, num_digits: int = 4, digit_bits: int = 16,
                 top_digit_bits: int = 13, p: int = P) -> list[int]:
    """Mirrors test/integration/ztgate_pipeline.cpp's digit_split(): a
    carry-less little-endian digit slice of the low 61 bits of `r`. `p` (=
    2^61-1) doubles as the 61-bit mask since its binary form is 61 one-bits,
    exactly like the C++ side's `v &= kRejectedMask`.
    """
    v = r & p
    d = [(v >> (digit_bits * j)) & ((1 << digit_bits) - 1) for j in range(num_digits - 1)]
    d.append((v >> (digit_bits * (num_digits - 1))) & ((1 << top_digit_bits) - 1))
    return d


def dpf_oracle(domain: int, point: int, payload: int, p: int = P) -> list[int]:
    """W2.3 plan text, verbatim: the PLAINTEXT point-function oracle —
    f(x) = payload iff x == point, else 0, for x in [0, domain). Returns the
    full length-`domain` table so there is exactly one place (this function)
    the oracle's semantics live; callers needing only f(x) for one x should
    index the result rather than reimplement the predicate.
    """
    assert 0 <= point < domain, f"point {point} out of [0, {domain})"
    assert 0 <= payload < p, f"payload {payload} not a canonical Fp value"
    return [payload if x == point else 0 for x in range(domain)]


def find_generator(p: int = P, factors: Sequence[int] = FP_MINUS_ONE_FACTORS) -> int:
    """Smallest generator of F_p^*: smallest g >= 2 with g^((p-1)/q) != 1
    (mod p) for every prime q dividing p-1. Independent Python-bigint
    reimplementation of Encoder::Encoder() (src/utils/encoding.cpp), used
    to cross-check the C++-computed generator before it is pinned as a
    golden (ENC-5, [POST-GATE]).
    """
    candidate = 2
    while True:
        if all(pow(candidate, (p - 1) // q, p) != 1 for q in factors):
            return candidate
        candidate += 1


class Ref:
    """Plaintext reference for sympsica's power-sum / syndrome primitives
    (task-3-brief.md W1.7, verbatim signatures).
    """

    P = P

    @staticmethod
    def sigma(id_: int, g: int, p: int = P) -> int:
        return pow(g, id_, p)

    @staticmethod
    def power_sums(ids: Iterable[int], g: int, depth: int, p: int = P) -> list[int]:
        """list of sums of sigma(id)^k, k = 1..depth, mod p (sigma = g^id)."""
        sigmas = [Ref.sigma(i, g, p) for i in ids]
        return [sum(pow(s, k, p) for s in sigmas) % p for k in range(1, depth + 1)]

    @staticmethod
    def syndromes(A: Iterable[int], B: Iterable[int], g: int, depth: int, p: int = P) -> list[int]:
        """power_sums(A) - power_sums(B) mod p, per depth."""
        sa = Ref.power_sums(A, g, depth, p)
        sb = Ref.power_sums(B, g, depth, p)
        return [(a - b) % p for a, b in zip(sa, sb)]

    @staticmethod
    def emit_fixtures(seed: int, out_path: str) -> None:
        """Emits the Phase-1 fixture file for `seed` to `out_path`.

        Content (line-based format — see test/fixtures/README.md for the
        full spec; the "out.json" name in task-3-brief.md's plan text is
        kept only as the CLI flag's historical label, see requirement 3's
        explicit carve-out for a non-JSON line format):

          (a) FLD-4 boundary-reduction golden rows (deterministic, not
              seed-dependent — computed once per file for completeness).
          (b) mul associativity/canonicity property-test fixture: a pinned
              splitmix64 seed + sample count + a digest of the expected
              product sequence (10^6 samples by default at Phase-1 SC).
          (c) the independently cross-checked generator g.
          (d) TBL-1 fixtures: a two-id power-sum row (future phase; nothing
              consumes this yet, emitted now per plan text).
        """
        g = find_generator()
        assert g == 37, (
            f"find_generator() returned {g}, expected 37 — this contradicts "
            "the Task-2-computed generator; STOP, do not silently emit a "
            "fixture with a mismatched g."
        )

        lines: list[str] = []
        lines.append(f"# sympsica reference fixtures (generated) - format v{FIXTURE_FORMAT_VERSION}")
        lines.append(f"# generator: python3 ref/reference.py emit --seed {seed} --out {out_path}")
        lines.append(f"format {FIXTURE_FORMAT_VERSION}")
        lines.append(f"seed {seed}")
        lines.append(f"p {P}")

        # (c) generator cross-check (ENC-5 support data).
        lines.append(f"generator_g {g}")
        lines.append("generator_g_crosscheck_pass 1")

        # (a) FLD-4: boundary reduction goldens. Inputs verbatim from
        # task-3-brief.md's FLD-4 row: {p-1, p, p+1, 2^61, 2^64-1}. Note
        # p+1 == 2^61 numerically (both emit the same input/expected pair,
        # intentionally — the test-vector table lists both).
        fld4_inputs = [P - 1, P, P + 1, 1 << 61, MASK64]
        fld4_rows = [(x, fp_from_u64(x)) for x in fld4_inputs]
        lines.append(f"fld4_count {len(fld4_rows)}")
        for x, y in fld4_rows:
            lines.append(f"fld4 {x} {y}")

        # (b) mul associativity/canonicity property fixture (10^6 samples).
        # The C++ side regenerates the same (a,b,c) triples from
        # mulprop_prng_seed via splitmix64 + fp_from_u64, computes
        # (a*b)*c and a*(b*c) via its own Fp::mul, asserts both canonical
        # and equal to each other (associativity), and folds the two
        # per-sample products (ab, abc) into a digest that must match
        # mulprop_digest below (cross-language equivalence of the
        # reduction algorithm — catches a single-fold-instead-of-double-
        # fold Fp::mul bug that a small hand-picked KAT could miss).
        mulprop_count = 1_000_000
        mulprop_seed = seed
        state = mulprop_seed
        digest = _FNV_OFFSET
        for _ in range(mulprop_count):
            state, r1 = splitmix64_next(state)
            state, r2 = splitmix64_next(state)
            state, r3 = splitmix64_next(state)
            a = fp_from_u64(r1)
            b = fp_from_u64(r2)
            c = fp_from_u64(r3)
            ab = (a * b) % P
            abc = (ab * c) % P
            bc = (b * c) % P
            a_bc = (a * bc) % P
            assert abc == a_bc, "associativity must hold under plain bigint mod arithmetic"
            digest = digest_update(digest, ab)
            digest = digest_update(digest, abc)
        lines.append(f"mulprop_prng_seed {mulprop_seed}")
        lines.append(f"mulprop_count {mulprop_count}")
        lines.append(f"mulprop_digest {digest}")

        # (d) TBL-1: two-id power-sum row over a set A = {a, b}, k = 1..7
        # (Params::K). A separate splitmix64 stream (seed XOR a fixed
        # constant) so it does not consume/collide with the mulprop
        # stream above. Nothing in Phase 1 consumes this row; emitted now
        # per plan text for the table layer (Phase 3+) to pick up later.
        tbl1_k = 7
        tstate = seed ^ 0xA5A5A5A5A5A5A5A5
        tstate, ra = splitmix64_next(tstate)
        tstate, rb = splitmix64_next(tstate)
        id_a = ra % (1 << 60)
        id_b = rb % (1 << 60)
        while id_b == id_a:
            tstate, rb = splitmix64_next(tstate)
            id_b = rb % (1 << 60)
        power_sums_row = Ref.power_sums([id_a, id_b], g, tbl1_k)
        lines.append(f"tbl1_k {tbl1_k}")
        lines.append("tbl1_count 1")
        lines.append("tbl1 " + " ".join(str(x) for x in ([id_a, id_b] + power_sums_row)))

        with open(out_path, "w") as f:
            f.write("\n".join(lines) + "\n")

    @staticmethod
    def emit_zt4_oracle(seeds: Sequence[int], out_path: str, domain_bits: int = 16,
                         num_digits: int = 4, main_payload: int = 1) -> None:
        """Emits ZT-4's oracle fixture (task-7-brief.md, W2.3 Test C /
        [CROSS-IMPL-SEMANTIC]): for each seed, a 61-bit mask `r` drawn from
        that seed's splitmix64 stream (rejecting p-1 = 2^61-1 exactly like
        ztgate_pipeline.cpp's step-1/step-2, so every emitted `r` is a
        canonical, ACCEPTED mask), its digit_split() (the four points each
        digit tree's oracle checks against — same carry-less split the
        pipeline uses for step 4), and the shared public payload (1,
        matching the Task-5 pipeline's hardcoded step-5 value). Consumers
        force these exact `r` values into the pipeline via
        PipelineOpts::forced_mask_halves so the interactively-generated DPF
        keys' points match these rows bit-for-bit.

        Also pins the two near-wrap payloads {p-1, p-2} the plan's ZT-4 row
        names explicitly; consumers reuse the SAME points above with these
        payloads via a direct RegularDpf::keyGen call (the pipeline's
        payload is fixed at 1, so near-wrap payloads bypass the pipeline).
        """
        domain = 1 << domain_bits
        lines: list[str] = []
        lines.append(f"# sympsica ZT-4 oracle fixture (generated) - format v{FIXTURE_FORMAT_VERSION}")
        lines.append(f"# generator: python3 ref/reference.py emit-zt4 "
                     f"--seeds {','.join(str(s) for s in seeds)} --out {out_path}")
        lines.append(f"format {FIXTURE_FORMAT_VERSION}")
        lines.append(f"p {P}")
        lines.append(f"domain_bits {domain_bits}")
        lines.append(f"domain {domain}")
        lines.append(f"num_digits {num_digits}")
        lines.append(f"main_payload {main_payload}")
        lines.append(f"seed_count {len(seeds)}")

        for seed in seeds:
            state = seed
            r = P  # the one rejected value; loop runs at least once
            while r == P:
                state, raw = splitmix64_next(state)
                r = raw & P
            digits = digit_split(r, num_digits=num_digits, digit_bits=(domain_bits))
            assert len(digits) == num_digits
            # Sanity self-check: dpf_oracle at each digit's point is the
            # single nonzero entry in that tree's table, valued main_payload
            # — the exact property the emitted row lets the C++ side assert
            # against its own full-domain expansion.
            for pt in digits:
                table = dpf_oracle(domain, pt, main_payload)
                assert table[pt] == main_payload
                assert sum(table) == main_payload
            lines.append("oracle_row " + " ".join(str(x) for x in ([seed, r] + digits)))

        lines.append("nearwrap_count 2")
        lines.append(f"nearwrap {P - 1}")  # p-1
        lines.append(f"nearwrap {P - 2}")  # p-2

        with open(out_path, "w") as f:
            f.write("\n".join(lines) + "\n")


def _cli(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(prog="reference.py")
    sub = parser.add_subparsers(dest="cmd", required=True)

    emit = sub.add_parser("emit", help="emit a Phase-1 fixture file for one seed")
    emit.add_argument("--seed", type=int, required=True)
    emit.add_argument("--out", type=str, required=True)

    emit_zt4 = sub.add_parser("emit-zt4", help="emit the ZT-4 oracle fixture (task-7-brief.md, W2.3)")
    emit_zt4.add_argument("--seeds", type=str, required=True,
                          help="comma-separated list of seeds, e.g. 0,1,2,...,9")
    emit_zt4.add_argument("--out", type=str, required=True)

    args = parser.parse_args(argv)
    if args.cmd == "emit":
        Ref.emit_fixtures(args.seed, args.out)
        print(f"wrote {args.out} (seed={args.seed})")
        return 0
    if args.cmd == "emit-zt4":
        seeds = [int(s) for s in args.seeds.split(",")]
        Ref.emit_zt4_oracle(seeds, args.out)
        print(f"wrote {args.out} (seeds={seeds})")
        return 0
    return 1


if __name__ == "__main__":
    raise SystemExit(_cli(sys.argv[1:]))

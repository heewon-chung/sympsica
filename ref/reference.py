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

        # (e) UPD-1 (task-11-brief.md, Phase 3b, ADDITIVE -- cut-1 sections
        # above are byte-identical to before this cut). A separate
        # splitmix64 stream (seed XOR yet another fixed constant, distinct
        # from both mulprop's `seed` and tbl1's 0xA5A5A5A5A5A5A5A5) draws 4
        # fresh initial ids and 2 more fresh insert ids; 2 of the 4 initial
        # ids are picked (deterministically, not drawn) as the delete set,
        # so every delete is a VALID delete -- UPD-1 is "basic apply: fresh
        # inserts + valid deletes", not a filter-corner-case KAT (those are
        # UPD-3/UPD-4, pure C++, no fixture needed). I'/D' are computed via
        # update_filter() (trivially equal to I/D here, since nothing in
        # this construction collides) and the resulting final id set/size
        # are emitted so the C++ side can assert st.my_ids/st.my_size/
        # st.table (via check_against)/st.J directly against these
        # Python-computed values (requirement 3: UPD-1 must compare
        # against a reference.py fixture, not a C++ reimplementation).
        ustate = seed ^ 0x5A5A5A5A5A5A5A5A
        upd1_used: set[int] = set()

        def _upd1_draw() -> int:
            nonlocal ustate
            while True:
                ustate, r = splitmix64_next(ustate)
                x = r % (1 << 60)
                if x not in upd1_used:
                    upd1_used.add(x)
                    return x

        upd1_init_ids = [_upd1_draw() for _ in range(4)]
        upd1_I = [_upd1_draw(), _upd1_draw()]
        upd1_D = upd1_init_ids[:2]  # valid deletes: already in the initial set
        upd1_Iprime, upd1_Dprime = Ref.update_filter(upd1_init_ids, upd1_I, upd1_D)
        upd1_final_ids = sorted((set(upd1_init_ids) - set(upd1_Dprime)) | set(upd1_Iprime))
        upd1_my_size = len(upd1_init_ids) + len(upd1_Iprime) - len(upd1_Dprime)

        lines.append(f"upd1_n_init {len(upd1_init_ids)}")
        lines.append("upd1_init_ids " + " ".join(str(x) for x in upd1_init_ids))
        lines.append(f"upd1_I_count {len(upd1_I)}")
        lines.append("upd1_I " + " ".join(str(x) for x in upd1_I))
        lines.append(f"upd1_D_count {len(upd1_D)}")
        lines.append("upd1_D " + " ".join(str(x) for x in upd1_D))
        lines.append(f"upd1_Iprime_count {len(upd1_Iprime)}")
        lines.append("upd1_Iprime " + " ".join(str(x) for x in upd1_Iprime))
        lines.append(f"upd1_Dprime_count {len(upd1_Dprime)}")
        lines.append("upd1_Dprime " + " ".join(str(x) for x in upd1_Dprime))
        lines.append(f"upd1_expected_final_count {len(upd1_final_ids)}")
        lines.append("upd1_expected_final_ids " + " ".join(str(x) for x in upd1_final_ids))
        lines.append(f"upd1_expected_my_size {upd1_my_size}")

        with open(out_path, "w") as f:
            f.write("\n".join(lines) + "\n")

    # -------------------------------------------------------------------
    # Cut 2 (task-11-brief.md, W3.6/W3.7): minors D_1..D_4, t_of, count,
    # the Update-filter mirror, and the epoch-schedule simulator. Pure
    # Python bigints throughout -- numpy is NOT available on this host and
    # is NOT to be added (controller ruling: the plan's "(numpy...)"
    # parenthetical is an optional vectorization hint; the binding
    # property is exact mod-p arithmetic, which plain bigints already
    # give exactly).
    # -------------------------------------------------------------------

    @staticmethod
    def minors(d: Sequence[int], p: int = P) -> tuple[int, int, int, int]:
        """D_1..D_4 -- the leading principal minors of the 4x4 Hankel
        matrix H[i][j] = d_{i+j-1} (1-indexed i, j = 1..4) built from the
        depth-7 power-sum/syndrome vector `d` = (d_1..d_7). Straight
        Python reimplementation of the hand-optimized cofactor circuit
        Phase 4's gates/minors.hpp (MinorCircuit::eval) will use --
        m[0..19] (layer 1, 20 products) and the T/B/A3/B3/C3 combos
        (layer 2) are the SAME index scheme as .handoff/sympsica-plan.md's
        W4.2 verbatim text (task-11-brief.md R-MIN, binding):
          D1 = d1
          D2 = d1*d3 - d2^2
          D3 = d1*(d3*d5-d4^2) - d2*(d2*d5-d3*d4) + d3*(d2*d4-d3^2)
          D4 = T12*B34 - T13*B24 + T14*B23 + T23*B14 - T24*B13 + T34*B12

        Requires the FULL depth-7 vector (D_4's Hankel matrix bottom row
        is [d4,d5,d6,d7]) -- d_6/d_7 must be genuine continuations of the
        same power-sum/syndrome construction as d_1..d_5, never zero
        padding (zero-padding a partial vector does NOT generally
        preserve the D_3=0 => D_4=0 rank identity; verified by hand
        against the MIN-2 worked row while building this function -- see
        task-11-report.md).
        """
        assert len(d) == 7, "Ref.minors: d must be the full depth-7 vector (d_1..d_7)"
        d1, d2, d3, d4, d5, d6, d7 = (x % p for x in d)

        m = [
            (d1 * d3) % p, (d2 * d2) % p, (d1 * d4) % p, (d2 * d3) % p,
            (d1 * d5) % p, (d2 * d4) % p, (d3 * d3) % p, (d2 * d5) % p,
            (d3 * d4) % p, (d3 * d5) % p, (d4 * d4) % p, (d3 * d6) % p,
            (d4 * d5) % p, (d3 * d7) % p, (d4 * d6) % p, (d5 * d5) % p,
            (d4 * d7) % p, (d5 * d6) % p, (d5 * d7) % p, (d6 * d6) % p,
        ]  # m[0..19], W4.2 layer-1 schedule verbatim

        T12 = (m[0] - m[1]) % p
        T13 = (m[2] - m[3]) % p
        T14 = (m[4] - m[5]) % p
        T23 = (m[5] - m[6]) % p
        T24 = (m[7] - m[8]) % p
        T34 = (m[9] - m[10]) % p

        B12 = (m[9] - m[10]) % p
        B13 = (m[11] - m[12]) % p
        B14 = (m[13] - m[14]) % p
        B23 = (m[14] - m[15]) % p
        B24 = (m[16] - m[17]) % p
        B34 = (m[18] - m[19]) % p

        A3, B3, C3 = T34, T24, T23  # W4.2: A3=m9-m10, B3=m7-m8, C3=m5-m6

        D1 = d1
        D2 = T12
        D3 = (d1 * A3 - d2 * B3 + d3 * C3) % p
        D4 = (T12 * B34 - T13 * B24 + T14 * B23 + T23 * B14 - T24 * B13 + T34 * B12) % p

        return D1, D2, D3, D4

    @staticmethod
    def t_of(d: Sequence[int], p: int = P, t_max: int = 4) -> int:
        """Rank-recovery rule (Remark 1, .handoff/sympsica-test-vectors.md
        TV-F7): t = max{tau in 1..t_max : D_tau != 0}, else 0 -- NEVER the
        first-zero index (MIN-3's crafted interior-accidental-zero case is
        exactly why a first-zero rule is wrong: an interior D_tau can
        vanish even when the true t is larger).
        """
        D = Ref.minors(d, p)
        t = 0
        for tau in range(1, t_max + 1):
            if D[tau - 1] % p != 0:
                t = tau
        return t

    @staticmethod
    def count(A: Iterable[int], B: Iterable[int], g: int, p: int = P) -> tuple[int, int]:
        """count(A, B) -> (t, |A intersect B|) (design doc's reference.py
        contract). DEVIATION (documented, R-SIM/R-MIN scope note): the
        real protocol partitions ids into buckets (BucketOracle, a BLAKE3
        ROM instantiation -- no Python port exists in this repo) and runs
        one MinorCircuit per bucket; this reference-level count() treats
        the WHOLE input as a single conceptual bucket, which validates the
        core algebraic identity (symmetric-difference-size recovery via
        minors) independent of bucketing -- bucketing is a pure
        efficiency/parallelism concern, not part of the identity itself.
        Not consumed by the Phase-3 C++ suite (R-SIM: count()/t_of() full
        consumption is Phase 4/6 work).
        """
        A = list(A)
        B = list(B)
        d = Ref.syndromes(A, B, g, 7, p)
        t = Ref.t_of(d, p)
        nA, nB = len(A), len(B)
        assert (nA + nB - t) % 2 == 0, (
            "Ref.count: |A|+|B|-t must be even for the affine "
            "2^-1(nA+nB-t) intersection-size recombination to be exact"
        )
        intersection = (nA + nB - t) // 2
        return t, intersection

    @staticmethod
    def update_filter(my_ids: Iterable[int], I: Iterable[int], D: Iterable[int]
                       ) -> tuple[list[int], list[int]]:
        """Mirrors Update::apply's three-stage step-1 filter EXACTLY
        (src/protocols/update.cpp, R-DUP controller ruling, binding):
        (1) dedup the raw I/D lists (first occurrence wins), (2) same-
        epoch pair drop (checked against the DEDUPED raw lists, not the
        post-membership-filtered I'/D', which are disjoint by
        construction), (3) membership filter (I' excludes ids already in
        my_ids; D' keeps only ids currently in my_ids). Returns (I', D').
        This is now a SPECIFIED algorithm (the ruling, and update.hpp's
        own doc comment) rather than something reverse-engineered from the
        C++ implementation, so an independent Python reimplementation is a
        legitimate cross-language check of that spec (UPD-1's fixture rows
        below are built from this function).
        """
        my_set = set(my_ids)

        def dedup(seq: Iterable[int]) -> list[int]:
            seen: set[int] = set()
            out: list[int] = []
            for x in seq:
                if x not in seen:
                    seen.add(x)
                    out.append(x)
            return out

        i_dedup = dedup(I)
        d_dedup = dedup(D)
        i_set = set(i_dedup)
        d_set = set(d_dedup)
        i_prime = [x for x in i_dedup if x not in my_set and x not in d_set]
        d_prime = [x for x in d_dedup if x in my_set and x not in i_set]
        return i_prime, d_prime

    @staticmethod
    def simulate(schedule: dict) -> list[int]:
        """Schedule simulator (task-11-brief.md W3.6/R-SIM). `schedule` is
        an R-SIM-format dict: {"n_init": int, "init_ids": [ids],
        "epochs": [{"I": [ids], "D": [ids], "query": bool}, ...]}. Replays
        every epoch through update_filter() against a SINGLE evolving id
        set (mirrors Update::apply's local, zero-communication semantics
        -- no second party/channel is modeled here); returns the list of
        my_size values observed at every epoch with "query": true, in
        epoch order.

        `init_ids` (the CONCRETE starting id set) is not part of R-SIM's
        literal one-line schema (which only names `n_init` as a bare
        count) but is required here so simulate() is a pure function of
        `schedule` alone, with no hidden PRNG-replay dependency -- see
        emit_schedule_fixture's docstring for why the emitted fixture
        stores concrete ids rather than asking a consumer to regenerate a
        PRNG stream (same convention the tbl1 fixture rows already use).
        """
        my_ids = set(schedule["init_ids"])
        assert len(my_ids) == schedule["n_init"], "simulate: init_ids/n_init mismatch"
        counts: list[int] = []
        for epoch in schedule["epochs"]:
            i_prime, d_prime = Ref.update_filter(my_ids, epoch["I"], epoch["D"])
            my_ids -= set(d_prime)
            my_ids |= set(i_prime)
            if epoch["query"]:
                counts.append(len(my_ids))
        return counts

    # Distinct from tbl1's 0xA5A5A5A5A5A5A5A5 XOR constant so the two
    # streams never collide even for the same seed.
    _SCHEDULE_SEED_XOR = 0xC0FFEE1234567890

    @staticmethod
    def make_schedule(seed: int, n_init: int = 3, n_epochs: int = 4) -> dict:
        """Deterministically builds an R-SIM-format schedule dict from a
        seeded splitmix64 stream: n_init fresh ids, then n_epochs epochs.
        Even epochs (0, 2, ...) are pure inserts (2 fresh ids each), not a
        query. Odd epochs (1, 3, ...) insert 1 fresh id AND delete the
        least-recently-inserted id still held (FIFO) -- so every delete
        this generator produces is a REAL, valid delete; UPD-3/UPD-4
        already cover the skip/no-op filter paths directly on hand-picked
        data, so this generator's job is pure multi-epoch state
        EVOLUTION (my_size tracking), not filter-corner-case coverage.
        query=True on every odd epoch.
        """
        state = seed ^ Ref._SCHEDULE_SEED_XOR
        used: set[int] = set()

        def draw_fresh() -> int:
            nonlocal state
            while True:
                state, r = splitmix64_next(state)
                x = r % (1 << 60)
                if x not in used:
                    used.add(x)
                    return x

        init_ids = [draw_fresh() for _ in range(n_init)]
        held: list[int] = list(init_ids)  # FIFO order of currently-held ids
        epochs = []
        for e in range(n_epochs):
            if e % 2 == 0:
                ins = [draw_fresh(), draw_fresh()]
                dele: list[int] = []
                query = False
            else:
                dele = [held.pop(0)] if held else []
                ins = [draw_fresh()]
                query = True
            held.extend(ins)
            epochs.append({"I": ins, "D": dele, "query": query})

        return {"n_init": n_init, "init_ids": init_ids, "epochs": epochs}

    @staticmethod
    def emit_schedule_fixture(seed: int, out_path: str) -> None:
        """Emits one epoch-schedule fixture (R-SIM): a schedule built by
        make_schedule() plus its expected per-query my_size counts
        (computed by simulate(), the one true golden replay). On-disk
        format mirrors the project's existing line-based `.fixture`
        convention (test/fixtures/README.md's "Why not JSON" rationale
        applies here too: avoids a JSON dependency on the C++ side even
        though R-SIM names the LOGICAL schema "schedule.json") --
        "schedule.json" stays the schema's name/label only, exactly like
        cut 1's "out.json" CLI-flag precedent.

        FORWARD-LOOKING NOTE (R-SIM, mirrors how TBL-1 was emitted
        unconsumed in Phase 1): the Phase-3 C++ suite consumes ONLY the
        `epoch`/`query` rows below as an update-path multi-epoch state
        check (replay every epoch through Update::apply, assert my_size at
        each query epoch) -- it does NOT yet exercise count()/t_of() (that
        is Phase 4/6, once gates/minors.hpp + BucketOracle-aware bucketing
        exist).
        """
        schedule = Ref.make_schedule(seed)
        expected_counts = Ref.simulate(schedule)

        lines: list[str] = []
        lines.append(f"# sympsica schedule fixture (generated) - format v{FIXTURE_FORMAT_VERSION}")
        lines.append(f"# generator: python3 ref/reference.py emit-schedule --seed {seed} --out {out_path}")
        lines.append(f"format {FIXTURE_FORMAT_VERSION}")
        lines.append(f"seed {seed}")
        lines.append(f"p {P}")
        lines.append(f"n_init {schedule['n_init']}")
        lines.append("init_ids " + " ".join(str(x) for x in schedule["init_ids"]))
        lines.append(f"epoch_count {len(schedule['epochs'])}")

        query_rows: list[tuple[int, int, int]] = []
        qi = 0
        for idx, ep in enumerate(schedule["epochs"]):
            q_flag = 1 if ep["query"] else 0
            row = [str(idx), str(q_flag), str(len(ep["I"]))] + [str(x) for x in ep["I"]]
            row += [str(len(ep["D"]))] + [str(x) for x in ep["D"]]
            lines.append("epoch " + " ".join(row))
            if ep["query"]:
                query_rows.append((qi, idx, expected_counts[qi]))
                qi += 1
        assert qi == len(expected_counts)

        lines.append(f"query_count {len(query_rows)}")
        for qidx, eidx, cnt in query_rows:
            lines.append(f"query {qidx} {eidx} {cnt}")

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


def _selftest_minors() -> None:
    """MIN-2 worked row (task-11-brief.md R-MIN; .handoff/sympsica-test-
    vectors.md's minors/rank section, verbatim): over the toy field F_101,
    signed difference {2 (+), 3 (-)} (t=2), d_k = 2^k - 3^k for k = 1..7
    (NOT the abbreviated 5-term (100, 96, 82, 36, 92) printed in the doc/
    ruling with zero padding for d_6/d_7 -- D_4 needs the full depth-7
    vector, and d_6/d_7 must continue the SAME 2^k-3^k construction or the
    D_3=0 => D_4=0 identity does not hold; verified by hand + brute force
    while building Ref.minors(), see task-11-report.md's derivation).
    Runs unconditionally on every import/execution of this module (a
    module self-test, per R-MIN's "module self-test or __main__ check").
    """
    toy_p = 101
    d = [(pow(2, k, toy_p) - pow(3, k, toy_p)) % toy_p for k in range(1, 8)]
    assert d[:5] == [100, 96, 82, 36, 92], f"MIN-2 setup drifted: d[:5]={d[:5]}"
    D1, D2, D3, D4 = Ref.minors(d, toy_p)
    assert (D1, D2, D3, D4) == (100, 95, 0, 0), (
        f"MIN-2 self-check FAILED: got D=({D1},{D2},{D3},{D4}), expected (100,95,0,0)"
    )
    assert Ref.t_of(d, toy_p) == 2, f"MIN-2: t_of() must recover t=2, got {Ref.t_of(d, toy_p)}"

    # MIN-1 companion (t=1 rank-1 identity, same doc section): a single
    # +x term (x != 0) must give D_1 = x, D_2 = 0.
    for x in (1, 5, 100):
        d1 = [pow(x, k, toy_p) for k in range(1, 8)]
        D1x, D2x, _, _ = Ref.minors(d1, toy_p)
        assert D1x == x % toy_p and D2x == 0, f"MIN-1 self-check FAILED for x={x}"


_selftest_minors()  # module self-test (R-MIN): always runs, aborts via AssertionError on failure


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

    emit_sched = sub.add_parser("emit-schedule",
                                 help="emit a Phase-3 epoch-schedule fixture (task-11-brief.md R-SIM)")
    emit_sched.add_argument("--seed", type=int, required=True)
    emit_sched.add_argument("--out", type=str, required=True)

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
    if args.cmd == "emit-schedule":
        Ref.emit_schedule_fixture(args.seed, args.out)
        print(f"wrote {args.out} (seed={args.seed})")
        return 0
    return 1


if __name__ == "__main__":
    raise SystemExit(_cli(sys.argv[1:]))

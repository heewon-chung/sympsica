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
import itertools
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


# ---------------------------------------------------------------------------
# task-13-brief.md controller ruling (binding): reference.py's independent
# check for MIN-4 uses a pure-Python Bareiss (fraction-free) determinant mod
# p over the LITERAL Hankel matrices -- NOT the m-index scheme Ref.minors()
# implements -- so `Ref.minors_det()` below is a genuinely independent
# cross-check of the schedule transcription, not a re-derivation of the same
# formula. See _det_bareiss's docstring for why it can fall back to exact
# Leibniz expansion on the (rare, MIN-3-specific) interior-zero-pivot case.
# ---------------------------------------------------------------------------


def _perm_sign(perm: tuple[int, ...]) -> int:
    """Sign of a permutation via inversion-count parity (n <= 4 here, so the
    naive O(n^2) count is trivial)."""
    n = len(perm)
    inversions = sum(1 for i in range(n) for j in range(i + 1, n) if perm[i] > perm[j])
    return -1 if inversions % 2 else 1


def _det_expansion(mat: list[list[int]]) -> int:
    """Exact determinant via Leibniz permutation expansion: a full sum over
    all n! permutations, pure multiply/add/subtract (fraction-free in the
    strongest sense -- no division anywhere), unconditionally correct
    including for singular/rank-deficient matrices. O(n!) is trivial for
    n <= 4. Used as the fallback when _det_bareiss cannot continue past a
    zero intermediate pivot (see its docstring) -- structurally unrelated to
    the m-index cofactor SCHEDULE (a full permutation sum rather than a
    hand-optimized 2x2-cofactor recursion), so it remains an independent
    cross-check even on the rare path where it is the one actually
    exercised.
    """
    n = len(mat)
    total = 0
    for perm in itertools.permutations(range(n)):
        term = _perm_sign(perm)
        for i in range(n):
            term *= mat[i][perm[i]]
        total += term
    return total


def _det_bareiss(mat: list[list[int]]) -> int | None:
    """Fraction-free Bareiss elimination over the INTEGERS (the matrix's
    canonical [0,p) representatives, treated as plain integers -- Bareiss's
    theorem guarantees exact division at every step whenever every leading
    principal minor encountered along the way is nonzero; reducing the
    final integer determinant mod p afterwards gives the same value as
    working mod p throughout, since the determinant is a polynomial in the
    entries and reduction mod p commutes with it).

    Returns None if elimination would need to divide by a ZERO leading
    principal minor of `mat` -- an interior accidental zero, exactly MIN-3's
    deliberately-constructed scenario. The classical UNPIVOTED Bareiss
    algorithm cannot continue past this point without a "look-ahead"
    extension (a documented limitation of the 1968 algorithm: row/column
    pivoting to dodge a zero pivot would compute a DIFFERENT matrix's
    determinant, not this matrix's leading principal minor, since leading
    principal minors are defined w.r.t. a FIXED row/column order). Callers
    fall back to _det_expansion, which is unconditionally correct, in that
    case -- see Ref.minors_det().
    """
    n = len(mat)
    a = [row[:] for row in mat]
    prev = 1
    for k in range(n - 1):
        if prev == 0:
            return None
        for i in range(k + 1, n):
            for j in range(k + 1, n):
                num = a[k][k] * a[i][j] - a[i][k] * a[k][j]
                q, r = divmod(num, prev)
                assert r == 0, "Bareiss: exact-division invariant violated"
                a[i][j] = q
        prev = a[k][k]
    return a[n - 1][n - 1]


def _hankel4(d: Sequence[int], p: int = P) -> list[list[int]]:
    """The 4x4 Hankel matrix H[i][j] = d_{i+j-1} (1-indexed), 0-indexed as
    H0[i][j] = dd[i+j] where dd[0] = d_1 -- same construction Ref.minors()'s
    docstring documents (D_4's bottom row is [d4,d5,d6,d7])."""
    dd = [x % p for x in d]
    return [[dd[i + j] for j in range(4)] for i in range(4)]


# ---------------------------------------------------------------------------
# task-18-brief.md W5.6 (SCOPE DECISION, no implementer discretion): the
# depth-T' overflow detector generalizes _hankel4/Ref.minors_det from a
# fixed depth 4 to an arbitrary depth n. _det_bareiss/_det_expansion are
# ALREADY n-agnostic (plain n x n elimination / permutation expansion, see
# their own docstrings above); only the Hankel-matrix construction and the
# max-nonzero-leading-minor rank rule needed generalizing.
# ---------------------------------------------------------------------------


def _hankel_n(d: Sequence[int], n: int, p: int = P) -> list[list[int]]:
    """Generalization of _hankel4 to an arbitrary depth n: the n x n Hankel
    matrix H[i][j] = d_{i+j-1} (1-indexed), 0-indexed as H0[i][j] = dd[i+j]
    where dd[0] = d_1. Requires len(d) >= 2*n-1 (the bottom-right entry
    needs d_{2n-1})."""
    assert len(d) >= 2 * n - 1, f"_hankel_n: need >= {2 * n - 1} syndrome terms for depth {n}, got {len(d)}"
    dd = [x % p for x in d]
    return [[dd[i + j] for j in range(n)] for i in range(n)]


def _minors_det_n(d: Sequence[int], n: int, p: int = P) -> tuple[int, ...]:
    """Generalization of Ref.minors_det to an arbitrary depth n: D_1..D_n,
    the leading principal minors of the n x n Hankel matrix built from `d`,
    via the SAME fraction-free Bareiss elimination (falling back to exact
    Leibniz expansion on an interior zero pivot) Ref.minors_det already uses
    for n=4."""
    H = _hankel_n(d, n, p)
    Ds: list[int] = []
    for k in range(1, n + 1):
        sub = [row[:k] for row in H[:k]]
        det = _det_bareiss(sub)
        if det is None:
            det = _det_expansion(sub)
        Ds.append(det % p)
    return tuple(Ds)


def _t_of_n(d: Sequence[int], n: int, p: int = P) -> int:
    """Generalization of Ref.t_of to an arbitrary depth n: the largest tau
    in 1..n with D_tau != 0, else 0 (Remark 1's max-nonzero-index rule,
    generalized). STRUCTURALLY bounded by n: an n x n Hankel test can never
    report a rank greater than n (there are only n leading principal
    minors to examine) -- this is exactly what makes FC3 (t_prime=4)
    provably incapable of flagging a true-rank-5 bucket, rather than
    "happening" not to.
    """
    D = _minors_det_n(d, n, p)
    t = 0
    for tau in range(1, n + 1):
        if D[tau - 1] % p != 0:
            t = tau
    return t


_GENERATOR_CACHE: dict[int, int] = {}


def _generator(p: int = P) -> int:
    """Lazily-cached find_generator(p) -- detect_overflow (below) needs a
    generator to compute sigma()-power sums but is not itself given one
    (task-18-brief.md's literal signature is `detect_overflow(A, B, oracle,
    Tprime=7)`, no `g` parameter); reusing the single canonical generator
    (37, for p=P) avoids re-deriving it on every call."""
    if p not in _GENERATOR_CACHE:
        _GENERATOR_CACHE[p] = find_generator(p)
    return _GENERATOR_CACHE[p]


# T_CAP mirrors Params::T (params.hpp) -- the real protocol's stored-table
# rank cap; detect_overflow flags a bucket whose DEEP (depth-2T'-1) rank
# exceeds this, i.e. a true multiplicity the T=4 circuit cannot represent.
T_CAP = 4


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
    def minors_det(d: Sequence[int], p: int = P) -> tuple[int, int, int, int]:
        """Independent cross-check of Ref.minors(): D_1..D_4 computed as the
        LITERAL leading principal minors of the 4x4 Hankel matrix via
        fraction-free Bareiss elimination (falling back to exact Leibniz
        expansion only when Bareiss hits a zero interior pivot -- see
        _det_bareiss's docstring) -- NOT the m-index cofactor schedule.
        task-13-brief.md controller ruling: this is what makes the
        `minors() == minors_det()` self-checks genuinely independent of the
        schedule transcription rather than just re-deriving the same
        formula a different way.
        """
        assert len(d) == 7, "Ref.minors_det: d must be the full depth-7 vector (d_1..d_7)"
        H = _hankel4(d, p)
        Ds: list[int] = []
        for k in range(1, 5):
            sub = [row[:k] for row in H[:k]]
            det = _det_bareiss(sub)
            if det is None:
                det = _det_expansion(sub)
            Ds.append(det % p)
        return tuple(Ds)  # type: ignore[return-value]

    @staticmethod
    def make_signed_set(state: int, t: int, p: int = P) -> tuple[list[int], list[int], int]:
        """Draws t DISTINCT NONZERO field elements and t signs (alternating
        +1/-1, starting with +1 -- matches the doc's {y(+), z(-)}-style
        worked examples) from a splitmix64 stream seeded by `state`.
        Returns (xs, signs, new_state) so callers can keep drawing from the
        same stream across several sets.
        """
        used: set[int] = set()
        xs: list[int] = []
        for _ in range(t):
            while True:
                state, r = splitmix64_next(state)
                x = r % p
                if x != 0 and x not in used:
                    used.add(x)
                    xs.append(x)
                    break
        signs = [1 if i % 2 == 0 else -1 for i in range(t)]
        return xs, signs, state

    @staticmethod
    def signed_syndromes(xs: Sequence[int], signs: Sequence[int], p: int = P,
                          depth: int = 7) -> list[int]:
        """d_k = sum s_i * x_i^k mod p, k = 1..depth -- MIN's notation
        directly (.handoff/sympsica-test-vectors.md: "elements of F_101
        written as integers; signed set difference {x_i, s_i} ... d_k = sum
        s_i x_i^k"), NOT routed through sigma()/the generator -- x_i ARE
        already field elements here, matching Ref.minors()'s own input
        contract. Empty xs/signs (t=0) correctly yields an all-zero vector.
        """
        assert len(xs) == len(signs)
        return [sum(s * pow(x, k, p) for x, s in zip(xs, signs)) % p for k in range(1, depth + 1)]

    @staticmethod
    def find_min3_example(seed: int, t: int, p: int = P) -> dict:
        """Deliberately constructs a signed t-element set whose D_1 (= d_1)
        is EXACTLY zero -- an interior accidental zero for any t >= 2 (since
        tau=1 < t), while D_t stays nonzero, i.e. the true rank t_of()
        recovers is still t despite the FIRST minor vanishing
        (task-13-brief.md MIN-3; TV-F7's binding max-rule pin: a
        first-zero-index rule would wrongly report t=0 here).

        Construction (deterministic, not blind search): draw the first t-1
        (element, sign) pairs freely from a seeded splitmix64 stream; SOLVE
        the last element so the signed linear sum (= d_1) is exactly 0
        (a sign is its own inverse in {+1,-1}, so
        x_last = -sign_last * partial_sum mod p); retry (perturbing the
        stream) only on the negligible-probability degenerate draws
        (x_last colliding with an earlier element, or D_t itself vanishing
        too). "found by reference.py search per Remark 1" (the brief's
        wording) is satisfied by this retry loop, even though the core
        d_1=0 property is enforced by direct construction rather than
        trial-and-error over signs.

        SIGN PATTERN, t=2 special case: for t >= 3 the first t-1 signs
        alternate (+1,-1,+1,...) and the last sign continues the
        alternation. For t=2 specifically this is IMPOSSIBLE without
        colliding the two elements: with signs {+1(x0), -1(x1)} (the
        design doc's own "{y(+), z(-)}" shape), forcing d1 = x0 - x1 = 0
        algebraically forces x1 == x0, which is not a valid 2-element SET
        (this is a genuine mathematical fact, not an implementation gap --
        see task-13-report.md for the derivation). t=2 therefore uses
        SAME-sign elements {+1(x0), +1(x1)}: d1 = x0 + x1 = 0 forces
        x1 = -x0 mod p, which is always DISTINCT from x0 (p is odd, so
        x0 == -x0 mod p only at x0 == 0, already excluded) -- a legitimate
        signed set (both elements on the same symmetric-difference side,
        |A\\B|=2, |B\\A|=0), still exercising the exact same D_1=0
        interior-zero property MIN-3/TV-F7 needs.
        """
        assert 2 <= t <= 4, "find_min3_example: t must be in {2,3,4} (MIN-3's stated range)"
        state = seed ^ 0x4D494E33 ^ (t << 32)  # 'MIN3'-ish salt, distinct per t
        for _attempt in range(1000):
            if t == 2:
                prefix_signs = [1]
                sign_last = 1
            else:
                prefix_signs = [1 if i % 2 == 0 else -1 for i in range(t - 1)]
                sign_last = 1 if (t - 1) % 2 == 0 else -1

            xs: list[int] = []
            used: set[int] = set()
            for _ in range(t - 1):
                while True:
                    state, r = splitmix64_next(state)
                    x = r % p
                    if x != 0 and x not in used:
                        used.add(x)
                        xs.append(x)
                        break

            partial = sum(s * x for s, x in zip(prefix_signs, xs)) % p
            x_last = (-sign_last * partial) % p
            if x_last == 0 or x_last in xs:
                state ^= 0x9E3779B97F4A7C15
                continue
            xs_full = xs + [x_last]
            signs_full = prefix_signs + [sign_last]
            d = Ref.signed_syndromes(xs_full, signs_full, p)
            D = Ref.minors(d, p)
            assert D[0] == 0, "find_min3_example: construction failed to zero D_1"
            if D[t - 1] != 0 and Ref.t_of(d, p) == t:
                return {"xs": xs_full, "signs": signs_full, "d": d, "D": D, "t": t}
            state ^= 0x9E3779B97F4A7C15
        raise RuntimeError(f"find_min3_example: failed to construct a MIN-3 example for t={t}")

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

    @staticmethod
    def emit_min_fixtures(seed: int, out_path: str) -> None:
        """Emits MIN-3 (interior-accidental-zero, t in {2,3,4}) and MIN-4
        (bulk random cross-check, t in {0..4}, 100 each) fixture rows for
        one seed (task-13-brief.md, additive; existing format/README
        discipline). Golden source for both: Ref.minors()/Ref.t_of() (the
        m-index schedule, independently brute-force-verified at Phase 3 --
        see _selftest_minors()), further cross-checked here against
        Ref.minors_det() (the Bareiss/literal-Hankel-determinant path, NOT
        the m-index scheme -- controller ruling) BEFORE anything is
        written, so a schedule-transcription bug can never silently reach a
        committed fixture.

        Row format (one 13-token row per case, `min3`/`min4` key): `<n>
        <d1..d7> <D1..D4> <expected_t>` -- x_i/s_i themselves are NOT
        emitted (the consuming C++ test only needs the resulting syndrome
        vector `d` and the golden D/t outputs; `n` documents how many
        signed elements produced this row, purely informational).
        """
        def row_tokens(d: Sequence[int], D: Sequence[int], t: int, n: int) -> str:
            toks = [str(n)] + [str(x) for x in d] + [str(x) for x in D] + [str(t)]
            return " ".join(toks)

        lines: list[str] = []
        lines.append(f"# sympsica MIN fixture (generated) - format v{FIXTURE_FORMAT_VERSION}")
        lines.append(f"# generator: python3 ref/reference.py emit-min --seed {seed} --out {out_path}")
        lines.append(f"format {FIXTURE_FORMAT_VERSION}")
        lines.append(f"seed {seed}")
        lines.append(f"p {P}")

        # --- MIN-3: interior accidental zero, t in {2,3,4} ----------------
        min3_lines: list[str] = []
        for t in (2, 3, 4):
            ex = Ref.find_min3_example(seed, t)
            d, D, tt = ex["d"], ex["D"], ex["t"]
            assert D[0] == 0, "MIN-3 fixture row: D_1 must be the interior zero"
            assert tt == t
            assert Ref.minors_det(d) == D, f"MIN-3 (t={t}): minors() vs minors_det() disagree"
            min3_lines.append(row_tokens(d, D, tt, len(ex["xs"])))
        lines.append(f"min3_count {len(min3_lines)}")
        for row in min3_lines:
            lines.append("min3 " + row)

        # --- MIN-4: bulk random cross-check, t in {0..4}, 100 each --------
        min4_lines: list[str] = []
        state = seed ^ 0x4D494E34
        for t in range(0, 5):
            for _trial in range(100):
                xs, signs, state = Ref.make_signed_set(state, t)
                d = Ref.signed_syndromes(xs, signs)
                D = Ref.minors(d)
                assert Ref.minors_det(d) == D, f"MIN-4 (t={t}): minors() vs minors_det() disagree"
                tt = Ref.t_of(d)
                min4_lines.append(row_tokens(d, D, tt, len(xs)))
        lines.append(f"min4_count {len(min4_lines)}")
        for row in min4_lines:
            lines.append("min4 " + row)

        with open(out_path, "w") as f:
            f.write("\n".join(lines) + "\n")

    @staticmethod
    def emit_sd_fixtures(seed: int, out_path: str) -> None:
        """Emits SD-1/SD-2 fixture rows (task-14-brief.md R-SYNDROMES,
        additive -- no existing section touched): one signed-set syndrome
        vector per t in {0..4}, via the SAME golden generator as MIN-4
        (Ref.make_signed_set / Ref.signed_syndromes / Ref.t_of -- "power_sums
        difference" semantics, per Ref.syndromes's own docstring: signed
        syndromes are algebraically the difference of two sides' power sums).

        SD-1 ("synthetic buckets t in {0..4}, incl. the padding row") and
        SD-2 ("t = T = 4 exactly, pivot at bound") both read from this single
        5-row family: the t=0 row is DOUBLY meaningful -- make_signed_set(
        state, 0) draws no elements, so signed_syndromes([], []) is already
        the all-zero syndrome vector, which is simultaneously "a genuine
        empty-difference bucket" and "the padding-row convention" (both
        parties literally holding zero shares of an all-zero d) -- the C++
        side (test/gates/kat_symdiff.cpp) exercises both by varying how that
        SAME all-zero vector is additively shared, not by asking Python for
        two different rows. SD-2 reads the t=4 row directly.
        """
        def row_tokens(d: Sequence[int], D: Sequence[int], t: int, n: int) -> str:
            toks = [str(n)] + [str(x) for x in d] + [str(x) for x in D] + [str(t)]
            return " ".join(toks)

        lines: list[str] = []
        lines.append(f"# sympsica SD fixture (generated) - format v{FIXTURE_FORMAT_VERSION}")
        lines.append(f"# generator: python3 ref/reference.py emit-sd --seed {seed} --out {out_path}")
        lines.append(f"format {FIXTURE_FORMAT_VERSION}")
        lines.append(f"seed {seed}")
        lines.append(f"p {P}")

        sd_lines: list[str] = []
        state = seed ^ 0x53443100  # 'SD1'-ish salt, distinct from MIN's 0x4D494E34
        for t in range(0, 5):
            xs, signs, state = Ref.make_signed_set(state, t)
            d = Ref.signed_syndromes(xs, signs)
            D = Ref.minors(d)
            assert Ref.minors_det(d) == D, f"emit_sd_fixtures (t={t}): minors() vs minors_det() disagree"
            tt = Ref.t_of(d)
            assert tt == t, f"emit_sd_fixtures: t_of mismatch for t={t}: got {tt}"
            sd_lines.append(row_tokens(d, D, tt, len(xs)))
        lines.append(f"sd_count {len(sd_lines)}")
        for row in sd_lines:
            lines.append("sd " + row)

        with open(out_path, "w") as f:
            f.write("\n".join(lines) + "\n")

    # -------------------------------------------------------------------
    # Cut 3 (task-18-brief.md W5.7/W5.6, ADDITIVE -- cut-1/cut-2 sections
    # above are byte-identical to before this cut). R-ORACLE-AGNOSTIC:
    # this module never ports BucketOracle (a BLAKE3 ROM instantiation);
    # simulate_days() only needs bucket-independent |A intersect B|
    # counting (no-overflow assumption), and detect_overflow() takes the
    # oracle as an injected callable instead.
    # -------------------------------------------------------------------

    @staticmethod
    def simulate_days(schedule_r: Sequence[dict], schedule_s: Sequence[dict]) -> list[int]:
        """Day-schedule simulator for the W5.7 PAIR format (task-18-brief.md
        R-ORACLE-AGNOSTIC), additive alongside Ref.simulate()'s OLD
        {"n_init","epochs"} format above (untouched, not called by this).

        `schedule_r`/`schedule_s` are two lists in the SAME per-party
        format apps/party_main.cpp's own `--schedule` files use: each day
        is `{"day": str, "insert": [ids], "delete": [ids], "query": bool,
        "maintenance": bool}`. A schedule PAIR is generated together
        (R-SCHED2: same day sequence, same query/maintenance markers), so
        this asserts that pairing holds (day string, query flag,
        maintenance flag agree at every index) -- the same invariant
        party_main's own ScheduleSync::sync() enforces at the C++ level,
        checked here so a malformed test fixture fails loudly in Python
        too, before ever reaching a two-process run.

        Each party's own id set evolves LOCALLY via update_filter() from
        its own insert/delete lists (mirrors Update::apply's zero-
        communication semantics -- no channel is modeled). On a day with
        "query": true, records len(ids_r intersect ids_s) -- oracle-
        INDEPENDENT (R-ORACLE-AGNOSTIC: |A intersect B| does not depend on
        how ids are bucketed, only WHICH ids are held, as long as no
        bucket's true multiplicity overflows T -- see detect_overflow for
        the deep-scan check of that assumption). Returns the list of
        expected counts, in day order, one per query day.
        """
        assert len(schedule_r) == len(schedule_s), "simulate_days: schedule pair length mismatch"
        ids_r: set[int] = set()
        ids_s: set[int] = set()
        counts: list[int] = []
        for day_r, day_s in zip(schedule_r, schedule_s):
            assert day_r["day"] == day_s["day"], (
                f"simulate_days: day mismatch {day_r['day']!r} vs {day_s['day']!r} (R-SCHED2 pair drift)"
            )
            assert day_r["query"] == day_s["query"], (
                f"simulate_days: query-flag mismatch on {day_r['day']!r} (R-SCHED2 pair drift)"
            )
            assert day_r["maintenance"] == day_s["maintenance"], (
                f"simulate_days: maintenance-flag mismatch on {day_r['day']!r} (R-SCHED2 pair drift)"
            )

            ir_prime, dr_prime = Ref.update_filter(ids_r, day_r["insert"], day_r["delete"])
            ids_r -= set(dr_prime)
            ids_r |= set(ir_prime)

            is_prime, ds_prime = Ref.update_filter(ids_s, day_s["insert"], day_s["delete"])
            ids_s -= set(ds_prime)
            ids_s |= set(is_prime)

            if day_r["query"]:
                counts.append(len(ids_r & ids_s))
        return counts

    @staticmethod
    def detect_overflow(A: Iterable[int], B: Iterable[int], oracle, t_prime: int = 7,
                         p: int = P) -> dict:
        """W5.6's plaintext-only deep-scan overflow detector (SCOPE
        DECISION, task-18-brief.md, no implementer discretion): the
        depth-T'=7 detector needs syndromes to depth 2*T'-1=13 > K=7, which
        the stored PowerSumTable rows cannot supply and an MPC deep-scan
        circuit (mu(7)) is out of PoC scope -- this function is the ONLY
        working implementation anywhere in this codebase; src/protocols/
        salt.hpp's C++ OverflowChecker is a stub that always dies (SC4/FC2).

        `oracle` is a CALLABLE id -> bucket (R-ORACLE-AGNOSTIC: this module
        never ports BucketOracle/BLAKE3, so the Phase-6 planted-collision
        demo passes a synthetic map instead of a real oracle). Buckets `A`
        and `B` separately via `oracle`, then for every bucket touched by
        EITHER side computes the signed syndrome vector d_1..d_{2*t_prime-1}
        (Ref.syndromes's own power_sums(A-side) - power_sums(B-side)
        convention) and its depth-t_prime generalized rank recovery
        (_t_of_n: literally Ref.t_of/Ref.minors_det generalized from the
        fixed depth 4 to an arbitrary depth n via the SAME Bareiss/Leibniz
        machinery -- just a bigger Hankel matrix).

        A bucket is flagged OVERFLOWING iff its recovered depth-t_prime
        rank EXCEEDS T_CAP (= Params::T = 4): the real protocol's own
        t_of() -- capped at depth 4 -- can never itself see past t=4, so
        only a deeper (>= 2*t_prime-1 >= 13-term) scan can distinguish
        "true multiplicity exactly 4" from "true multiplicity > 4, silently
        misrecovered as (at most) 4 by the capped circuit". FC3 (t_prime=4,
        i.e. no deep headroom beyond the real protocol's own depth) proves
        this: _t_of_n is STRUCTURALLY bounded by its own depth argument (an
        n x n Hankel test has only n leading principal minors), so
        detect_overflow(..., t_prime=4) can never report a rank above 4 and
        therefore can never flag anything -- not because of some
        incidental implementation detail, but because the depth itself is
        the only thing that makes detection possible.

        Returns {"overflowing_buckets": [bucket, ...] (sorted), "detail":
        {bucket: {"d": [...], "t_prime_rank": int}, ...}} -- `detail`
        carries EVERY bucket actually touched (not just flagged ones) so a
        caller can inspect borderline cases.
        """
        assert t_prime >= 1, "detect_overflow: t_prime must be >= 1"
        depth = 2 * t_prime - 1
        g = _generator(p)

        bucket_a: dict[int, list[int]] = {}
        for id_ in A:
            bucket_a.setdefault(oracle(id_), []).append(id_)
        bucket_b: dict[int, list[int]] = {}
        for id_ in B:
            bucket_b.setdefault(oracle(id_), []).append(id_)

        overflowing: list[int] = []
        detail: dict[int, dict] = {}
        for beta in sorted(set(bucket_a) | set(bucket_b)):
            ids_a = bucket_a.get(beta, [])
            ids_b = bucket_b.get(beta, [])
            d = Ref.syndromes(ids_a, ids_b, g, depth, p)
            rank = _t_of_n(d, t_prime, p)
            detail[beta] = {"d": d, "t_prime_rank": rank}
            if rank > T_CAP:
                overflowing.append(beta)

        return {"overflowing_buckets": overflowing, "detail": detail}

    # -------------------------------------------------------------------
    # Cut 4 (task-19-brief.md W5.8, ADDITIVE -- cut-1/2/3 sections above are
    # byte-identical to before this cut; verify with `git diff` against the
    # pre-cut-4 blob). SC1's 100-seed schedule sweep (SMALL scale, R-SCALE19)
    # and E2E-1..4's seed-fixed n~2^10 schedule pairs.
    # -------------------------------------------------------------------

    @staticmethod
    def make_paired_day_schedule(seed: int, n_days: int, universe: int,
                                  insert_per_day: int, delete_prob: float = 0.3
                                  ) -> tuple[list[dict], list[dict]]:
        """Builds a small, randomized W5.7-format day-schedule PAIR (SC1,
        R-SCALE19 SMALL scale: n in [32,128] ids/party, 4-8 days, >=2 query
        days). Query days are every other day starting at index 1 (>= 2 by
        construction whenever n_days >= 3; n_days in [4,8] per caller always
        satisfies this). Each party draws `insert_per_day` fresh ids per day
        uniformly from [1, universe] (independent per-party splitmix64
        streams, so R and S do NOT draw identical sequences) and, with
        probability `delete_prob`, deletes one id currently (approximately)
        held -- `held_r`/`held_s` are a LOCAL bookkeeping approximation only
        (not required to be exact: Ref.simulate_days/Update::apply's own
        update_filter is the actual golden authority for what a delete
        candidate resolves to; an approximate/invalid delete candidate is
        simply filtered to a no-op by update_filter on both sides, never a
        correctness hazard).
        """
        state = seed ^ 0x53434831303044  # 'SCH100D'-ish salt, distinct from every other stream
        held_r: set[int] = set()
        held_s: set[int] = set()
        query_days = set(range(1, n_days, 2))
        if len(query_days) < 2:
            query_days = set(range(max(0, n_days - 2), n_days))

        def draw(st: int) -> tuple[int, int]:
            return splitmix64_next(st)

        schedule_r: list[dict] = []
        schedule_s: list[dict] = []
        for day_idx in range(n_days):
            day_label = f"d{day_idx:03d}"
            is_query = day_idx in query_days

            ins_r: list[int] = []
            for _ in range(insert_per_day):
                state, r = draw(state)
                ins_r.append(1 + (r % universe))
            del_r: list[int] = []
            state, r = draw(state)
            if held_r and (r % 1000) < int(delete_prob * 1000):
                state, r2 = draw(state)
                del_r = [sorted(held_r)[r2 % len(held_r)]]
            held_r |= set(ins_r)
            held_r -= set(del_r)
            schedule_r.append({"day": day_label, "insert": ins_r, "delete": del_r,
                                "query": is_query, "maintenance": False})

            ins_s: list[int] = []
            for _ in range(insert_per_day):
                state, r = draw(state)
                ins_s.append(1 + (r % universe))
            del_s: list[int] = []
            state, r = draw(state)
            if held_s and (r % 1000) < int(delete_prob * 1000):
                state, r2 = draw(state)
                del_s = [sorted(held_s)[r2 % len(held_s)]]
            held_s |= set(ins_s)
            held_s -= set(del_s)
            schedule_s.append({"day": day_label, "insert": ins_s, "delete": del_s,
                                "query": is_query, "maintenance": False})

        return schedule_r, schedule_s

    @staticmethod
    def emit_sched100_fixture(seed_lo: int, seed_hi: int, out_path: str) -> None:
        """Emits SC1's bulk 100-seed schedule-pair fixture (task-19-brief.md):
        for each seed in [seed_lo, seed_hi], a schedule pair from
        make_paired_day_schedule() (params varied per seed for diversity)
        plus its golden Ref.simulate_days() expected per-query-day count
        sequence. Ids are stored CONCRETE (same convention as every other
        fixture in this module -- tbl1/upd1/schedule -- so the C++ side never
        needs to replicate this file's PRNG stream).
        """
        body: list[str] = []
        sched_count = 0
        for seed in range(seed_lo, seed_hi + 1):
            n_days = 4 + (seed % 5)          # 4..8 (R-SCALE19)
            universe = 120 + (seed % 60)     # 120..179
            insert_per_day = 6 + (seed % 5)  # 6..10
            schedule_r, schedule_s = Ref.make_paired_day_schedule(seed, n_days, universe, insert_per_day)
            expected = Ref.simulate_days(schedule_r, schedule_s)
            assert len(expected) >= 2, f"seed={seed}: need >=2 query days, got {len(expected)}"
            body.append(f"sched {seed} {n_days} {len(expected)}")
            for day_r, day_s in zip(schedule_r, schedule_s):
                row = [str(seed), day_r["day"], "1" if day_r["query"] else "0",
                       str(len(day_r["insert"]))] + [str(x) for x in day_r["insert"]]
                row += [str(len(day_r["delete"]))] + [str(x) for x in day_r["delete"]]
                row += [str(len(day_s["insert"]))] + [str(x) for x in day_s["insert"]]
                row += [str(len(day_s["delete"]))] + [str(x) for x in day_s["delete"]]
                body.append("day " + " ".join(row))
            body.append("expected " + str(seed) + " " + " ".join(str(x) for x in expected))
            sched_count += 1

        lines: list[str] = []
        lines.append(f"# sympsica SCHED100 fixture (generated) - format v{FIXTURE_FORMAT_VERSION}")
        lines.append("# generator: python3 ref/reference.py emit-sched100 "
                      f"--seed-lo {seed_lo} --seed-hi {seed_hi} --out {out_path}")
        lines.append(f"format {FIXTURE_FORMAT_VERSION}")
        lines.append(f"seed_lo {seed_lo}")
        lines.append(f"seed_hi {seed_hi}")
        lines.append(f"sched_count {sched_count}")
        lines.extend(body)
        with open(out_path, "w") as f:
            f.write("\n".join(lines) + "\n")

    @staticmethod
    def make_e2e_schedule_pair(seed: int) -> tuple[list[dict], list[dict]]:
        """Seed-fixed E2E-1..4 day-schedule pair (task-19-brief.md SC2,
        R-SCALE19): n ~ 2^10 ids/party, exactly 3 query days -- d1 (first
        query, plain FullPublic), d2 (maintenance=true, salt-refresh query,
        >= 1 salt-refresh day), d3 (delete-bearing normal query day, >= 1
        delete-bearing normal day) -- kept intentionally SHORT (R-SCALE19:
        "Expect minutes per schedule; keep each schedule SHORT"), since each
        full-path query at this scale costs tens of seconds of real MPC.
        `seed` only shifts the id-range offset (distinct, non-overlapping
        universes across seeds 0..3) so the four fixed schedules are
        structurally identical but operate on disjoint id spaces.
        """
        off = seed * 10_000
        r_base = list(range(1 + off, 1001 + off))     # 1000 ids
        r_extra = list(range(1001 + off, 1025 + off)) # 24 more -> ~1024
        s_base = list(range(600 + off, 1601 + off))   # 1001 ids, overlaps r_base in [600,1000]
        s_extra = list(range(1601 + off, 1625 + off)) # 24 more
        r_delete = [1 + off, 2 + off, 3 + off, 4 + off, 5 + off]
        s_delete = [600 + off, 601 + off, 602 + off, 603 + off, 604 + off]

        schedule_r = [
            {"day": "d0", "insert": r_base, "delete": [], "query": False, "maintenance": False},
            {"day": "d1", "insert": [], "delete": [], "query": True, "maintenance": False},
            {"day": "d2", "insert": r_extra, "delete": [], "query": True, "maintenance": True},
            {"day": "d3", "insert": [], "delete": r_delete, "query": True, "maintenance": False},
        ]
        schedule_s = [
            {"day": "d0", "insert": s_base, "delete": [], "query": False, "maintenance": False},
            {"day": "d1", "insert": [], "delete": [], "query": True, "maintenance": False},
            {"day": "d2", "insert": s_extra, "delete": [], "query": True, "maintenance": True},
            {"day": "d3", "insert": [], "delete": s_delete, "query": True, "maintenance": False},
        ]
        return schedule_r, schedule_s

    @staticmethod
    def emit_e2e_schedule_pair(seed: int, out_r: str, out_s: str) -> None:
        """Writes one E2E-1..4 schedule pair to `out_r`/`out_s` as real JSON
        (party_main.cpp's --schedule format directly, NOT this module's own
        line-based fixture convention -- E2E fixtures are consumed by the
        `party` binary itself, not by a C++ test reading via
        fixture_support.hpp). Asserts the pair is well-formed (R-SCHED2
        pairing, exactly 3 query days) via Ref.simulate_days() before
        writing anything.
        """
        schedule_r, schedule_s = Ref.make_e2e_schedule_pair(seed)
        expected = Ref.simulate_days(schedule_r, schedule_s)
        assert len(expected) == 3, f"seed={seed}: expected exactly 3 query days, got {len(expected)}"
        import json
        with open(out_r, "w") as f:
            json.dump(schedule_r, f)
        with open(out_s, "w") as f:
            json.dump(schedule_s, f)


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

    # task-13-brief.md controller ruling (binding): minors()/minors_det()
    # cross-check over a MIN-4-style sample (every t in {0..4}) PLUS the
    # MIN-3 interior-zero construction, run unconditionally at import time
    # (not just inside emit_min_fixtures) so this module can never be
    # imported with a broken minors_det() without failing loudly. Smaller
    # than the full 100-per-t fixture sample (20, not 100) to keep
    # import-time cost negligible; emit_min_fixtures repeats the full check
    # over its own larger sample before writing anything.
    state = 0xC0FFEE ^ 0x4D494E34
    for t in range(0, 5):
        for _trial in range(20):
            xs, signs, state = Ref.make_signed_set(state, t)
            d = Ref.signed_syndromes(xs, signs)
            got, want = Ref.minors_det(d), Ref.minors(d)
            assert got == want, (
                f"minors()/minors_det() self-check FAILED for t={t}: "
                f"minors={want} minors_det={got}"
            )
    for t in (2, 3, 4):
        ex = Ref.find_min3_example(0xC0FFEE, t)
        assert Ref.minors_det(ex["d"]) == ex["D"], f"MIN-3 minors_det self-check FAILED for t={t}"


def _selftest_overflow() -> None:
    """SC5/FC3 (task-18-brief.md W5.6): detect_overflow's planted
    5-collision bucket is flagged at Tprime=7 with the exact recovered rank
    5; the SAME bucket is NOT flagged at Tprime=4 (FC3: proves the depth,
    not a side effect, is what detects); a post-refresh dispersal (a
    disjoint synthetic oracle standing in for BucketOracle::refreshed --
    R-ORACLE-AGNOSTIC, no BLAKE3 port exists here) clears it entirely
    (salt-refresh transience, pre-building the Phase-6 demo); three random
    small no-overflow sets (seeds 0..2) are NOT flagged. Runs
    unconditionally on every import (same "module self-test" discipline
    _selftest_minors() already establishes, R-MIN's own precedent).
    """
    planted_ids = [10_001, 10_002, 10_003, 10_004, 10_005]  # true multiplicity 5, all on the A side

    def planted_oracle(id_: int) -> int:
        return 777 if id_ in planted_ids else id_  # everything else maps to itself: no incidental collisions

    res7 = Ref.detect_overflow(planted_ids, [], planted_oracle, t_prime=7)
    assert 777 in res7["overflowing_buckets"], "SC5: planted 5-collision bucket must be flagged at Tprime=7"
    assert res7["detail"][777]["t_prime_rank"] == 5, (
        f"SC5: planted bucket's recovered depth-7 rank must be exactly 5, "
        f"got {res7['detail'][777]['t_prime_rank']}"
    )

    # FC3: same planted bucket, Tprime=4 (no deep headroom) -> NOT flagged.
    res4 = Ref.detect_overflow(planted_ids, [], planted_oracle, t_prime=4)
    assert 777 not in res4["overflowing_buckets"], (
        "FC3: Tprime=4 must NOT flag the planted 5-collision bucket -- detection needs depth "
        "headroom beyond T=4, not some other side effect"
    )
    assert res4["detail"][777]["t_prime_rank"] <= 4, "FC3: a depth-4 Hankel test cannot report rank > 4"

    # Salt-refresh transience: a disjoint synthetic oracle (standing in for
    # BucketOracle::refreshed -- a fresh salt disperses ids to new buckets)
    # scatters the same 5 planted ids across 5 distinct buckets (identity
    # map: 5 distinct ids -> 5 distinct buckets); the detector must find NO
    # overflowing bucket at all post-refresh.
    def refreshed_oracle(id_: int) -> int:
        return id_

    res_refreshed = Ref.detect_overflow(planted_ids, [], refreshed_oracle, t_prime=7)
    assert res_refreshed["overflowing_buckets"] == [], (
        "SC5: post-refresh dispersal must clear the planted bucket entirely"
    )

    # Random no-overflow sets, seeds 0..2: small sets bucketed via a coarse
    # (but real, non-degenerate) modulus oracle; the precondition (no
    # bucket actually collects > T_CAP ids) is VERIFIED, not just assumed,
    # before asserting detect_overflow agrees.
    modulus = 997

    def modulus_oracle(id_: int) -> int:
        return id_ % modulus

    for seed in range(3):
        state = seed ^ 0x4F56464C  # 'OVFL'-ish salt, distinct from every other stream in this module
        ids_a: list[int] = []
        ids_b: list[int] = []
        for _ in range(12):
            state, r = splitmix64_next(state)
            ids_a.append(200_000 + (r % 100_000))
        for _ in range(12):
            state, r = splitmix64_next(state)
            ids_b.append(200_000 + (r % 100_000))

        bucket_counts: dict[int, int] = {}
        for id_ in ids_a + ids_b:
            bucket_counts[modulus_oracle(id_)] = bucket_counts.get(modulus_oracle(id_), 0) + 1
        assert max(bucket_counts.values()) <= T_CAP, (
            f"_selftest_overflow precondition failed for seed={seed}: a bucket collected more than "
            f"T_CAP={T_CAP} ids by chance -- adjust the modulus/sample size"
        )

        res = Ref.detect_overflow(ids_a, ids_b, modulus_oracle, t_prime=7)
        assert res["overflowing_buckets"] == [], (
            f"SC5: seed={seed} random no-overflow set must not be flagged, got "
            f"{res['overflowing_buckets']}"
        )


def _selftest_simulate_days() -> None:
    """SC6 support (task-18-brief.md R-ORACLE-AGNOSTIC): a tiny hand-built
    schedule PAIR (3 days, 1 query day, disjoint insert ids so the true
    intersection is known by construction) round-trips through
    simulate_days() to the expected count. Also checks the R-SCHED2 pair-
    drift assertion actually fires on a mismatched pair.
    """
    schedule_r = [
        {"day": "2023-01-01", "insert": [1, 2, 3], "delete": [], "query": False, "maintenance": False},
        {"day": "2023-01-02", "insert": [4], "delete": [], "query": True, "maintenance": False},
        {"day": "2023-01-03", "insert": [], "delete": [1], "query": True, "maintenance": False},
    ]
    schedule_s = [
        {"day": "2023-01-01", "insert": [2, 3, 5], "delete": [], "query": False, "maintenance": False},
        {"day": "2023-01-02", "insert": [], "delete": [], "query": True, "maintenance": False},
        {"day": "2023-01-03", "insert": [6], "delete": [], "query": True, "maintenance": False},
    ]
    # Day 1 (non-query): R={1,2,3}, S={2,3,5}.
    # Day 2 (query): R={1,2,3,4}, S={2,3,5} -> intersection {2,3} = 2.
    # Day 3 (query): R={2,3,4}, S={2,3,5,6} -> intersection {2,3} = 2.
    counts = Ref.simulate_days(schedule_r, schedule_s)
    assert counts == [2, 2], f"_selftest_simulate_days: expected [2, 2], got {counts}"

    drifted_s = [dict(day) for day in schedule_s]
    drifted_s[1] = dict(drifted_s[1], query=False)  # flip day 2's query flag -> pair drift
    drifted = False
    try:
        Ref.simulate_days(schedule_r, drifted_s)
    except AssertionError:
        drifted = True
    assert drifted, "_selftest_simulate_days: a query-flag pair mismatch must raise AssertionError"


def _selftest_cut4() -> None:
    """Lightweight generator self-check (task-19-brief.md, additive):
    make_paired_day_schedule()/make_e2e_schedule_pair() each produce a
    well-formed pair (R-SCHED2 pairing already asserted inside
    Ref.simulate_days itself) with the query-day-count invariants their own
    callers rely on. Deliberately small (one seed each, not the full
    100-seed sweep) so import-time cost stays negligible -- mirrors
    _selftest_minors()'s own "smaller than the full fixture sample" note.
    """
    sr, ss = Ref.make_paired_day_schedule(0, n_days=6, universe=150, insert_per_day=8)
    assert len(sr) == 6 and len(ss) == 6
    counts = Ref.simulate_days(sr, ss)
    assert len(counts) >= 2, f"_selftest_cut4: need >=2 query days, got {len(counts)}"

    er, es = Ref.make_e2e_schedule_pair(0)
    e_counts = Ref.simulate_days(er, es)
    assert len(e_counts) == 3, f"_selftest_cut4: e2e schedule must have exactly 3 query days, got {len(e_counts)}"
    assert all(c >= 0 for c in e_counts)


_selftest_minors()  # module self-test (R-MIN): always runs, aborts via AssertionError on failure
_selftest_overflow()  # module self-test (task-18-brief.md SC5/FC3): always runs
_selftest_simulate_days()  # module self-test (task-18-brief.md, R-ORACLE-AGNOSTIC): always runs
_selftest_cut4()  # module self-test (task-19-brief.md, additive): always runs


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

    emit_min = sub.add_parser("emit-min",
                               help="emit MIN-3/MIN-4 fixture rows (task-13-brief.md, W4.2)")
    emit_min.add_argument("--seed", type=int, required=True)
    emit_min.add_argument("--out", type=str, required=True)

    emit_sd = sub.add_parser("emit-sd",
                              help="emit SD-1/SD-2 fixture rows (task-14-brief.md, W4.4)")
    emit_sd.add_argument("--seed", type=int, required=True)
    emit_sd.add_argument("--out", type=str, required=True)

    emit_sched100 = sub.add_parser("emit-sched100",
                                    help="emit SC1's 100-seed schedule sweep fixture (task-19-brief.md)")
    emit_sched100.add_argument("--seed-lo", type=int, required=True)
    emit_sched100.add_argument("--seed-hi", type=int, required=True)
    emit_sched100.add_argument("--out", type=str, required=True)

    emit_e2e = sub.add_parser("emit-e2e",
                               help="emit one E2E-1..4 schedule pair as JSON (task-19-brief.md)")
    emit_e2e.add_argument("--seed", type=int, required=True)
    emit_e2e.add_argument("--out-r", type=str, required=True)
    emit_e2e.add_argument("--out-s", type=str, required=True)

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
    if args.cmd == "emit-min":
        Ref.emit_min_fixtures(args.seed, args.out)
        print(f"wrote {args.out} (seed={args.seed})")
        return 0
    if args.cmd == "emit-sd":
        Ref.emit_sd_fixtures(args.seed, args.out)
        print(f"wrote {args.out} (seed={args.seed})")
        return 0
    if args.cmd == "emit-sched100":
        Ref.emit_sched100_fixture(args.seed_lo, args.seed_hi, args.out)
        print(f"wrote {args.out} (seeds={args.seed_lo}..{args.seed_hi})")
        return 0
    if args.cmd == "emit-e2e":
        Ref.emit_e2e_schedule_pair(args.seed, args.out_r, args.out_s)
        print(f"wrote {args.out_r}, {args.out_s} (seed={args.seed})")
        return 0
    return 1


if __name__ == "__main__":
    raise SystemExit(_cli(sys.argv[1:]))

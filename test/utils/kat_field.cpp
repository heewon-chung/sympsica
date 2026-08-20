// test/utils/kat_field.cpp — Phase-1 formal KAT suite for utils/field.hpp
// (Fp, CoeffCtxFp61) and the wire-endianness negative TV-F1
// (task-3-brief.md, W1.8). Golden source: .handoff/sympsica-test-vectors.md
// FLD-1..7 and TV-F1, reproduced verbatim in task-3-brief.md.
//
// Distinct from test/utils/task2_self_check.cpp (Task 2's throwaway
// self-verification, left untouched) — this file owns the formal,
// row-ID-named golden vectors.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <random>

#include "sympsica/utils/coeff_ctx.hpp"
#include "sympsica/utils/encoding.hpp"
#include "sympsica/utils/field.hpp"
#include "sympsica/utils/serdes.hpp"

#include "fixture_support.hpp"

using namespace sympsica;
using sympsica_test::Fixture;
using sympsica_test::digest_update;
using sympsica_test::fixture_path;
using sympsica_test::splitmix64_next;

// FLD-1 [CONCRETE] — task-27-brief.md Minor #6/R6-NOTAUTO: RELABELED. This is
// a DOCUMENTATION SELF-CHECK, not implementation evidence: it computes
// 97+7, 97*7 -> 3, 73 with plain built-in u64 arithmetic over the toy
// modulus 101 and calls NO sympsica field code at all (Fp is hardcoded to
// p = 2^61-1, so it cannot even be exercised at this modulus). A regression
// in Fp::add or Fp::mul leaves this test green regardless. It exists only to
// confirm the worked example quoted in the test-vector doc is itself
// arithmetically self-consistent, i.e. it can catch a typo in the doc, not a
// defect in the artifact. Production field correctness is exercised by
// FLD-2..7 and MulAssociativityCanonicityFixture1e6PerSeed below, all of
// which call the real Fp class.
TEST(Field, FLD1_DocumentationSelfCheckF101) {
    constexpr u64 p101 = 101;
    EXPECT_EQ((97u + 7u) % p101, 3u);
    EXPECT_EQ((97u * 7u) % p101, 73u);
}

// FLD-2 [IDENTITY]: over F_p (p=2^61-1): (p-1)+2, (p-1)*(p-1) -> 1, 1.
TEST(Field, FLD2_IdentityFp) {
    Fp pm1(Fp::P - 1);
    EXPECT_EQ(pm1.add(Fp(2)), Fp(1));
    EXPECT_EQ(pm1.mul(pm1), Fp(1));
}

// FLD-3 [IDENTITY]: x*inv(x) for 10^4 random nonzero x -> 1, always.
TEST(Field, FLD3_InverseIdentity1e4) {
    std::mt19937_64 rng(0x464C44335F53454Dull); // fixed seed: "FLD3_SEM"-ish
    std::uniform_int_distribution<u64> dist(1, Fp::P - 1);
    for (int i = 0; i < 10000; ++i) {
        Fp x(dist(rng));
        EXPECT_EQ(x.mul(x.inv()), Fp(1));
    }
}

// FLD-4 [SEED-FIXED]: reduce inputs {p-1, p, p+1, 2^61, 2^64-1} ->
// {p-1, 0, 1, 1, 7}. Per the plan's binding requirement, the golden values
// are read from a reference.py-emitted fixture, never hardcoded by hand
// here. fld4 rows are seed-independent (see ref/reference.py:
// Ref.emit_fixtures), so any committed seed's fixture carries the same 5
// rows; seed0.fixture is used as the canonical source.
//
// CORRECTED CLAIM (task-27-brief.md Important #2/R6-NOTAUTO — the original
// comment here claimed this row set would expose a "single-fold mul" bug;
// that claim was WRONG and has been retracted, not just relabeled):
//
// 1. This test calls ONLY Fp::from_u64 (never Fp::mul), so a defect
//    confined to Fp::mul cannot make FLD-4 fail in the first place —
//    from_u64 and mul are separate reduction call sites in field.hpp that
//    happen to share the same fold-and-subtract shape, but exercising one
//    is not exercising the other.
// 2. Separately, and independent of (1): for CANONICAL operands a,b < p,
//    a*b < 2^122 (p < 2^61), so after ONE Mersenne fold
//    r = (t & p) + (t >> 61) we get r < 2p (t>>61 < 2^61, t&p < p), and a
//    SINGLE conditional subtraction already lands r in [0,p). The second
//    fold in the committed double-fold mul()/from_u64() (field.hpp) is
//    therefore harmless but NOT mathematically necessary for canonical
//    in-scope operands — a hypothetical one-fold-plus-subtract
//    implementation would compute the IDENTICAL canonical result on every
//    input in this row set (including the 2^64-1 row: its from_u64 folding
//    starts from a 64-bit, not a 122-bit, product, but the same "one fold
//    already lands under 2p" argument applies: x>>61 < 8, so one fold plus
//    one subtract suffices there too). So "single-fold" is not a wrong
//    construction this row set — or any from_u64/mul input — can
//    distinguish from the committed double-fold; it should not, and does
//    not, fail FLD-4.
//
// A genuinely wrong Fp::mul mutation (one that actually diverges from the
// correct product, e.g. dropping the final conditional subtraction so
// non-canonical results leak through) is exercised by
// MulAssociativityCanonicityFixture1e6PerSeed below via its canonicity check
// — see that test's comment for the R6-NOTAUTO demonstration recorded in
// task-27-report.md.
TEST(Field, FLD4_BoundaryReduction) {
    Fixture fx(fixture_path("test/fixtures/seed0.fixture"));
    auto rows = fx.all("fld4");
    ASSERT_EQ(rows.size(), 5u);
    for (const auto& row : rows) {
        ASSERT_EQ(row.size(), 2u);
        u64 input = std::stoull(row[0]);
        u64 expected = std::stoull(row[1]);
        EXPECT_EQ(Fp::from_u64(input).v, expected)
            << "input = " << input;
    }
}

// FLD-5 [IDENTITY]: pow(g, 0), pow(g, p-1) -> 1, 1 (Fermat).
TEST(Field, FLD5_FermatIdentity) {
    Encoder enc; // real, Encoder-computed generator (not hardcoded)
    Fp g(enc.generator());
    EXPECT_EQ(g.pow(0), Fp(1));
    EXPECT_EQ(g.pow(Fp::P - 1), Fp(1));
}

// FLD-6 [IDENTITY]: serdes round-trip of 10^6 random Fp -> identity.
// Uses a single accumulate-then-assert pattern (rather than 10^6 individual
// EXPECT_EQ calls) to keep the test comfortably inside the Phase-1 SC's
// ~30s/10^6-sample budget.
TEST(Field, FLD6_SerdesRoundTrip1e6) {
    std::mt19937_64 rng(0xF1D6F1D6F1D6F1D6ull);
    std::uniform_int_distribution<u64> dist(0, Fp::P - 1);
    constexpr int kCount = 1'000'000;
    int mismatches = 0;
    for (int i = 0; i < kCount; ++i) {
        Fp x(dist(rng));
        std::array<u8, 8> buf{};
        write_fp(buf, x);
        if (read_fp(buf) != x) ++mismatches;
    }
    EXPECT_EQ(mismatches, 0);
}

// FLD-7 [IDENTITY]: CoeffCtxFp61.fromBlock over 10^6 random blocks -> all
// outputs canonical in [0,p); bias statement documented (coeff_ctx.hpp's
// fromBlock comment: folding the low 64 bits of the block mod P gives
// values in [0, 2^64 mod P) = [0, 8) a ~|8|/2^64 statistical over-weighting
// relative to the rest of [0, P) — negligible for this PoC, not something a
// finite sample here could observe, hence tested as a canonicity property
// only, per W1.8/requirement 6: fromBlock only, binaryDecomposition is
// deferred (not linkable yet).
TEST(Field, FLD7_FromBlockCanonical1e6) {
    CoeffCtxFp61 ctx;
    std::mt19937_64 rng(0xF1D7F1D7F1D7F1D7ull);
    std::uniform_int_distribution<u64> dist;
    constexpr int kCount = 1'000'000;
    int non_canonical = 0;
    for (int i = 0; i < kCount; ++i) {
        osuCrypto::block blk(dist(rng), dist(rng));
        Fp out(0);
        ctx.fromBlock(out, blk);
        if (out.v >= Fp::P) ++non_canonical;
    }
    EXPECT_EQ(non_canonical, 0);
}

// Phase-1 SC property fixture (W1.7(b)): mul associativity vs Python
// bigint via generated fixtures, canonicity v < P after every op. Not tied
// to a single FLD-n row — cross-language equivalence support for all of
// Fp::mul, exercised across every committed seed (seeds 0..9), 10^6 samples
// each (10^7 total Fp::mul calls). The pinned splitmix64 PRNG regenerates
// the exact input sequence reference.py used, so only a PRNG seed + count +
// digest need to be shipped in the fixture (see fixture_support.hpp).
//
// task-27-brief.md Important #2/R6-NOTAUTO: THIS is the correct home for a
// genuinely-wrong Fp::mul mutation demonstration — not FLD-4 above, which
// calls only from_u64 and can never observe a mul-only defect. This test
// DOES call Fp::mul directly (ab = a.mul(b), abc = ab.mul(c), a_bc =
// a.mul(bc)) and gates on three independent properties any wrong mul
// mutation must defeat all of: canonicity (every product < P), associativity
// (abc == a_bc, checked in-process, no reference needed), and the
// cross-language digest (must match reference.py's independent plain-bigint
// mod arithmetic). Demonstrated in task-27-report.md: a temporary
// single-fold-without-final-subtract Fp::mul mutation (dropping the
// `if (r >= P) r -= P;` line) was built and run against this test; it fails
// both the canonicity and digest checks with real recorded output, then was
// reverted (git checkout) and the suite re-confirmed green. Fp::mul itself
// is NOT changed by task-27 — it is, and remains, correct.
TEST(Field, MulAssociativityCanonicityFixture1e6PerSeed) {
    for (int seed = 0; seed <= 9; ++seed) {
        Fixture fx(fixture_path("test/fixtures/seed" + std::to_string(seed) + ".fixture"));
        u64 state = fx.u64_at("mulprop_prng_seed");
        u64 count = fx.u64_at("mulprop_count");
        u64 expected_digest = fx.u64_at("mulprop_digest");
        ASSERT_EQ(count, 1'000'000u) << "seed " << seed;

        u64 digest = sympsica_test::kFnvOffsetBasis;
        u64 canonical_violations = 0;
        u64 assoc_mismatches = 0;
        for (u64 i = 0; i < count; ++i) {
            u64 r1 = splitmix64_next(state);
            u64 r2 = splitmix64_next(state);
            u64 r3 = splitmix64_next(state);
            Fp a = Fp::from_u64(r1);
            Fp b = Fp::from_u64(r2);
            Fp c = Fp::from_u64(r3);

            Fp ab = a.mul(b);
            Fp abc = ab.mul(c);
            Fp bc = b.mul(c);
            Fp a_bc = a.mul(bc);

            if (ab.v >= Fp::P || abc.v >= Fp::P || bc.v >= Fp::P || a_bc.v >= Fp::P) {
                ++canonical_violations;
            }
            if (abc != a_bc) ++assoc_mismatches;

            digest_update(digest, ab.v);
            digest_update(digest, abc.v);
        }
        EXPECT_EQ(canonical_violations, 0u) << "seed " << seed;
        EXPECT_EQ(assoc_mismatches, 0u) << "seed " << seed;
        EXPECT_EQ(digest, expected_digest)
            << "seed " << seed
            << ": Fp::mul's reduction diverges from reference.py's plain "
               "bigint mod arithmetic on this sample sequence";
    }
}

// TV-F1(a) [negative, primary path — controller ruling]: read_fp is
// canonicity-checking (aborts on wire values >= P), so a naive "feed
// big-endian bytes and expect abort" test cannot exercise the interesting
// failure mode: a BE/LE confusion frequently still decodes to SOME
// canonical value, just the WRONG one, silently. This test picks a value
// whose byte-reversed reinterpretation is both < P and != the original
// (0x0100000000000002 <-> 0x0200000000000001, per the ruling's example),
// feeds the byte-reversed encoding through read_fp, and asserts the
// decoded value does NOT match the golden — the mismatch is caught by the
// test itself, not by an abort.
TEST(Field, TVF1_BigEndianMismatchDetectedNotAborted) {
    Fp golden(0x0100000000000002ull);
    std::array<u8, 8> correct_le{};
    write_fp(correct_le, golden);

    std::array<u8, 8> be_confused = correct_le;
    std::reverse(be_confused.begin(), be_confused.end());

    Fp decoded = read_fp(be_confused); // must NOT abort: still canonical
    EXPECT_LT(decoded.v, Fp::P);
    EXPECT_NE(decoded, golden);
    EXPECT_EQ(decoded, Fp(0x0200000000000001ull));
}

// TV-F1(b) [negative, supplementary — controller ruling]: documents that
// when a BE/LE confusion DOES land on a non-canonical value (>= P), the
// canonicity check in read_fp aborts loudly rather than silently accepting
// bad wire data.
TEST(FieldDeathTest, TVF1_BigEndianNonCanonicalAborts) {
    Fp v(255); // LE bytes: FF 00 00 00 00 00 00 00
    std::array<u8, 8> correct_le{};
    write_fp(correct_le, v);

    std::array<u8, 8> be_confused = correct_le; // -> 00 00 00 00 00 00 00 FF
    std::reverse(be_confused.begin(), be_confused.end());
    // Interpreted LE: top byte (bit 56..63) = 0xFF -> value >= 2^61 > P.

    EXPECT_DEATH({ read_fp(be_confused); }, "read_fp: value not canonical");
}

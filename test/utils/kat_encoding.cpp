// test/utils/kat_encoding.cpp — Phase-1 formal KAT suite for
// utils/encoding.hpp (Encoder, BucketOracle) (task-3-brief.md, W1.8).
// Golden source: .handoff/sympsica-test-vectors.md ENC-1..5, reproduced
// verbatim in task-3-brief.md.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <random>
#include <unordered_set>

#include "sympsica/utils/encoding.hpp"
#include "sympsica/utils/field.hpp"

#include "fixture_support.hpp"

using namespace sympsica;
using sympsica_test::Fixture;
using sympsica_test::fixture_path;

// ENC-1 [IDENTITY]: sigma injectivity on 10^5 random distinct ids -> all
// distinct, none zero. Ids are constructed as `base + i` for a randomly
// drawn base (base + 10^5 < 2^60, respecting Encoder::sigma's precondition)
// so distinctness holds by construction, not by chance/rejection sampling.
// sigma(id) = g^id mod p is then injective on these ids for exactly the
// reason Encoder's generator search establishes: g has multiplicative order
// p-1 (verified by GeneratorSearchTerminatesAndIsValid / find_generator's
// full-order Fermat check), so i -> g^i mod p is injective on all of
// Z/(p-1)Z, a fortiori on any 10^5 distinct ids well below that order.
TEST(Encoding, ENC1_SigmaInjectivity1e5) {
    Encoder enc;
    std::mt19937_64 rng(0xE0C1E0C1E0C1E0C1ull);
    constexpr u64 kCount = 100000;
    std::uniform_int_distribution<u64> base_dist(0, (1ull << 60) - kCount - 1);
    u64 base = base_dist(rng);

    std::unordered_set<u64> seen;
    seen.reserve(kCount * 2);
    u64 zero_count = 0;
    for (u64 i = 0; i < kCount; ++i) {
        Fp s = enc.sigma(base + i);
        if (s.is_zero()) ++zero_count;
        seen.insert(s.v);
    }
    EXPECT_EQ(zero_count, 0u);
    EXPECT_EQ(seen.size(), kCount) << "sigma collided on distinct ids";
}

// ENC-2 [CONCRETE]: toy F_101, g=2: sigma(0), sigma(1), sigma(6) ->
// 1, 2, 64. Exercised via Encoder::sigma_generic (the testability hook for
// an arbitrary small modulus/generator pair, independent of Fp's fixed
// p = 2^61-1). Also pins TV-F4 ("sigma as g*x or x^g -> ENC-2 FAILS") in
// two ways: the sigma(6) = 64 row itself already rules out the linear
// misreading g*x = 12 and the power misreading x^g = 6^2 = 36; sigma(1) = g
// exactly is asserted directly against the real (non-toy) Encoder too.
//
// task-27-brief.md Important #1/codex/phase-0-1-review.md: this test alone
// does NOT establish that production Encoder::sigma is exponential -- every
// assertion above either calls the separate sigma_generic hook, or (for
// sigma(1)==g) is ALSO true of the rejected linear reading sigma(id)=g*id.
// See ENC2b_ProductionFieldGoldenRejectsNonExponentialMaps below, which is
// the test that actually closes this finding.
TEST(Encoding, ENC2_ConcreteToyF101) {
    EXPECT_EQ(Encoder::sigma_generic(2, 101, 0), 1u);
    EXPECT_EQ(Encoder::sigma_generic(2, 101, 1), 2u);
    EXPECT_EQ(Encoder::sigma_generic(2, 101, 6), 64u);

    Encoder enc; // sigma(1) = g exactly, on the real field -- necessary but
                 // NOT sufficient (also true of sigma(id)=g*id); see ENC2b.
    EXPECT_EQ(enc.sigma(1), Fp(enc.generator()));
}

// ENC-2b [CONCRETE, production field] — task-27-brief.md Important #1,
// R6-NOTAUTO: the test that actually establishes production Encoder::sigma
// is exponential, on a golden generated independently by ref/reference.py
// (Ref.sigma(6, g, P) via plain Python pow(), NOT copied from any C++
// output) over the REAL field p = 2^61-1, not the toy F_101 hook above.
//
// id=6 is the reviewer's own suggested distinguishing input: the rejected
// non-exponential map sigma(0)=1, sigma(1)=g, sigma(id)=id+100 for id>=2
// agrees with the real g^id at id=0 and id=1 (the only points ENC-1/ENC-2
// pin on the production field) but diverges wildly at id=6, since g^6 mod p
// has no reason to equal 6+100=106 for this p. The g*x linear misreading is
// re-checked here too, now against production Fp/Encoder instead of the toy
// hook (closing the brief's "re-attach TV-F4's g*x negative to a mutation of
// production sigma, not the toy hook").
TEST(Encoding, ENC2b_ProductionFieldGoldenRejectsNonExponentialMaps) {
    Fixture fx(fixture_path("test/fixtures/seed0.fixture"));
    const u64 id = fx.u64_at("enc2_prod_id");
    const u64 golden = fx.u64_at("enc2_prod_sigma");
    ASSERT_EQ(id, 6u);

    Encoder enc;
    const Fp real_sigma = enc.sigma(id);
    ASSERT_EQ(real_sigma, Fp(golden))
        << "production Encoder::sigma(6) diverged from ref/reference.py's "
           "independently-computed g^6 mod p";

    // Reviewer's counterexample map: sigma(0)=1, sigma(1)=g, sigma(id)=id+100
    // for id>=2. It survives ENC-1 (injective/nonzero) and ENC-2's
    // sigma(1)==g check, but must be REJECTED by this golden.
    const Fp wrong_nonexponential(id + 100);
    EXPECT_NE(wrong_nonexponential, real_sigma)
        << "R6-NOTAUTO: the reviewer's non-exponential map sigma(id)=id+100 "
           "(id>=2) must be distinguishable from the real g^id at id=6 -- "
           "wrong value would be " << (id + 100) << ", real production "
           "sigma(6) is " << real_sigma.v;

    // TV-F4's "sigma as g*x" misreading, re-attached to PRODUCTION Fp/g
    // (not the toy-field g=2,p=101,x=6 hook TVF4_LinearAndPowerMisreadingsGiveWrongValue
    // below still uses for its own, separate, toy-field pinning).
    const Fp wrong_linear = Fp(enc.generator()).mul(Fp(id));
    EXPECT_NE(wrong_linear, real_sigma)
        << "R6-NOTAUTO: the g*x misreading, computed on the PRODUCTION "
           "field, must be distinguishable from the real g^id at id=6 -- "
           "wrong value would be " << wrong_linear.v << ", real production "
           "sigma(6) is " << real_sigma.v;
}

// --- TV-F4 (negative-as-positive-assert, task-20-brief.md W6.1 / R6-FSUITE:
// "a row with no wrong construction yet is NEW WORK"). ENC-2 above only
// PINS the correct value (64) and documents the two misreadings in a
// comment; this test makes the misreadings EXECUTABLE and asserts they are
// distinguishable from the real g^x, over the SAME toy field/point ENC-2
// uses (g=2, p=101, x=6).
TEST(Encoding, TVF4_LinearAndPowerMisreadingsGiveWrongValue) {
    constexpr u64 g = 2, p = 101, x = 6;
    const u64 real_sigma = Encoder::sigma_generic(g, p, x);
    ASSERT_EQ(real_sigma, 64u); // re-pin ENC-2's own value as this test's precondition

    const u64 wrong_linear = (g * x) % p; // the "sigma as g*x" misreading
    u64 wrong_power = 1;                  // the "sigma as x^g" misreading
    for (u64 i = 0; i < g; ++i) wrong_power = (wrong_power * x) % p;

    EXPECT_NE(wrong_linear, real_sigma)
        << "TV-F4: the g*x misreading must be distinguishable from the real g^x";
    EXPECT_NE(wrong_power, real_sigma)
        << "TV-F4: the x^g misreading must be distinguishable from the real g^x";
}

// ENC-3 [IDENTITY]: BucketOracle range on 10^6 ids -> all in [1..m];
// chi-square uniformity sanity (non-gating: reported, does not fail the
// build on a marginal statistic). The only GATING assertion is the range
// check; the chi-square figure is printed for a human to eyeball, folding
// the M = 2^31 output space into 256 bins (expected count/bin = 1e6/256 ~
// 3906, comfortably above the >=5 rule of thumb for a valid chi-square
// approximation).
TEST(Encoding, ENC3_BucketRangeAndUniformitySanity1e6) {
    BucketOracle oracle; // epoch 0, all-zero salt
    constexpr u64 kCount = 1'000'000;
    constexpr int kBins = 256;
    std::array<u64, kBins> bins{};
    u32 min_b = std::numeric_limits<u32>::max();
    u32 max_b = 0;

    for (u64 id = 0; id < kCount; ++id) {
        u32 b = oracle.of(id);
        min_b = std::min(min_b, b);
        max_b = std::max(max_b, b);
        bins[b % kBins]++;
    }
    EXPECT_GE(min_b, 1u);
    EXPECT_LE(max_b, static_cast<u32>(BucketOracle::M));

    double expected = static_cast<double>(kCount) / kBins;
    double chi2 = 0.0;
    for (int i = 0; i < kBins; ++i) {
        double d = static_cast<double>(bins[i]) - expected;
        chi2 += d * d / expected;
    }
    std::fprintf(stderr,
                 "[ENC-3] chi-square (%d bins, %llu samples) = %f "
                 "(informational, non-gating; ~%d bins -> critical value "
                 "~293 at alpha=0.05)\n",
                 kBins, static_cast<unsigned long long>(kCount), chi2, kBins - 1);
}

// ENC-4 [IDENTITY]: same id under two salts -> different buckets with
// prob ~ 1 - 1/m (10^4 ids, >99.9% differ).
TEST(Encoding, ENC4_SaltChangesBucketWithHighProbability1e4) {
    std::array<u8, 4> salt_a{1, 2, 3, 4};
    std::array<u8, 4> salt_b{5, 6, 7, 8};
    BucketOracle oracle_a = BucketOracle::refreshed(salt_a, {});
    BucketOracle oracle_b = BucketOracle::refreshed(salt_b, {});
    ASSERT_NE(oracle_a.salt(), oracle_b.salt());

    constexpr u64 kCount = 10000;
    u64 differ = 0;
    for (u64 id = 0; id < kCount; ++id) {
        if (oracle_a.of(id) != oracle_b.of(id)) ++differ;
    }
    double frac = static_cast<double>(differ) / static_cast<double>(kCount);
    EXPECT_GT(frac, 0.999);
}

// ENC-5 [POST-GATE]: the smallest generator g, computed by Encoder, is
// pinned as a checked-in golden (test/fixtures/generator_g.txt). Per the
// plan's ruling, that file was generated FROM a real Encoder() run (see
// test/utils/task2_self_check.cpp's GeneratorSearchTerminatesAndIsValid
// stderr output, g = 37) and cross-checked independently in
// ref/reference.py's find_generator() (same 12-factor order test, Python
// bigint) before being committed — see task-3-report.md for that
// cross-check's output. This test only compares against the pinned file;
// it does not regenerate it.
TEST(Encoding, ENC5_GeneratorPinnedGolden) {
    std::ifstream f(fixture_path("test/fixtures/generator_g.txt"));
    ASSERT_TRUE(f.is_open()) << "missing pinned golden: test/fixtures/generator_g.txt";
    u64 golden_g = 0;
    f >> golden_g;
    ASSERT_GT(golden_g, 1u);

    Encoder enc;
    EXPECT_EQ(enc.generator(), golden_g);
}

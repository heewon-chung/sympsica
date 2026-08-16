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
TEST(Encoding, ENC2_ConcreteToyF101) {
    EXPECT_EQ(Encoder::sigma_generic(2, 101, 0), 1u);
    EXPECT_EQ(Encoder::sigma_generic(2, 101, 1), 2u);
    EXPECT_EQ(Encoder::sigma_generic(2, 101, 6), 64u);

    Encoder enc; // TV-F4: sigma(1) = g exactly, on the real field.
    EXPECT_EQ(enc.sigma(1), Fp(enc.generator()));
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

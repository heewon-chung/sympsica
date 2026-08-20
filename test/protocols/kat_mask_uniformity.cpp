// test/protocols/kat_mask_uniformity.cpp — task-24-brief.md W6.6(i)/
// R6-CHISQ (SC4): recomputes the pre-registered chi-square statistics
// (test/utils/mask_chisq.hpp) from the 10^5-mask fixture
// test/campaign/mask_campaign_gen.cpp committed (test/fixtures/
// mask_uniformity_1e5.fixture) -- this test itself runs in seconds; the
// ~1.5-2h cost is paid ONCE by the campaign generator, registered
// separately (CMakeLists.txt, labels "heavy;campaign", excluded from the
// default ctest set).
//
// FC3 [chi-square non-vacuity]: the SAME chisq machinery is fed a
// deliberately biased synthetic sample and must REJECT at the
// pre-registered level -- a uniformity test that has never rejected
// anything is not a test.

#include <gtest/gtest.h>

#include <cstdio>
#include <vector>

#include "sympsica/protocols/detail/ztgate_pipeline.hpp"

#include "../utils/fixture_support.hpp"
#include "../utils/mask_chisq.hpp"

namespace {

namespace zt = sympsica::ztgate;
using sympsica::u64;
using sympsica_test::analyze_digit_position;
using sympsica_test::DigitPositionStats;
using sympsica_test::Fixture;
using sympsica_test::fixture_path;
using sympsica_test::mask_digit_split;

std::vector<u64> load_committed_masks() {
    Fixture fx(fixture_path("test/fixtures/mask_uniformity_1e5.fixture"));
    std::vector<u64> masks;
    for (const auto& row : fx.all("mask")) {
        masks.push_back(std::stoull(row.at(0)));
    }
    return masks;
}

} // namespace

// ---------------------------------------------------------------------------
// SC4 [MASK-UNIFORM]: the pre-registered binned chi-square, all four digit
// positions, real pipeline output.
// ---------------------------------------------------------------------------
TEST(MaskUniformity, SC4_BinnedChiSquarePassesForAllFourDigitPositionsOfCommittedFixture) {
    const std::vector<u64> masks = load_committed_masks();
    ASSERT_EQ(masks.size(), 100000u)
        << "committed fixture must hold exactly the spec-side 10^5 masks (R6-MASKSTAT)";

    for (int digit_pos = 0; digit_pos < 4; ++digit_pos) {
        SCOPED_TRACE("digit position " + std::to_string(digit_pos));
        const DigitPositionStats st = analyze_digit_position(masks, digit_pos);

        std::fprintf(stderr,
                     "[MaskUniformity] digit %d: full_res(bins=%llu)=%.4f (caveat: expected/bin=%.3f, "
                     "%s validity floor 5) binned(bins=%llu)=%.4f crit=%.4f -> %s\n",
                     digit_pos, (unsigned long long)st.full_resolution_bins, st.full_resolution_chisq,
                     100000.0 / static_cast<double>(st.full_resolution_bins),
                     (100000.0 / static_cast<double>(st.full_resolution_bins) >= 5.0) ? "at/above" : "BELOW",
                     (unsigned long long)st.binned_bins, st.binned_chisq, st.binned_critical_value,
                     st.binned_rejects ? "REJECT" : "pass");

        // Pass/fail decision uses ONLY the binned statistic (R6-CHISQ).
        EXPECT_FALSE(st.binned_rejects)
            << "digit " << digit_pos << ": binned chi-square " << st.binned_chisq
            << " exceeds the pre-registered critical value " << st.binned_critical_value
            << " (alpha=" << sympsica_test::kAlpha << ", df=" << (st.binned_bins - 1) << ")";

        // The full-resolution statistic is reported (above), never asserted on.
    }
}

// ---------------------------------------------------------------------------
// FC3 [chi-square non-vacuity]: a deliberately biased top-digit sample must
// be REJECTED by the exact same analyze_digit_position() call SC4 uses.
// ---------------------------------------------------------------------------
TEST(MaskUniformity, FC3_DeliberatelyBiasedTopDigitSampleIsRejected) {
    // 10^5 synthetic values: low 48 bits vary (irrelevant to this leg), top
    // 13-bit digit confined to a 10-value cycle {0..9} instead of the full
    // [0, 8192) range -- a massive, obvious departure from uniform.
    std::vector<u64> biased;
    biased.reserve(100000);
    for (u64 i = 0; i < 100000; ++i) {
        const u64 low48 = i * 0x9E3779B97F4A7C15ull; // arbitrary spread, irrelevant to this leg
        const u64 top13 = i % 10;                    // heavily biased: only 10 of 8192 values ever occur
        const u64 v = (low48 & 0xFFFFFFFFFFFFull) | (top13 << 48);
        biased.push_back(v);
    }

    const DigitPositionStats st = analyze_digit_position(biased, /*digit_pos=*/3);
    std::fprintf(stderr, "[MaskUniformity FC3] top-digit binned chisq=%.2f (crit=%.4f) -> %s\n",
                 st.binned_chisq, st.binned_critical_value, st.binned_rejects ? "REJECT (expected)" : "pass (BUG)");

    EXPECT_TRUE(st.binned_rejects) << "a top digit confined to 10 of 8192 possible values must be "
                                       "flagged non-uniform by the pre-registered test -- got chisq="
                                    << st.binned_chisq << " vs crit=" << st.binned_critical_value;
    // Sanity: the OTHER three (unbiased-by-construction) digit positions of
    // this SAME synthetic sample should NOT reject -- confirms the rejection
    // above is specific to the biased digit, not an artifact of the whole
    // synthetic sample being degenerate.
    for (int digit_pos = 0; digit_pos < 3; ++digit_pos) {
        const DigitPositionStats other = analyze_digit_position(biased, digit_pos);
        EXPECT_FALSE(other.binned_rejects)
            << "digit " << digit_pos << " was constructed to vary freely and should not reject";
    }
}

// ---------------------------------------------------------------------------
// Cross-check: the test-support digit_split copy (mask_chisq.hpp) matches
// the REAL production digit_split (protocols/detail/ztgate_pipeline.hpp)
// bit-for-bit -- catches a drift between the two independent copies, per
// mask_chisq.hpp's own doc comment on why a duplicate exists at all.
// ---------------------------------------------------------------------------
TEST(MaskUniformity, TestSupportDigitSplitMatchesProductionDigitSplit) {
    const std::vector<u64> probes = {0ull, 1ull, zt::kRejectedMask - 1, 0x1FFFFFFFFFFFFFFFull & zt::kRejectedMask,
                                      0xAAAAAAAAAAAAAAAAull, 0x5555555555555555ull, 123456789ull};
    for (u64 v : probes) {
        const auto want = zt::digit_split(v);
        const auto got = mask_digit_split(v);
        for (u64 k = 0; k < 4; ++k) {
            EXPECT_EQ(got[k], want[k]) << "digit " << k << " of value " << v;
        }
    }
}

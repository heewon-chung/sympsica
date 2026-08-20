#ifndef SYMPSICA_TEST_MASK_CHISQ_HPP
#define SYMPSICA_TEST_MASK_CHISQ_HPP

// test/utils/mask_chisq.hpp — task-24-brief.md W6.6(i)/R6-CHISQ: the shared
// chi-square-over-digit-marginals statistic. ONE production computation,
// used by BOTH test/protocols/kat_mask_uniformity.cpp's SC4 (recomputed in
// seconds from the committed 10^5-mask fixture) and that same file's FC3
// non-vacuity leg (fed a deliberately biased synthetic sample) -- not two
// independently hand-derived copies (R6-NOTAUTO: an assertion whose BOTH
// sides are hand-derived from the same inputs tests the arithmetic, not the
// system).
//
// PRE-REGISTERED DECISION RULE (binding per R6-CHISQ -- fixed BEFORE this
// task's own 10^5-mask sample was generated or inspected; do not adjust
// either the significance level or the accept/reject rule after seeing a
// result):
//
//   - significance level alpha = 0.01 PER digit-position marginal. Chosen
//     stricter than the conventional 0.05 specifically because FOUR
//     marginals are tested (one per digit position); this is not a formal
//     Bonferroni correction, but a deliberate margin against the higher
//     combined false-positive rate of testing four marginals at once.
//   - the PASS/FAIL decision uses the BINNED statistic ONLY: 256
//     consecutive raw digit values per bin. For the three 16-bit digits
//     (65536 possible values) that is 256 bins, expected count
//     10^5/256 ~= 390.6 per bin. For the 13-bit TOP digit (8192 possible
//     values -- NOT 16 bits; kTopDigitBits in
//     protocols/detail/ztgate_pipeline.hpp is 13, and the four marginals
//     are NOT identical) that is 32 bins, expected 10^5/32 = 3125 per bin.
//     Both are comfortably over the conventional chi-square validity floor
//     of 5 expected/bin.
//   - REJECT (this test FAILS) a digit position iff its binned statistic
//     exceeds the chi-square upper-alpha critical value for that binning's
//     degrees of freedom (bins-1):
//       * three 16-bit digits: df=255, crit(255, 0.01) = 310.4574
//       * top 13-bit digit:    df=31,  crit(31,  0.01) = 52.1914
//     Both critical values were computed via a regularized-incomplete-gamma
//     bisection solver (standard Numerical Recipes series/continued-
//     fraction algorithm) and independently cross-checked against the
//     Wilson-Hilferty cube-root normal approximation, which agreed to
//     within 0.15 in both cases (see task-24-report.md for the derivation
//     script and both numbers). This is a fixed, pre-registered constant --
//     not recomputed at test time (no chi-square-inverse library is linked
//     into this project).
//   - the FULL-RESOLUTION statistic (one bin per raw digit value: 65536
//     bins for the three 16-bit digits, 8192 for the top digit) is ALSO
//     computed and reported, but NEVER used for pass/fail. At N=10^5, the
//     three 16-bit digits' full-resolution expected count is ~1.53/bin,
//     far below the validity floor -- reported with that caveat, not as a
//     sound test. The top digit's OWN full-resolution table (8192 bins,
//     expected ~12.2/bin) is actually valid on its own by the same floor;
//     it is still reported-only here (not used for pass/fail) so all four
//     digit positions share one consistent decision rule (the binned
//     statistic), rather than silently switching rules for one of the four.

#include <array>
#include <cstdint>
#include <vector>

namespace sympsica_test {

using u64 = std::uint64_t;

// Carry-less little-endian digit split, mirroring
// sympsica::ztgate::digit_split's contract (digits 0..2 are 16 bits, digit
// 3 is the narrow 13-bit top digit) bit-for-bit. Deliberately a SEPARATE,
// independent copy rather than #include-ing the production pipeline header
// (this file is pure statistics infrastructure with zero pipeline
// dependency, reusable by both the real-pipeline SC4 test and the
// synthetic-sample FC3 test); kat_mask_uniformity.cpp's own SC4 path
// cross-checks this copy against ztgate::digit_split's real output
// directly, so a drift between the two would be caught, not silently
// trusted.
inline std::array<u64, 4> mask_digit_split(u64 v) {
    v &= (u64(1) << 61) - 1;
    return {v & 0xFFFFull, (v >> 16) & 0xFFFFull, (v >> 32) & 0xFFFFull, (v >> 48) & 0x1FFFull};
}

struct DigitPositionStats {
    double full_resolution_chisq = 0.0;
    u64 full_resolution_bins = 0;
    double binned_chisq = 0.0;
    u64 binned_bins = 0;
    double binned_critical_value = 0.0;
    bool binned_rejects = false; // true == this digit position FAILS the pre-registered test
};

// Pre-registered critical values -- see this file's header comment.
inline constexpr double kAlpha = 0.01;
inline constexpr double kCritDf255Alpha01 = 310.4574; // 256-bin marginals (16-bit digits)
inline constexpr double kCritDf31Alpha01 = 52.1914;   // 32-bin marginal (13-bit top digit)
inline constexpr u64 kBinWidth = 256; // consecutive raw digit values per bin

// Pearson chi-square goodness-of-fit statistic for a uniform distribution
// over `counts.size()` equiprobable bins, given a common expected count per
// bin.
inline double chisq_uniform(const std::vector<u64>& counts, double expected_per_bin) {
    double stat = 0.0;
    for (u64 c : counts) {
        const double diff = static_cast<double>(c) - expected_per_bin;
        stat += diff * diff / expected_per_bin;
    }
    return stat;
}

// analyze_digit_position(masks, digit_pos) — full computation for ONE of
// the four digit positions (digit_pos in [0,3]) over the whole `masks`
// sample. `masks` are raw 61-bit logical mask values r = r_R XOR r_S (the
// pipeline's own step-1 output, task-24-report.md's committed fixture
// format).
inline DigitPositionStats analyze_digit_position(const std::vector<u64>& masks, int digit_pos) {
    const u64 domain_bits = (digit_pos == 3) ? 13 : 16; // R6-CHISQ: top digit is 13 bits, not 16
    const u64 domain = u64(1) << domain_bits;
    const double n = static_cast<double>(masks.size());

    std::vector<u64> full_counts(domain, 0);
    for (u64 m : masks) {
        const auto d = mask_digit_split(m);
        ++full_counts[d[static_cast<std::size_t>(digit_pos)]];
    }

    DigitPositionStats st;
    st.full_resolution_bins = domain;
    st.full_resolution_chisq = chisq_uniform(full_counts, n / static_cast<double>(domain));

    // NOTE (Phase-6 gate review, Important 1): val/kBinWidth is a HIGH-BIT
    // projection -- the binned statistic below, which is the only asserted
    // one, cannot see any bias confined within a single 256-value bin. See
    // the claim boundary at the head of test/protocols/kat_mask_uniformity.cpp.
    const u64 num_bins = domain / kBinWidth; // 256 for 16-bit digits, 32 for the 13-bit top digit
    std::vector<u64> binned_counts(num_bins, 0);
    for (u64 val = 0; val < domain; ++val) {
        binned_counts[val / kBinWidth] += full_counts[val];
    }
    st.binned_bins = num_bins;
    st.binned_chisq = chisq_uniform(binned_counts, n / static_cast<double>(num_bins));
    st.binned_critical_value = (digit_pos == 3) ? kCritDf31Alpha01 : kCritDf255Alpha01;
    st.binned_rejects = (st.binned_chisq > st.binned_critical_value);
    return st;
}

} // namespace sympsica_test

#endif // SYMPSICA_TEST_MASK_CHISQ_HPP

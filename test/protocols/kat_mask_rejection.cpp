// test/protocols/kat_mask_rejection.cpp — task-24-brief.md W6.6(i)/
// R6-MASKREJ (SC5): "the rejection path exercised by injection" is a
// REQUIRED leg, not optional. Uses the existing test-support knob
// PipelineOpts::forced_mask_halves (protocols/detail/ztgate_pipeline.hpp,
// the Phase-2 test/integration/w24_boundary_masks.cpp precedent) to force
// r = kRejectedMask (= p, the one value step 2 must reject) through the
// real pipeline, and asserts the rejection actually happened via
// PipelineStats::rejected -- a resample counter, not an inference.

#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "coproto/Socket/LocalAsyncSock.h"
#include "cryptoTools/Crypto/PRNG.h"
#include "macoro/trace.h"
#include "macoro/sync_wait.h"
#include "macoro/when_all.h"

#include "sympsica/protocols/detail/ztgate_pipeline.hpp"
#include "sympsica/utils/field.hpp"

namespace {

namespace zt = sympsica::ztgate;
namespace oc = osuCrypto;
using sympsica::Fp;
using sympsica::u64;

// Same single-thread cooperative-scheduling pattern as
// w24_boundary_masks.cpp's private eval()/TwoParty (LocalAsyncSocket
// requires both sides pumped on one thread); a private copy here, this
// file owns its own small harness (same precedent as that file's own
// comment on not reaching into another file's anonymous namespace).
void eval(macoro::task<>& t0, macoro::task<>& t1) {
    auto r = macoro::sync_wait(macoro::when_all_ready(std::move(t0), std::move(t1)));
    std::get<0>(r).result();
    std::get<1>(r).result();
}

u64 pool_size(u64 gates, u64 extra_rejections) {
    return zt::kOtsPerGate * gates + zt::kAndGates * extra_rejections;
}

struct TwoParty {
    std::array<coproto::LocalAsyncSocket, 2> sock = coproto::LocalAsyncSocket::makePair();
    std::array<zt::OtPool, 2> pool;
    std::array<oc::PRNG, 2> ot_prng{oc::PRNG(oc::block(913, 0)), oc::PRNG(oc::block(913, 1))};

    static constexpr zt::Role role(int i) { return i == 0 ? zt::Role::Receiver : zt::Role::Sender; }

    void fill_pool(u64 ots) {
        auto t0 = zt::generate_ot_pool(role(0), ots, ot_prng[0], sock[0], pool[0]);
        auto t1 = zt::generate_ot_pool(role(1), ots, ot_prng[1], sock[1], pool[1]);
        eval(t0, t1);
    }

    void run(const std::array<zt::PipelineOpts, 2>& opts, std::array<oc::PRNG, 2>& prng,
             std::array<std::vector<zt::ZtGateOut>, 2>& out, std::array<zt::PipelineStats, 2>& stats) {
        auto t0 = zt::generate_ztgates(role(0), sock[0], opts[0], prng[0], pool[0], out[0], stats[0]);
        auto t1 = zt::generate_ztgates(role(1), sock[1], opts[1], prng[1], pool[1], out[1], stats[1]);
        eval(t0, t1);
    }
};

} // namespace

TEST(MaskRejection, SC5_ForcedRejectedMaskIsResampledAndCounted) {
    // Force gate 0's logical mask to r = kRejectedMask (= Fp::P, the ONE
    // value step 2 must reject WITHOUT revealing r); split non-trivially
    // across the two parties (neither holds r in the clear), same
    // convention as w24_boundary_masks.cpp/kat_dkg_vole61.cpp.
    const u64 r_forced = zt::kRejectedMask;
    const u64 s_half = 0x0AAAAAAAAAAAAAAAull & Fp::P;
    std::array<std::vector<u64>, 2> forced;
    forced[0].push_back(r_forced ^ s_half);
    forced[1].push_back(s_half);

    TwoParty tp;
    // Slack budget: the forced gate resamples at least once (guaranteed --
    // the forced value is exactly the rejected one), and the resampled
    // replacement is drawn at random (rejection probability 2^-61, so a
    // SECOND rejection on the same gate is not expected but the slack
    // covers max_resample_rounds worth of headroom regardless).
    tp.fill_pool(pool_size(/*gates=*/1, /*extra_rejections=*/zt::PipelineOpts{}.max_resample_rounds));

    std::array<zt::PipelineOpts, 2> opts;
    for (int p = 0; p < 2; ++p) {
        opts[p].count = 1;
        opts[p].forced_mask_halves = forced[p];
    }
    std::array<oc::PRNG, 2> prng{oc::PRNG(oc::block(914, 0)), oc::PRNG(oc::block(914, 1))};
    std::array<std::vector<zt::ZtGateOut>, 2> out;
    std::array<zt::PipelineStats, 2> stats;
    tp.run(opts, prng, out, stats);

    // The rejection actually happened -- a counter incremented by the real
    // pipeline, not an assertion inferred from the output shape.
    ASSERT_GE(stats[0].rejected, 1u) << "party 0's PipelineStats::rejected did not observe the forced rejection";
    ASSERT_GE(stats[1].rejected, 1u) << "party 1's PipelineStats::rejected did not observe the forced rejection";
    EXPECT_EQ(stats[0].rejected, stats[1].rejected) << "both parties observe the SAME publicly-opened rejection bit";
    ASSERT_GE(stats[0].resample_rounds, 1u);

    // The final output mask must NOT be the rejected value (the pipeline
    // resampled a fresh, uniform replacement after the forced rejection) --
    // real production behavior, not merely "a rejection was counted".
    ASSERT_EQ(out[0].size(), 1u);
    ASSERT_EQ(out[1].size(), 1u);
    const u64 final_mask = out[0][0].mask_half ^ out[1][0].mask_half;
    EXPECT_NE(final_mask, r_forced) << "the resampled replacement must not equal the rejected mask";
    EXPECT_NE(final_mask, zt::kRejectedMask);
}

// FC-style non-vacuity for this same file: an UNforced gate (ordinary
// sampling, no forced_mask_halves) essentially never rejects (probability
// 2^-61) -- confirms the counter above is a genuine signal of the FORCED
// injection, not something that fires unconditionally on every run.
TEST(MaskRejection, OrdinarySampledGateDoesNotReject) {
    TwoParty tp;
    tp.fill_pool(pool_size(/*gates=*/4, /*extra_rejections=*/2));

    std::array<zt::PipelineOpts, 2> opts;
    for (int p = 0; p < 2; ++p) opts[p].count = 4; // no forced_mask_halves: ordinary sampling
    std::array<oc::PRNG, 2> prng{oc::PRNG(oc::block(915, 0)), oc::PRNG(oc::block(915, 1))};
    std::array<std::vector<zt::ZtGateOut>, 2> out;
    std::array<zt::PipelineStats, 2> stats;
    tp.run(opts, prng, out, stats);

    EXPECT_EQ(stats[0].rejected, 0u);
    EXPECT_EQ(stats[1].rejected, 0u);
}

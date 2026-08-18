// kat_dkg_vole61.cpp — Phase-2 W2.1 gate tests for the mask-and-key pipeline
// (test/integration/ztgate_pipeline.{hpp,cpp}).
//
// Test IDs carried in the test names:
//   ZT-3  [CONCRETE]  — digit split of a pinned 61-bit value.
//   ZT-5  [POST-GATE] — distributed key generation at 61 bits: 4 trees,
//                       10^3 random probes per tree, over 8 gates (seeds 0..7),
//                       with expansion digests pinned in
//                       test/fixtures/zt5_dkg61.fixture.
// plus mask/B2A correctness over 32 gates and the step-2 rejection tests.
//
// ==========================================================================
// TASK 6 REGION — VOLE-triple tests (W2.2) belong below this banner, in this
// file or a sibling test/integration/kat_vole61.cpp. Nothing above it depends
// on VOLE; the shared two-party plumbing is in the anonymous namespace at the
// top of this file and is meant to be reused.
// ==========================================================================

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <string>
#include <vector>

#include "coproto/Socket/LocalAsyncSock.h"
#include "macoro/sync_wait.h"
#include "macoro/when_all.h"

#include "../utils/fixture_support.hpp"
#include "sympsica/utils/field.hpp"
#include "ztgate_pipeline.hpp"

namespace {

namespace zt = sympsica::ztgate;
namespace oc = osuCrypto;
using sympsica::Fp;
using sympsica::u64;

// Runs the two parties' coroutines to completion on this thread. coproto's
// LocalAsyncSocket resumes the peer whenever one side blocks, so a single
// thread is enough (this is exactly what libOTe's own Dpf_Tests.cpp does).
// Exceptions are re-thrown after BOTH sides have been reaped, so a failure on
// one side cannot leave the other's frame dangling.
void eval(macoro::task<>& t0, macoro::task<>& t1)
{
    auto r = macoro::sync_wait(macoro::when_all_ready(std::move(t0), std::move(t1)));
    std::get<0>(r).result();
    std::get<1>(r).result();
}

// Build tag for the pinned-digest rows. The expansion transcript is fully
// determined by the two PRNG seeds, but the libOTe code paths that produce it
// are selected at configure time by ENABLE_SSE, which CMakeLists.txt derives
// from the host architecture — so the tag records which variant produced the
// pin. See test/fixtures/zt5_dkg61.fixture's header.
std::string build_tag()
{
#if defined(__aarch64__) || defined(_M_ARM64)
    std::string arch = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    std::string arch = "x86_64";
#else
    std::string arch = "unknown";
#endif
#ifdef ENABLE_SSE
    return arch + "-sse";
#else
    return arch + "-nosse";
#endif
}

// A two-party in-process harness: one socket pair, one silent-OT pool per
// party, reusable across several pipeline runs.
struct TwoParty {
    // LocalAsyncSocket derives from coproto::Socket, so these bind directly to
    // the pipeline's type-erased `coproto::Socket&` parameters — Task 8 (W2.4)
    // swaps in coproto's asio/TCP socket at the same call sites.
    std::array<coproto::LocalAsyncSocket, 2> sock = coproto::LocalAsyncSocket::makePair();
    std::array<zt::OtPool, 2> pool;
    std::array<oc::PRNG, 2> ot_prng{oc::PRNG(oc::block(97, 0)), oc::PRNG(oc::block(97, 1))};

    static constexpr zt::Role role(int i) { return i == 0 ? zt::Role::Receiver : zt::Role::Sender; }

    void fill_pool(u64 ots)
    {
        auto t0 = zt::generate_ot_pool(role(0), ots, ot_prng[0], sock[0], pool[0]);
        auto t1 = zt::generate_ot_pool(role(1), ots, ot_prng[1], sock[1], pool[1]);
        eval(t0, t1);
    }

    // One symmetric pipeline run. `opts` and `prng` are indexed by party.
    void run(const std::array<zt::PipelineOpts, 2>& opts, std::array<oc::PRNG, 2>& prng,
             std::array<std::vector<zt::ZtGateOut>, 2>& out,
             std::array<zt::PipelineStats, 2>& stats)
    {
        auto t0 = zt::generate_ztgates(role(0), sock[0], opts[0], prng[0], pool[0], out[0], stats[0]);
        auto t1 = zt::generate_ztgates(role(1), sock[1], opts[1], prng[1], pool[1], out[1], stats[1]);
        eval(t0, t1);
    }
};

// Pool size for `gates` gates plus `extra_rejections` forced resamples (a
// resample re-runs step 2 only, so it costs kAndGates more OTs).
u64 pool_size(u64 gates, u64 extra_rejections = 0)
{
    return zt::kOtsPerGate * gates + zt::kAndGates * extra_rejections;
}

// ---------------------------------------------------------------------------
// Correlated randomness — the foundation everything else draws on
// ---------------------------------------------------------------------------

TEST(ZtGatePipeline, OtPoolIsConsistentAcrossParties)
{
    TwoParty tp;
    tp.fill_pool(256);
    ASSERT_EQ(tp.pool[0].size(), tp.pool[1].size());

    // The defining random-OT relation, in both directions: what one party
    // learned as receiver is the peer's sender message at the choice bit.
    for (u64 i = 0; i < tp.pool[0].size(); ++i) {
        ASSERT_EQ(tp.pool[0].recv[i], tp.pool[1].send[i][tp.pool[0].choices[i]]) << i;
        ASSERT_EQ(tp.pool[1].recv[i], tp.pool[0].send[i][tp.pool[1].choices[i]]) << i;
    }
    EXPECT_EQ(tp.pool[0].consumed(), 0u);
    EXPECT_EQ(tp.pool[0].remaining(), tp.pool[0].size());
}

// ---------------------------------------------------------------------------
// ZT-3 [CONCRETE] — digit split
// ---------------------------------------------------------------------------

TEST(ZtGatePipeline, ZT3_DigitSplitOfPinnedValue)
{
    // Plan row: z = 0x0123_4567_89AB_CDEF masked to 61 bits splits (LE, 16-bit
    // digits, narrow 13-bit top digit) into [0xCDEF, 0x89AB, 0x4567,
    // 0x0123 & 0x1FFF].
    const u64 z = 0x0123456789ABCDEFull;
    auto d = zt::digit_split(z);

    EXPECT_EQ(d[0], 0xCDEFull);
    EXPECT_EQ(d[1], 0x89ABull);
    EXPECT_EQ(d[2], 0x4567ull);
    EXPECT_EQ(d[3], 0x0123ull & 0x1FFFull);

    // The split must be exactly the low 61 bits of z, reassembled.
    u64 back = d[0] | (d[1] << 16) | (d[2] << 32) | (d[3] << 48);
    EXPECT_EQ(back, z & Fp::P);

    // Every digit stays inside the DPF domain, and the top one inside its
    // narrower range.
    for (auto x : d) EXPECT_LT(x, zt::kDomain);
    EXPECT_LT(d[3], u64(1) << zt::kTopDigitBits);

    // Carry-less: splitting is a homomorphism for XOR, which is what makes
    // step 4 free.
    const u64 a = 0x1FEDCBA987654321ull & Fp::P;
    const u64 b = 0x0A5A5A5A5A5A5A5Aull & Fp::P;
    auto da = zt::digit_split(a), db = zt::digit_split(b), dab = zt::digit_split(a ^ b);
    for (u64 j = 0; j < zt::kNumDigits; ++j) EXPECT_EQ(da[j] ^ db[j], dab[j]) << j;
}

// ---------------------------------------------------------------------------
// Step 2 — rejection: the opened bit is exactly [r == 2^61-1]
// ---------------------------------------------------------------------------

TEST(ZtGatePipeline, RejectionOpenedBitEqualsPredicate)
{
    // Rejection fires with probability 2^-61, so it is unreachable by sampling;
    // it is covered by injecting the masks directly.
    const std::vector<u64> targets{0ull, 1ull, Fp::P /* 2^61-1 */, Fp::P - 1};

    // Use a non-trivial split so the GMW circuit sees real sharing rather than
    // one party holding r in the clear.
    const u64 s_half = 0x0AAAAAAAAAAAAAAAull & Fp::P;
    std::vector<u64> r_halves, s_halves;
    for (auto r : targets) {
        s_halves.push_back(s_half);
        r_halves.push_back(r ^ s_half);
    }

    TwoParty tp;
    tp.fill_pool(zt::kAndGates * targets.size() + 64);

    auto m0 = tp.pool[0].take_mult(zt::Role::Receiver, zt::kAndGates * targets.size());
    auto m1 = tp.pool[1].take_mult(zt::Role::Sender, zt::kAndGates * targets.size());

    std::vector<oc::u8> op0, op1;
    auto t0 = zt::gmw_all_ones(zt::Role::Receiver, r_halves, m0, tp.sock[0], op0);
    auto t1 = zt::gmw_all_ones(zt::Role::Sender, s_halves, m1, tp.sock[1], op1);
    eval(t0, t1);

    ASSERT_EQ(op0.size(), targets.size());
    for (std::size_t i = 0; i < targets.size(); ++i) {
        // Both parties learn the same opened bit...
        EXPECT_EQ(op0[i], op1[i]) << "target " << i;
        // ...and it is the rejection predicate, nothing more.
        EXPECT_EQ(op0[i], targets[i] == Fp::P ? 1 : 0)
            << "target " << i << " = " << targets[i];
    }
}

TEST(ZtGatePipeline, ForcedRejectedMaskIsResampled)
{
    TwoParty tp;
    // Gate 0 is forced to r = 2^61-1 and must be resampled once; budget one
    // extra step-2 round for that resample.
    tp.fill_pool(pool_size(2, 2));

    std::array<zt::PipelineOpts, 2> opts;
    opts[0].count = 2;
    opts[1].count = 2;
    opts[0].forced_mask_halves = {Fp::P};  // r_R = 2^61-1
    opts[1].forced_mask_halves = {0ull};   // r_S = 0   ->  r = 2^61-1
    std::array<oc::PRNG, 2> prng{oc::PRNG(oc::block(5, 0)), oc::PRNG(oc::block(5, 1))};
    std::array<std::vector<zt::ZtGateOut>, 2> out;
    std::array<zt::PipelineStats, 2> stats;
    tp.run(opts, prng, out, stats);

    // The rejection was observed by both parties and cost exactly one extra
    // round; only the forced gate was resampled.
    EXPECT_EQ(stats[0].rejected, 1u);
    EXPECT_EQ(stats[1].rejected, 1u);
    EXPECT_EQ(stats[0].resample_rounds, 1u);
    EXPECT_EQ(stats[1].resample_rounds, 1u);

    // The gate that survived is a fresh mask, not the forced one.
    u64 r0 = out[0][0].mask_half ^ out[1][0].mask_half;
    EXPECT_NE(r0, Fp::P);
    EXPECT_NE(out[0][0].mask_half, Fp::P);

    // ...and it is a well-formed gate: mask share and digits agree with it.
    for (u64 g = 0; g < 2; ++g) {
        u64 r = out[0][g].mask_half ^ out[1][g].mask_half;
        EXPECT_NE(r, Fp::P);
        EXPECT_EQ(out[0][g].mask_share.add(out[1][g].mask_share).v, r);
    }
}

TEST(ZtGatePipeline, SampledMasksAreNeverRejected)
{
    TwoParty tp;
    tp.fill_pool(pool_size(4));

    std::array<zt::PipelineOpts, 2> opts;
    opts[0].count = 4;
    opts[1].count = 4;
    std::array<oc::PRNG, 2> prng{oc::PRNG(oc::block(9, 0)), oc::PRNG(oc::block(9, 1))};
    std::array<std::vector<zt::ZtGateOut>, 2> out;
    std::array<zt::PipelineStats, 2> stats;
    tp.run(opts, prng, out, stats);

    EXPECT_EQ(stats[0].rejected, 0u);
    EXPECT_EQ(stats[0].resample_rounds, 0u);
    // Exactly the advertised OT budget was drawn, no more.
    EXPECT_EQ(stats[0].ots_consumed, pool_size(4));
    EXPECT_EQ(stats[1].ots_consumed, pool_size(4));
}

// ---------------------------------------------------------------------------
// Steps 1-4 — mask, digit and B2A correctness over >= 32 gates
// ---------------------------------------------------------------------------

TEST(ZtGatePipeline, MaskDigitAndB2ACorrectnessOver32Gates)
{
    constexpr u64 kGates = 32;

    TwoParty tp;
    tp.fill_pool(pool_size(kGates));

    std::array<zt::PipelineOpts, 2> opts;
    opts[0].count = kGates;
    opts[1].count = kGates;
    std::array<oc::PRNG, 2> prng{oc::PRNG(oc::block(0, 0)), oc::PRNG(oc::block(0, 1))};
    std::array<std::vector<zt::ZtGateOut>, 2> out;
    std::array<zt::PipelineStats, 2> stats;
    tp.run(opts, prng, out, stats);

    ASSERT_EQ(out[0].size(), kGates);
    ASSERT_EQ(out[1].size(), kGates);

    for (u64 g = 0; g < kGates; ++g) {
        SCOPED_TRACE("gate " + std::to_string(g));

        // Step 1: the logical mask is the integer XOR of the two private halves,
        // and both halves are 61-bit.
        EXPECT_LE(out[0][g].mask_half, Fp::P);
        EXPECT_LE(out[1][g].mask_half, Fp::P);
        u64 r = out[0][g].mask_half ^ out[1][g].mask_half;

        // Step 2: an accepted mask is never the one bad value, so r is a
        // canonical F_p element.
        EXPECT_NE(r, Fp::P);
        EXPECT_LT(r, Fp::P);

        // Step 4: the digit slices XOR-reconstruct r's canonical digits.
        auto expect_digits = zt::digit_split(r);
        for (u64 j = 0; j < zt::kNumDigits; ++j) {
            EXPECT_EQ(out[0][g].digit_shares[j] ^ out[1][g].digit_shares[j], expect_digits[j])
                << "digit " << j;
            EXPECT_LT(out[0][g].digit_shares[j], zt::kDomain) << "digit " << j;
            EXPECT_LT(out[1][g].digit_shares[j], zt::kDomain) << "digit " << j;
        }

        // Step 3: the B2A shares reconstruct m == r mod p, and each share is
        // canonical.
        EXPECT_LT(out[0][g].mask_share.v, Fp::P);
        EXPECT_LT(out[1][g].mask_share.v, Fp::P);
        EXPECT_EQ(out[0][g].mask_share.add(out[1][g].mask_share).v, Fp::from_u64(r).v);
        EXPECT_EQ(Fp::from_u64(r).v, r);  // r < p, so no reduction happened

        EXPECT_EQ(out[0][g].corr_id, g);
        EXPECT_EQ(out[1][g].corr_id, g);
    }

    // The masks are not all the same value (a stuck PRNG would still satisfy
    // every reconstruction check above).
    u64 distinct = 0;
    for (u64 g = 1; g < kGates; ++g)
        distinct += (out[0][g].mask_half != out[0][0].mask_half);
    EXPECT_GE(distinct, kGates - 2);
}

// ---------------------------------------------------------------------------
// ZT-5 [POST-GATE] — DKG at 61 bits
// ---------------------------------------------------------------------------

struct ExpandResult {
    // shares[p][k * kDomain + i], tags[p][k * kDomain + i]
    std::array<oc::AlignedUnVector<Fp>, 2> shares;
    std::array<std::vector<oc::u8>, 2> tags;
    u64 share_digest = 0;
    u64 recon_digest = 0;
    u64 noncanonical = 0;
};

ExpandResult expand_both(zt::ZtGateOut& g0, zt::ZtGateOut& g1)
{
    sympsica::CoeffCtxFp61 ctx;
    ExpandResult e;
    const u64 n = zt::kNumDigits * zt::kDomain;
    for (int p = 0; p < 2; ++p) {
        e.shares[p].resize(n);
        e.tags[p].assign(n, 0);
    }

    auto sink = [&](int p) {
        return [&, p](auto k, auto i, auto&& v, oc::block t) {
            e.shares[p][k * zt::kDomain + i] = v;
            e.tags[p][k * zt::kDomain + i] = t.template get<oc::u8>(0) & 1;
        };
    };
    oc::RegularDpf<Fp, sympsica::CoeffCtxFp61>::expand(0, zt::kDomain, g0.key, sink(0), ctx);
    oc::RegularDpf<Fp, sympsica::CoeffCtxFp61>::expand(1, zt::kDomain, g1.key, sink(1), ctx);

    e.share_digest = sympsica_test::kFnvOffsetBasis;
    e.recon_digest = sympsica_test::kFnvOffsetBasis;
    for (u64 idx = 0; idx < n; ++idx) {
        // Canonicality (task-4-report.md (d)): these values were materialised
        // by RegularDpf::implExpand from the key's mLeafVals through
        // CoeffCtxFp61::deserialize, so they are exactly the ones the guard
        // exists to protect.
        e.noncanonical += (e.shares[0][idx].v >= Fp::P) + (e.shares[1][idx].v >= Fp::P);

        sympsica_test::digest_update(e.share_digest, e.shares[0][idx].v);
        sympsica_test::digest_update(e.share_digest, e.shares[1][idx].v);
        sympsica_test::digest_update(e.share_digest, e.tags[0][idx]);
        sympsica_test::digest_update(e.share_digest, e.tags[1][idx]);

        auto recon = e.shares[0][idx].add(e.shares[1][idx]);
        e.noncanonical += (recon.v >= Fp::P);
        sympsica_test::digest_update(e.recon_digest, recon.v);
        sympsica_test::digest_update(e.recon_digest, e.tags[0][idx] ^ e.tags[1][idx]);
    }
    return e;
}

TEST(ZtGatePipeline, ZT5_DkgAt61Bits)
{
    constexpr u64 kSeeds = 8;      // >= 8 gates, seeds 0..7
    constexpr u64 kProbes = 1000;  // 10^3 random probes per tree

    TwoParty tp;
    tp.fill_pool(pool_size(kSeeds));

    sympsica_test::Fixture fx(sympsica_test::fixture_path("test/fixtures/zt5_dkg61.fixture"));
    const auto tag = build_tag();
    EXPECT_EQ(fx.one("tag")[1], tag)
        << "the pinned digests below were produced on a different build variant; "
           "see test/fixtures/zt5_dkg61.fixture for how to regenerate";
    EXPECT_EQ(fx.u64_at("domain"), zt::kDomain);
    EXPECT_EQ(fx.u64_at("num_points"), zt::kNumDigits);

    for (u64 seed = 0; seed < kSeeds; ++seed) {
        SCOPED_TRACE("seed " + std::to_string(seed));

        std::array<zt::PipelineOpts, 2> opts;
        opts[0].count = 1;
        opts[1].count = 1;
        std::array<oc::PRNG, 2> prng{oc::PRNG(oc::block(seed, 0)), oc::PRNG(oc::block(seed, 1))};
        std::array<std::vector<zt::ZtGateOut>, 2> out;
        std::array<zt::PipelineStats, 2> stats;
        tp.run(opts, prng, out, stats);

        const u64 r = out[0][0].mask_half ^ out[1][0].mask_half;
        const auto digits = zt::digit_split(r);
        auto e = expand_both(out[0][0], out[1][0]);

        // (a) Canonicality across the whole expansion.
        EXPECT_EQ(e.noncanonical, 0u);

        // (b) Full-domain check, counted rather than ASSERTed so the 2^18
        // leaves cost nothing in an unoptimised build.
        u64 bad_value = 0, bad_tag = 0;
        for (u64 k = 0; k < zt::kNumDigits; ++k) {
            for (u64 i = 0; i < zt::kDomain; ++i) {
                const u64 idx = k * zt::kDomain + i;
                const u64 want = (i == digits[k]) ? 1u : 0u;
                bad_value += (e.shares[0][idx].add(e.shares[1][idx]).v != want);
                bad_tag += ((e.tags[0][idx] ^ e.tags[1][idx]) != want);
            }
        }
        EXPECT_EQ(bad_value, 0u);
        EXPECT_EQ(bad_tag, 0u);

        // (c) The plan's assert block, verbatim: 10^3 random probes per tree,
        // reported individually so a failure names the tree and leaf.
        oc::PRNG probe_prng(oc::block(seed, 0xABCDEF));
        for (u64 k = 0; k < zt::kNumDigits; ++k) {
            for (u64 t = 0; t < kProbes; ++t) {
                const u64 i = probe_prng.get<u64>() % zt::kDomain;
                const u64 idx = k * zt::kDomain + i;
                const u64 want = (i == digits[k]) ? 1u : 0u;
                ASSERT_EQ(e.shares[0][idx].add(e.shares[1][idx]).v, want)
                    << "tree " << k << " leaf " << i;
                ASSERT_EQ(u64(e.tags[0][idx] ^ e.tags[1][idx]), want)
                    << "tag: tree " << k << " leaf " << i;
            }
            // The active leaf explicitly — random probes hit it with
            // probability 1000/65536 per tree, so it must be pinned directly.
            const u64 act = k * zt::kDomain + digits[k];
            ASSERT_EQ(e.shares[0][act].add(e.shares[1][act]).v, 1u) << "tree " << k;
            ASSERT_EQ(u64(e.tags[0][act] ^ e.tags[1][act]), 1u) << "tree " << k;
        }

        // (d) Pinned goldens. Printed unconditionally so regenerating the
        // fixture on a new build variant is copy-paste.
        std::printf("zt5 %llu %llu %llu %llu\n", (unsigned long long)seed,
                    (unsigned long long)r, (unsigned long long)e.share_digest,
                    (unsigned long long)e.recon_digest);

        const auto rows = fx.all("zt5");
        ASSERT_GT(rows.size(), seed) << "fixture has no zt5 row for seed " << seed;
        const auto& row = rows[seed];
        ASSERT_EQ(row.size(), 4u);
        EXPECT_EQ(std::stoull(row[0]), seed);
        EXPECT_EQ(std::stoull(row[1]), r) << "mask r drifted for seed " << seed;
        EXPECT_EQ(std::stoull(row[2]), e.share_digest) << "share digest drifted";
        EXPECT_EQ(std::stoull(row[3]), e.recon_digest) << "reconstruction digest drifted";
    }
}

} // namespace

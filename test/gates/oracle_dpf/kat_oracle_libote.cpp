// test/gates/oracle_dpf/kat_oracle_libote.cpp — Task 7 (task-7-brief.md,
// W2.3 Test C, part (a)): the cross-implementation SEMANTIC check for
// sympsica's libOTe-based DPF realization against a PLAINTEXT point-function
// oracle (ref/reference.py's dpf_oracle()). Part (b) — google/
// distributed_point_functions vs the SAME oracle — is a separate driver;
// see task-7-report.md for its build-path evidence.
//
// Test IDs carried in the test names:
//   ZT-4 [CROSS-IMPL-SEMANTIC] — libOTe keys realize the oracle:
//     (1) at the public main payload (1, matching the Task-5 pipeline's
//         hardcoded step-5 value) over the Python-golden points in
//         test/fixtures/zt4_oracle_dpf.fixture, generated through the REAL
//         interactive DKG (test/integration/ztgate_pipeline.hpp);
//     (2) at the near-wrap payloads {p-1, p-2} (the pipeline's payload is
//         fixed at 1, so these drive RegularDpf::keyGen directly, same
//         XOR-shared-point / additive-value-share convention, reusing the
//         pipeline's OtPool/socket/CoeffCtxFp61 plumbing).
//   TV-F2 [FC negative]        — additive sharing over Z_2^64 with each
//     share independently reduced mod p does NOT linearly reconstruct a
//     near-wrap payload (2^64 mod p == 8 != 0, so a u64-arithmetic wrap does
//     not cancel under mod-p reduction the way it would under mod-2^64).
//
// Oracle golden source: ref/reference.py's dpf_oracle(domain, point, payload)
// = payload iff x == point else 0, and digit_split() (mirroring
// ztgate_pipeline.cpp's digit_split() bit-for-bit — cross-checked directly
// against Task 5's pinned ZT-3 vector during this task's development). The
// fixture (test/fixtures/zt4_oracle_dpf.fixture) pins, per seed 0..9, a
// canonical accepted 61-bit mask r drawn from that seed's splitmix64 stream
// and r's four digit-tree points; consumers below FORCE these exact r values
// into the pipeline so the interactively-generated keys' points match the
// fixture bit-for-bit, then expand full-domain and compare every leaf
// against the oracle formula.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "coproto/Socket/LocalAsyncSock.h"
#include "cryptoTools/Crypto/PRNG.h"
#include "libOTe/Dpf/RegularDpf.h"
#include "macoro/sync_wait.h"
#include "macoro/when_all.h"

#include "../../integration/ztgate_pipeline.hpp"
#include "../../utils/fixture_support.hpp"
#include "sympsica/utils/coeff_ctx.hpp"
#include "sympsica/utils/field.hpp"

namespace {

namespace zt = sympsica::ztgate;
namespace oc = osuCrypto;
using sympsica::CoeffCtxFp61;
using sympsica::Fp;
using sympsica::u64;

// Runs both parties' coroutines to completion on this thread — same pattern
// as test/integration/kat_dkg_vole61.cpp's eval(). Kept as a local copy
// rather than a shared header: this task (test/gates/oracle_dpf/*) does not
// own that file.
void eval(macoro::task<>& t0, macoro::task<>& t1)
{
    auto r = macoro::sync_wait(macoro::when_all_ready(std::move(t0), std::move(t1)));
    std::get<0>(r).result();
    std::get<1>(r).result();
}

// A two-party in-process harness: one socket pair, one silent-OT pool per
// party. Same shape as kat_dkg_vole61.cpp's TwoParty; local copy for the
// same reason as eval() above.
struct TwoParty {
    std::array<coproto::LocalAsyncSocket, 2> sock = coproto::LocalAsyncSocket::makePair();
    std::array<zt::OtPool, 2> pool;
    std::array<oc::PRNG, 2> ot_prng{oc::PRNG(oc::block(197, 0)), oc::PRNG(oc::block(197, 1))};

    static constexpr zt::Role role(int i) { return i == 0 ? zt::Role::Receiver : zt::Role::Sender; }

    void fill_pool(u64 ots)
    {
        auto t0 = zt::generate_ot_pool(role(0), ots, ot_prng[0], sock[0], pool[0]);
        auto t1 = zt::generate_ot_pool(role(1), ots, ot_prng[1], sock[1], pool[1]);
        eval(t0, t1);
    }
};

// Full-domain expand of one gate's key pair (all `num_digits` trees).
struct ExpandResult {
    std::array<oc::AlignedUnVector<Fp>, 2> shares;  // [party][tree * kDomain + leaf]
    std::array<std::vector<oc::u8>, 2> tags;
};

ExpandResult expand_both(oc::RegularDpfKey& k0, oc::RegularDpfKey& k1, u64 num_digits)
{
    CoeffCtxFp61 ctx;
    ExpandResult e;
    const u64 n = num_digits * zt::kDomain;
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
    oc::RegularDpf<Fp, CoeffCtxFp61>::expand(0, zt::kDomain, k0, sink(0), ctx);
    oc::RegularDpf<Fp, CoeffCtxFp61>::expand(1, zt::kDomain, k1, sink(1), ctx);
    return e;
}

// The oracle check itself: for every one of `points.size() * kDomain` leaves
// (full domain, not sampled), the reconstructed value and tag must equal
// ref/reference.py's dpf_oracle(kDomain, points[tree], payload) — payload at
// the point, 0 (resp. tag 0) everywhere else, tag 1 at the point.
void assert_matches_oracle(oc::RegularDpfKey& k0, oc::RegularDpfKey& k1,
                           const std::vector<u64>& points, u64 payload,
                           const std::string& scope)
{
    ASSERT_LT(payload, Fp::P) << scope << ": payload is not a canonical Fp value";
    auto e = expand_both(k0, k1, points.size());

    u64 noncanonical = 0, bad_value = 0, bad_tag = 0;
    for (u64 k = 0; k < points.size(); ++k) {
        for (u64 i = 0; i < zt::kDomain; ++i) {
            const u64 idx = k * zt::kDomain + i;
            noncanonical += (e.shares[0][idx].v >= Fp::P) + (e.shares[1][idx].v >= Fp::P);

            const bool active = (i == points[k]);
            const u64 want_value = active ? payload : 0;
            const u64 want_tag = active ? 1u : 0u;

            const auto recon = e.shares[0][idx].add(e.shares[1][idx]);
            bad_value += (recon.v != want_value);
            bad_tag += (u64(e.tags[0][idx] ^ e.tags[1][idx]) != want_tag);
        }
    }
    EXPECT_EQ(noncanonical, 0u) << scope;
    EXPECT_EQ(bad_value, 0u)
        << scope << ": reconstructed value diverged from the plaintext oracle at >= 1 leaf "
                    "(full domain, " << points.size() * zt::kDomain << " leaves checked)";
    EXPECT_EQ(bad_tag, 0u)
        << scope << ": tag diverged from the oracle's active-leaf predicate at >= 1 leaf";
}

// ---------------------------------------------------------------------------
// ZT-4, main payload (1) — real interactive DKG via the Task-5 pipeline,
// forced to the fixture's Python-golden masks.
// ---------------------------------------------------------------------------

TEST(OracleDpf, ZT4_LibOTeKeysMatchPlaintextOracleMainPayload)
{
    sympsica_test::Fixture fx(sympsica_test::fixture_path("test/fixtures/zt4_oracle_dpf.fixture"));

    ASSERT_EQ(fx.u64_at("domain"), zt::kDomain);
    ASSERT_EQ(fx.u64_at("num_digits"), zt::kNumDigits);
    const u64 main_payload = fx.u64_at("main_payload");
    const auto rows = fx.all("oracle_row");
    ASSERT_EQ(rows.size(), fx.u64_at("seed_count"));
    ASSERT_FALSE(rows.empty());

    // Split each fixture's r non-trivially so neither party holds it in the
    // clear — same technique as Task 5's ForcedAcceptedMasksReachOutputUnchanged.
    const u64 s_half = 0x0AAAAAAAAAAAAAAAull & Fp::P;

    std::vector<u64> targets(rows.size());
    std::array<std::vector<u64>, 2> forced;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        ASSERT_EQ(rows[i].size(), 2 + zt::kNumDigits) << "oracle_row " << i;
        EXPECT_EQ(std::stoull(rows[i][0]), i) << "oracle_row " << i << " out of seed order";
        const u64 r = std::stoull(rows[i][1]);
        targets[i] = r;
        forced[0].push_back(r ^ s_half);
        forced[1].push_back(s_half);
    }

    TwoParty tp;
    tp.fill_pool(zt::kOtsPerGate * rows.size());

    std::array<zt::PipelineOpts, 2> opts;
    for (int p = 0; p < 2; ++p) {
        opts[p].count = rows.size();
        opts[p].forced_mask_halves = forced[p];
    }
    std::array<oc::PRNG, 2> prng{oc::PRNG(oc::block(41, 0)), oc::PRNG(oc::block(41, 1))};
    std::array<std::vector<zt::ZtGateOut>, 2> out;
    std::array<zt::PipelineStats, 2> stats;
    {
        auto t0 = zt::generate_ztgates(TwoParty::role(0), tp.sock[0], opts[0], prng[0], tp.pool[0], out[0], stats[0]);
        auto t1 = zt::generate_ztgates(TwoParty::role(1), tp.sock[1], opts[1], prng[1], tp.pool[1], out[1], stats[1]);
        eval(t0, t1);
    }

    for (int p = 0; p < 2; ++p) {
        EXPECT_EQ(stats[p].rejected, 0u) << "party " << p << " (fixture masks are all pre-rejected != p-1)";
        EXPECT_EQ(stats[p].resample_rounds, 0u) << "party " << p;
        EXPECT_EQ(stats[p].ots_consumed, zt::kOtsPerGate * rows.size()) << "party " << p;
    }

    for (std::size_t i = 0; i < rows.size(); ++i) {
        SCOPED_TRACE("seed " + std::to_string(i) + " r=" + std::to_string(targets[i]));

        // The forced mask reached the output unchanged (Task-5 property).
        ASSERT_EQ(out[0][i].mask_half, forced[0][i]);
        ASSERT_EQ(out[1][i].mask_half, forced[1][i]);
        const u64 r = out[0][i].mask_half ^ out[1][i].mask_half;
        ASSERT_EQ(r, targets[i]);

        // digit_shares XOR-reconstruct the SAME points the fixture pins
        // (ztgate_pipeline.cpp's digit_split() vs. reference.py's mirror).
        std::vector<u64> points(zt::kNumDigits);
        for (u64 j = 0; j < zt::kNumDigits; ++j) {
            points[j] = std::stoull(rows[i][2 + j]);
            EXPECT_EQ(out[0][i].digit_shares[j] ^ out[1][i].digit_shares[j], points[j]) << "digit " << j;
        }

        assert_matches_oracle(out[0][i].key, out[1][i].key, points, main_payload,
                              "seed " + std::to_string(i));
    }
}

// ---------------------------------------------------------------------------
// ZT-4, near-wrap payloads {p-1, p-2} — the pipeline's step 5 hardcodes
// payload = 1, so these drive RegularDpf::keyGen directly (task-7-brief.md,
// requirement 1(a)): same XOR-shared-point convention as the pipeline
// (points0 XOR points1 == the plaintext digit point), additive value shares
// summing to the payload (receiver contributes the payload, sender 0 — the
// same split the pipeline uses for its own payload-1 case), reusing the
// pipeline's OtPool / CoeffCtxFp61 / TwoParty socket plumbing rather than
// generate_ztgates() itself. Reuses the SAME public points as the
// main-payload sweep above (task-7-brief.md: "at the same public
// points/payloads"), one fixture row's points per near-wrap payload.
// ---------------------------------------------------------------------------

TEST(OracleDpf, ZT4_LibOTeKeysMatchPlaintextOracleNearWrapPayloads)
{
    sympsica_test::Fixture fx(sympsica_test::fixture_path("test/fixtures/zt4_oracle_dpf.fixture"));
    const auto oracle_rows = fx.all("oracle_row");
    const auto nearwrap_rows = fx.all("nearwrap");
    ASSERT_EQ(nearwrap_rows.size(), fx.u64_at("nearwrap_count"));
    ASSERT_GE(oracle_rows.size(), nearwrap_rows.size());

    for (std::size_t pi = 0; pi < nearwrap_rows.size(); ++pi) {
        ASSERT_EQ(nearwrap_rows[pi].size(), 1u) << "nearwrap row " << pi;
        const u64 payload = std::stoull(nearwrap_rows[pi][0]);
        ASSERT_LT(payload, Fp::P) << "nearwrap payload " << pi;
        SCOPED_TRACE("near-wrap payload " + std::to_string(payload));

        // The plaintext points, straight from the fixture (same public
        // points the main-payload sweep used for this row index).
        std::vector<u64> points(zt::kNumDigits);
        for (u64 j = 0; j < zt::kNumDigits; ++j)
            points[j] = std::stoull(oracle_rows[pi][2 + j]);

        // Non-trivial XOR split of the points (party 1's half is a fixed,
        // non-zero per-digit constant so neither party holds the plaintext
        // point).
        std::vector<u64> points0(zt::kNumDigits), points1(zt::kNumDigits);
        for (u64 j = 0; j < zt::kNumDigits; ++j) {
            points1[j] = (0xAAAAull + j) & (zt::kDomain - 1);
            points0[j] = points[j] ^ points1[j];
        }

        CoeffCtxFp61 ctx;
        auto values0 = ctx.makeVec<Fp>(zt::kNumDigits);
        auto values1 = ctx.makeVec<Fp>(zt::kNumDigits);
        for (u64 j = 0; j < zt::kNumDigits; ++j) {
            values0[j] = Fp(payload);  // receiver's additive share
            values1[j] = Fp(0);        // sender's additive share
        }

        TwoParty tp;
        tp.fill_pool(zt::kOtsDkg);

        std::array<oc::RegularDpf<Fp, CoeffCtxFp61>, 2> dpf;
        dpf[0].init(0, zt::kDomain, zt::kNumDigits, ctx);
        dpf[1].init(1, zt::kDomain, zt::kNumDigits, ctx);
        ASSERT_EQ(dpf[0].baseOtCount(), zt::kOtsDkg);
        {
            auto s0 = tp.pool[0].take(zt::kOtsDkg);
            auto s1 = tp.pool[1].take(zt::kOtsDkg);
            dpf[0].setBaseOts(s0.send, s0.recv, s0.choices);
            dpf[1].setBaseOts(s1.send, s1.recv, s1.choices);
        }

        std::array<oc::RegularDpfKey, 2> keys;
        oc::PRNG prng0(oc::block(71, 2 * pi)), prng1(oc::block(71, 2 * pi + 1));
        {
            auto t0 = dpf[0].keyGen(oc::span<u64>(points0), values0, prng0, keys[0], tp.sock[0], ctx);
            auto t1 = dpf[1].keyGen(oc::span<u64>(points1), values1, prng1, keys[1], tp.sock[1], ctx);
            eval(t0, t1);
        }

        assert_matches_oracle(keys[0], keys[1], points, payload,
                              "near-wrap payload " + std::to_string(payload));
    }
}

// ---------------------------------------------------------------------------
// TV-F2 [FC negative] — a Z_2^64 DPF whose outputs are naively reduced mod p
// independently per share FAILS the oracle on near-wrap payloads
// (task-7-brief.md, requirement 2). No second library needed: this is a
// pure-arithmetic simulation of the failure mode the FC row names.
//
// s_R is a uniform random u64; s_S = payload - s_R computed in UNSIGNED
// u64 arithmetic, so s_R + s_S == payload (mod 2^64) exactly by
// construction, wrapping around 2^64 whenever s_R > payload. Each share is
// then reduced mod p INDEPENDENTLY (Fp::from_u64, the same double-fold used
// throughout this project) and summed mod p. Because p = 2^61-1 and
// 2^61 == 1 (mod p), 2^64 == 8 (mod p) -- NOT 0 -- so a wrap in the u64 sum
// shifts the mod-p reconstruction by +8 rather than cancelling out. With
// payload = p-1 (< 2^61, so p-1 << 2^64), a random s_R exceeds payload with
// overwhelming probability (~1 - 2^-3), so the wrap -- and therefore the
// mismatch -- is expected on almost every seed, not just "possible".
TEST(OracleDpf, TVF2_Z2p64IndependentReductionBreaksNearWrapOracle)
{
    const std::vector<u64> payloads{Fp::P - 1, Fp::P - 2};  // {p-1, p-2}
    constexpr u64 kSeeds = 10;

    for (u64 payload : payloads) {
        SCOPED_TRACE("payload = " + std::to_string(payload));
        u64 mismatches = 0;
        for (u64 seed = 0; seed < kSeeds; ++seed) {
            oc::PRNG prng(oc::block(seed, 0xF2F2F2F2ull));
            const u64 s_R = prng.get<u64>();
            const u64 s_S = payload - s_R;  // unsigned wraparound == mod 2^64

            const Fp r_R = Fp::from_u64(s_R);
            const Fp r_S = Fp::from_u64(s_S);
            const Fp reconstructed = r_R.add(r_S);

            const bool matches_oracle = (reconstructed.v == payload);
            EXPECT_TRUE(matches_oracle || s_R > payload)
                << "seed " << seed << ": mismatch without a u64 wrap should be impossible "
                   "(s_R <= payload means s_R + s_S == payload with no wraparound)";
            if (!matches_oracle) ++mismatches;
        }
        // TV-F2's claim, checked rather than assumed: the oracle mismatch
        // fires (mathematically guaranteed whenever the u64 sum wraps, which
        // for a near-wrap payload is the overwhelmingly likely case).
        ASSERT_GT(mismatches, 0u)
            << "TV-F2: expected at least one seed where naive mod-2^64 sharing + "
               "independent mod-p reduction fails to reconstruct payload " << payload
            << " -- if this never fails, the FC row's claimed failure mode (FT2, additive "
               "shares mod 2^64 do NOT reduce linearly to mod p) is not actually exercised";
    }
}

}  // namespace

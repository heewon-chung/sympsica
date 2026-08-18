// w24_boundary_masks.cpp — W2.4's boundary-mask injection leg (task-8 brief,
// requirement 3): extends Task 5's ForcedAcceptedMasksReachOutputUnchanged
// (kat_dkg_vole61.cpp) from "a forced mask survives to the output" to the
// actual online semantics the gate exists to serve.
//
// The online z-test (out of this task's scope, but simulated here) opens a
// PUBLIC offset D and evaluates the gate at the masked opening
// z = D + r mod p — NOT at r itself. This file forces r in {0, 1, 2^61-2}
// (the same three boundary masks Task 5 injected) through steps 2-5 of the
// pipeline, then for each of D in {0, 1, 2^61-2} computes z = (r + D) mod p,
// digit-splits z, evaluates BOTH parties' DPF shares at z's four digits (via
// the only expansion API steps 5 exposes — see ztgate_pipeline.hpp's note on
// RegularDpf::expand being a full-domain, non-interactive call; there is no
// point-only variant), and asserts the reconstructed per-tree indicator
// equals [z_digit == r_digit] — the masked-opening semantics, including the
// wrap cases where r + D exceeds p and folds back down (e.g. r = 2^61-2,
// D = 1 -> z = 0, digit_split(0) far from digit_split(r)).
//
// In-process (LocalAsyncSocket) per the brief: this leg is about semantic
// coverage of forced_mask_halves, not about real-TCP scale (that is
// w24_pool_gate.cpp's job).

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <vector>

#include "coproto/Socket/LocalAsyncSock.h"
#include "cryptoTools/Crypto/PRNG.h"
#include "macoro/sync_wait.h"
#include "macoro/when_all.h"

#include "sympsica/utils/coeff_ctx.hpp"
#include "sympsica/utils/field.hpp"
#include "ztgate_pipeline.hpp"

namespace {

namespace zt = sympsica::ztgate;
namespace oc = osuCrypto;
using sympsica::Fp;
using sympsica::u64;

// Same single-thread cooperative-scheduling pattern as kat_dkg_vole61.cpp's
// eval() (LocalAsyncSocket requires both sides pumped on one thread); a
// private copy here rather than reaching into that file's anonymous
// namespace (Task 5's ownership; this file owns its own small harness).
void eval(macoro::task<>& t0, macoro::task<>& t1) {
    auto r = macoro::sync_wait(macoro::when_all_ready(std::move(t0), std::move(t1)));
    std::get<0>(r).result();
    std::get<1>(r).result();
}

// Pool size for `gates` gates plus `extra_rejections` forced-resample slack
// (Task 5 minor: OtPool::take has no top-up), mirroring kat_dkg_vole61.cpp's
// pool_size().
u64 pool_size(u64 gates, u64 extra_rejections) {
    return zt::kOtsPerGate * gates + zt::kAndGates * extra_rejections;
}

struct TwoParty {
    std::array<coproto::LocalAsyncSocket, 2> sock = coproto::LocalAsyncSocket::makePair();
    std::array<zt::OtPool, 2> pool;
    std::array<oc::PRNG, 2> ot_prng{oc::PRNG(oc::block(824, 0)), oc::PRNG(oc::block(824, 1))};

    static constexpr zt::Role role(int i) { return i == 0 ? zt::Role::Receiver : zt::Role::Sender; }

    void fill_pool(u64 ots) {
        auto t0 = zt::generate_ot_pool(role(0), ots, ot_prng[0], sock[0], pool[0]);
        auto t1 = zt::generate_ot_pool(role(1), ots, ot_prng[1], sock[1], pool[1]);
        eval(t0, t1);
    }

    void run(const std::array<zt::PipelineOpts, 2>& opts, std::array<oc::PRNG, 2>& prng,
             std::array<std::vector<zt::ZtGateOut>, 2>& out,
             std::array<zt::PipelineStats, 2>& stats) {
        auto t0 = zt::generate_ztgates(role(0), sock[0], opts[0], prng[0], pool[0], out[0], stats[0]);
        auto t1 = zt::generate_ztgates(role(1), sock[1], opts[1], prng[1], pool[1], out[1], stats[1]);
        eval(t0, t1);
    }
};

// Full-domain expand of both parties' keys for one gate. Small local twin of
// kat_dkg_vole61.cpp's expand_both() (that one lives in a different
// translation unit's anonymous namespace) -- this file only needs the raw
// per-leaf (value, tag) arrays, not the digest/canonicality bookkeeping that
// file also computes.
struct ExpandBoth {
    std::array<oc::AlignedUnVector<Fp>, 2> shares;
    std::array<std::vector<oc::u8>, 2> tags;
};

ExpandBoth expand_both(zt::ZtGateOut& g0, zt::ZtGateOut& g1) {
    sympsica::CoeffCtxFp61 ctx;
    ExpandBoth e;
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
    return e;
}

// z = (r + D) mod p, as a plain modular add over canonical u64 operands.
u64 masked_open(u64 r, u64 d) { return Fp::from_u64(r).add(Fp::from_u64(d)).v; }

// Independent ground truth for the 3x3 (r, D) grid below: z = (r+D) mod p and
// the per-tree indicator [z_digit == r_digit], computed by hand (see the
// task-8-report.md derivation) rather than via zt::digit_split/Fp::add, so
// this table can catch a bug in EITHER of those -- an assertion that recomputed
// `want` from the same masked_open()/digit_split() calls it is checking would
// be tautological (verified by mutation: see task-8-report.md).
struct BoundaryRow {
    u64 r;
    u64 d;
    u64 z;                     // pinned (r + d) mod p
    std::array<u64, 4> want;   // pinned [z_digit == r_digit] per tree
};

std::vector<BoundaryRow> boundary_table() {
    const u64 P = Fp::P;
    return {
        {0, 0, 0, {1, 1, 1, 1}},
        {0, 1, 1, {0, 1, 1, 1}},
        {0, P - 1, P - 1, {0, 0, 0, 0}},
        {1, 0, 1, {1, 1, 1, 1}},
        {1, 1, 2, {0, 1, 1, 1}},
        {1, P - 1, 0, {0, 1, 1, 1}},
        {P - 1, 0, P - 1, {1, 1, 1, 1}},
        {P - 1, 1, 0, {0, 0, 0, 0}},
        {P - 1, P - 1, P - 2, {0, 1, 1, 1}},
    };
}

} // namespace

// ---------------------------------------------------------------------------
// W24 boundary-mask injection: gate correctness at D in {0, 1, p-1}
// ---------------------------------------------------------------------------

TEST(W24BoundaryMasks, GateCorrectnessAtMaskedOpeningBoundaries) {
    const std::vector<u64> r_targets{0ull, 1ull, Fp::P - 1};
    const u64 kGates = r_targets.size();

    // Split each forced r non-trivially so neither party holds it in the
    // clear (same convention as Task 5's ForcedAcceptedMasksReachOutputUnchanged).
    const u64 s_half = 0x0AAAAAAAAAAAAAAAull & Fp::P;
    std::array<std::vector<u64>, 2> forced;
    for (auto r : r_targets) {
        forced[0].push_back(r ^ s_half);
        forced[1].push_back(s_half);
    }

    TwoParty tp;
    // A boundary mask r is never itself rejected (0, 1, p-1 are all != p), so
    // no resample is expected here; a small slack budget is kept anyway per
    // the brief's requirement 3 ("pool sizing must include extra_rejections
    // slack") in case a future change to this test injects an r that can be
    // rejected.
    tp.fill_pool(pool_size(kGates, /*extra_rejections=*/2));

    std::array<zt::PipelineOpts, 2> opts;
    for (int p = 0; p < 2; ++p) {
        opts[p].count = kGates;
        opts[p].forced_mask_halves = forced[p];
    }
    std::array<oc::PRNG, 2> prng{oc::PRNG(oc::block(825, 0)), oc::PRNG(oc::block(825, 1))};
    std::array<std::vector<zt::ZtGateOut>, 2> out;
    std::array<zt::PipelineStats, 2> stats;
    tp.run(opts, prng, out, stats);

    for (int p = 0; p < 2; ++p) {
        EXPECT_EQ(stats[p].rejected, 0u);
        EXPECT_EQ(stats[p].resample_rounds, 0u);
    }

    // Per-r expansions, computed once and reused across all three D's for
    // that r.
    std::array<zt::ZtGateOut*, 3> g0{&out[0][0], &out[0][1], &out[0][2]};
    std::array<zt::ZtGateOut*, 3> g1{&out[1][0], &out[1][1], &out[1][2]};
    std::vector<ExpandBoth> expansions;
    for (u64 g = 0; g < kGates; ++g) {
        const u64 r = out[0][g].mask_half ^ out[1][g].mask_half;
        ASSERT_EQ(r, r_targets[g]) << "forced mask did not reach the output unchanged";
        expansions.push_back(expand_both(*g0[g], *g1[g]));
    }

    // Map r -> its gate index, so the pinned table (independent of run order)
    // can be driven directly.
    auto gate_of_r = [&](u64 r) -> u64 {
        for (u64 g = 0; g < kGates; ++g)
            if (r_targets[g] == r) return g;
        ADD_FAILURE() << "no forced gate for r = " << r;
        return 0;
    };

    for (const auto& row : boundary_table()) {
        SCOPED_TRACE("r = " + std::to_string(row.r) + ", D = " + std::to_string(row.d) +
                     ", pinned z = " + std::to_string(row.z));

        // The masked-opening arithmetic itself, checked against the
        // independently pinned z (catches a bug in masked_open()/Fp::add
        // that a self-referential "recompute z, then check against itself"
        // assertion could not).
        const u64 z = masked_open(row.r, row.d);
        ASSERT_EQ(z, row.z) << "(r+D) mod p arithmetic drifted from the pinned value";

        const u64 g = gate_of_r(row.r);
        const auto z_digits = zt::digit_split(z);
        const auto& e = expansions[g];

        for (u64 k = 0; k < zt::kNumDigits; ++k) {
            const u64 idx = k * zt::kDomain + z_digits[k];
            const u64 want = row.want[k];

            const u64 recon_value = e.shares[0][idx].add(e.shares[1][idx]).v;
            const u64 recon_tag = e.tags[0][idx] ^ e.tags[1][idx];

            EXPECT_EQ(recon_value, want)
                << "tree " << k << ": reconstructed DPF value at z's digit ("
                << z_digits[k] << ") should equal the pinned indicator";
            EXPECT_EQ(recon_tag, want) << "tree " << k << ": reconstructed tag mismatch";
            // Canonicality of the two shares feeding the reconstruction.
            EXPECT_LT(e.shares[0][idx].v, Fp::P) << "tree " << k << " party 0 share";
            EXPECT_LT(e.shares[1][idx].v, Fp::P) << "tree " << k << " party 1 share";
        }
    }
}

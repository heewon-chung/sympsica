// test/gates/kat_ztest.cpp — Task 14 (task-14-brief.md, W4.3): the formal
// ZT-1/ZT-2/TV-F3 KAT suite for gates/ztest.hpp (ZeroTest).
//
// Test IDs carried in the test names:
//   ZT-1 [IDENTITY]  — shares of D=0 and D!=0 (250 each, R-ZT1-SCALE):
//                       b reconstructs to 1 resp. 0.
//   ZT-2 [CONCRETE]  — boundary masks: D=0 with r in {0,1,p-1}; D=p-1
//                       with r=1. Correct b; explicit canonical digit
//                       split of the opened z.
//   TV-F3            — non-canonical D/mask (raw value >= p, bypassing
//                       serdes entirely) aborts at ZeroTest's own entry
//                       guard (EXPECT_DEATH, no networking involved).
//
// Fresh ZtGates are generated through the REAL Phase-2 pipeline
// (test/integration/ztgate_pipeline.hpp, LocalAsyncSocket, in-process) and
// converted via ztgate_convert.hpp's to_ztgate() — production ZeroTest
// never sees the pipeline's own ZtGateOut type. Triples for the Beaver
// recombination rounds are DEALER-style (Task-13 precedent, explicitly
// permitted for correctness KATs). ZeroTest::eval itself runs over a real
// localhost TCP Channel pair (kat_minors.cpp's two-thread pattern).

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "coproto/Socket/LocalAsyncSock.h"
#include "cryptoTools/Crypto/PRNG.h"
// macoro/sync_wait.h uses std::source_location/basic_traceable but does not
// include macoro/trace.h itself; other files in this project that drive
// LocalAsyncSocket-based pipeline machinery hit the same gap and supply it
// directly (test/integration/w24_pool_gate.cpp's header comment has the
// full rationale; test/gates/kat_minors.cpp follows the same pattern).
#include "macoro/trace.h"
#include "macoro/sync_wait.h"
#include "macoro/when_all.h"

#include "../integration/ztgate_pipeline.hpp"
#include "sympsica/core/pools.hpp"
#include "sympsica/core/share.hpp"
#include "sympsica/gates/beaver.hpp"
#include "sympsica/gates/ztest.hpp"
#include "sympsica/utils/field.hpp"
#include "sympsica/utils/net.hpp"
#include "ztgate_convert.hpp"

using namespace sympsica;

namespace {

namespace zt = sympsica::ztgate;
namespace oc = osuCrypto;

// ---------------------------------------------------------------------------
// Two-party TCP harness (net_smoke.cpp / kat_minors.cpp pattern): the
// calling thread dials as client (Receiver), a spawned thread listens as
// server (Sender). A fresh port per call avoids TIME_WAIT collisions
// between the many sequential tests in this binary.
// ---------------------------------------------------------------------------

std::atomic<int> g_port_counter{47300};

std::string next_address() {
    return "127.0.0.1:" + std::to_string(g_port_counter.fetch_add(1));
}

std::unique_ptr<Channel> connect_client_with_retry(const std::string& address) {
    std::unique_ptr<Channel> ch;
    for (int attempt = 0; attempt < 200 && !ch; ++attempt) {
        try {
            ch = std::make_unique<Channel>(address, /*is_server=*/false);
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    return ch;
}

template <typename ReceiverFn, typename SenderFn>
void run_two_party(const std::string& address, ReceiverFn&& receiver_fn, SenderFn&& sender_fn) {
    std::thread server([&] {
        Channel ch(address, /*is_server=*/true);
        sender_fn(ch);
    });
    auto client = connect_client_with_retry(address);
    ASSERT_NE(client, nullptr) << "client failed to connect to loopback server at " << address;
    receiver_fn(*client);
    server.join();
}

// ---------------------------------------------------------------------------
// Dealer-style test triples (Task-13 precedent): locally sample (a,b,c=a*b),
// split into additive shares, hand each party its own pre-filled TriplePool.
// ---------------------------------------------------------------------------

struct DealerPools {
    TriplePool party0, party1;
};

DealerPools make_dealer_pools(u64 n, u64 seed) {
    DealerPools out;
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<u64> dist(0, Fp::P - 1);
    std::vector<Triple> t0, t1;
    t0.reserve(n);
    t1.reserve(n);
    for (u64 i = 0; i < n; ++i) {
        Fp a(dist(rng)), b(dist(rng));
        Fp c = a.mul(b);
        Fp a0(dist(rng));
        Fp b0(dist(rng));
        Fp c0(dist(rng));
        t0.push_back(Triple{i, Share{a0}, Share{b0}, Share{c0}});
        t1.push_back(Triple{i, Share{a.sub(a0)}, Share{b.sub(b0)}, Share{c.sub(c0)}});
    }
    out.party0.refill(std::move(t0));
    out.party1.refill(std::move(t1));
    return out;
}

std::pair<std::vector<Share>, std::vector<Share>> split_shares(const std::vector<Fp>& plain,
                                                                 u64 seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<u64> dist(0, Fp::P - 1);
    std::vector<Share> s0(plain.size()), s1(plain.size());
    for (std::size_t i = 0; i < plain.size(); ++i) {
        Fp r(dist(rng));
        s0[i] = Share{r};
        s1[i] = Share{plain[i].sub(r)};
    }
    return {s0, s1};
}

// ---------------------------------------------------------------------------
// Real ZtGate generation via the Phase-2 pipeline (in-process,
// LocalAsyncSocket), converted to production sympsica::ZtGate. `forced[p]`
// (if non-empty) supplies PipelineOpts::forced_mask_halves for party p
// (per-gate forced r halves, W2.4 precedent) — used by ZT-2's boundary
// rows; empty means every gate's mask is freshly sampled (ZT-1).
// ---------------------------------------------------------------------------

u64 zt_pool_size(u64 gates, u64 extra_rejections) {
    return zt::kOtsPerGate * gates + zt::kAndGates * extra_rejections;
}

void eval_local(macoro::task<>& t0, macoro::task<>& t1) {
    auto r = macoro::sync_wait(macoro::when_all_ready(std::move(t0), std::move(t1)));
    std::get<0>(r).result();
    std::get<1>(r).result();
}

std::array<std::vector<ZtGate>, 2> gen_ztgates(
    u64 count, u64 seed, const std::array<std::vector<oc::u64>, 2>& forced = {}) {
    std::array<coproto::LocalAsyncSocket, 2> sock = coproto::LocalAsyncSocket::makePair();
    std::array<zt::OtPool, 2> pool;
    std::array<oc::PRNG, 2> ot_prng{oc::PRNG(oc::block(seed, 100)), oc::PRNG(oc::block(seed, 101))};
    const u64 ots = zt_pool_size(count, /*extra_rejections=*/4);
    {
        auto t0 = zt::generate_ot_pool(zt::Role::Receiver, ots, ot_prng[0], sock[0], pool[0]);
        auto t1 = zt::generate_ot_pool(zt::Role::Sender, ots, ot_prng[1], sock[1], pool[1]);
        eval_local(t0, t1);
    }

    std::array<zt::PipelineOpts, 2> opts;
    for (int p = 0; p < 2; ++p) {
        opts[p].count = count;
        if (!forced[p].empty()) opts[p].forced_mask_halves = forced[p];
    }
    std::array<oc::PRNG, 2> gate_prng{oc::PRNG(oc::block(seed, 200)), oc::PRNG(oc::block(seed, 201))};
    std::array<std::vector<zt::ZtGateOut>, 2> out;
    std::array<zt::PipelineStats, 2> stats;
    {
        auto t0 = zt::generate_ztgates(zt::Role::Receiver, sock[0], opts[0], gate_prng[0], pool[0],
                                        out[0], stats[0]);
        auto t1 = zt::generate_ztgates(zt::Role::Sender, sock[1], opts[1], gate_prng[1], pool[1],
                                        out[1], stats[1]);
        eval_local(t0, t1);
    }

    std::array<std::vector<ZtGate>, 2> converted;
    for (int p = 0; p < 2; ++p) {
        converted[p].reserve(count);
        for (auto& g : out[p]) converted[p].push_back(sympsica_test::to_ztgate(g));
    }
    return converted;
}

} // namespace

// ---------------------------------------------------------------------------
// ZT-1 [IDENTITY] — shares of D=0 and D!=0 (250 each, R-ZT1-SCALE: reduced
// from 10^3 per the ruling's explicit allowance -- the identity is
// scale-invariant, the row's power is fresh-gate diversity, not count):
// b reconstructs to 1 resp. 0.
// ---------------------------------------------------------------------------

TEST(GatesZtest, ZT1_IdentityDZeroVsDNonzeroReconstructsCorrectly) {
    constexpr u64 kEach = 250;
    constexpr u64 kTotal = 2 * kEach;

    auto gates = gen_ztgates(kTotal, 0x2A0100ull);

    std::mt19937_64 rng(0x2A0200ull);
    std::uniform_int_distribution<u64> dist_nz(1, Fp::P - 1);
    std::vector<Fp> D_plain(kTotal);
    for (u64 i = 0; i < kEach; ++i) D_plain[i] = Fp(0);
    for (u64 i = kEach; i < kTotal; ++i) D_plain[i] = Fp(dist_nz(rng));
    auto [D0, D1] = split_shares(D_plain, 0x2A0300ull);

    auto pools = make_dealer_pools(kTotal * 3, 0x2A0400ull);  // 3 triples/eval() call

    std::vector<Fp> b0(kTotal), b1(kTotal);
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            BeaverEngine eng(Role::Receiver);
            for (u64 i = 0; i < kTotal; ++i)
                b0[i] = ZeroTest::eval(D0[i], gates[0][i], eng, pools.party0, ch).v;
        },
        [&](Channel& ch) {
            BeaverEngine eng(Role::Sender);
            for (u64 i = 0; i < kTotal; ++i)
                b1[i] = ZeroTest::eval(D1[i], gates[1][i], eng, pools.party1, ch).v;
        });

    u64 bad = 0;
    for (u64 i = 0; i < kTotal; ++i) {
        Fp b = b0[i].add(b1[i]);
        const u64 want = (i < kEach) ? 1u : 0u;
        if (b.v != want) {
            ++bad;
            ADD_FAILURE() << "gate " << i << " (D=" << (i < kEach ? "0" : "!=0")
                           << "): b=" << b.v << " want " << want;
        }
    }
    EXPECT_EQ(bad, 0u) << "at least one of " << kTotal << " ZeroTest::eval calls diverged";
}

// ---------------------------------------------------------------------------
// ZT-2 [CONCRETE] — boundary masks: D=0 with r in {0, 1, p-1}; D=p-1 with
// r=1 (exercises the wrap z = D+r = 0 mod p). Correct b AND explicit
// canonical digit split of the opened z in [0,p).
// ---------------------------------------------------------------------------

TEST(GatesZtest, ZT2_BoundaryMasksCorrectBAndCanonicalDigitSplit) {
    struct Row {
        Fp D;
        u64 r;
        u64 want_b;
    };
    const std::vector<Row> rows{
        {Fp(0), 0ull, 1u},
        {Fp(0), 1ull, 1u},
        {Fp(0), Fp::P - 1, 1u},
        {Fp(Fp::P - 1), 1ull, 0u},
    };
    const u64 kRows = rows.size();

    // Split each forced r non-trivially so neither party holds it in the
    // clear (same convention as w24_boundary_masks.cpp).
    const u64 s_half = 0x0AAAAAAAAAAAAAAAull & Fp::P;
    std::array<std::vector<oc::u64>, 2> forced;
    for (const auto& row : rows) {
        forced[0].push_back(row.r ^ s_half);
        forced[1].push_back(s_half);
    }
    auto gates = gen_ztgates(kRows, 0x2B0100ull, forced);

    std::vector<Fp> D_plain(kRows);
    for (u64 i = 0; i < kRows; ++i) D_plain[i] = rows[i].D;
    auto [D0, D1] = split_shares(D_plain, 0x2B0200ull);

    auto pools = make_dealer_pools(kRows * 3, 0x2B0300ull);

    std::vector<Fp> z0(kRows), z1(kRows), b0(kRows), b1(kRows);
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            BeaverEngine eng(Role::Receiver);
            for (u64 i = 0; i < kRows; ++i) {
                std::array<Share, 1> Ds{D0[i]};
                std::array<ZtGate, 1> gs{gates[0][i]};
                auto z = ZeroTest::open_masked(Ds, gs, ch);
                z0[i] = z[0];
                std::array<std::array<Share, 4>, 1> rhos{
                    ZeroTest::eval_local_rhos(z[0], gates[0][i], eng.role())};
                auto b = ZeroTest::recombine(rhos, eng, pools.party0, ch);
                b0[i] = b[0].v;
            }
        },
        [&](Channel& ch) {
            BeaverEngine eng(Role::Sender);
            for (u64 i = 0; i < kRows; ++i) {
                std::array<Share, 1> Ds{D1[i]};
                std::array<ZtGate, 1> gs{gates[1][i]};
                auto z = ZeroTest::open_masked(Ds, gs, ch);
                z1[i] = z[0];
                std::array<std::array<Share, 4>, 1> rhos{
                    ZeroTest::eval_local_rhos(z[0], gates[1][i], eng.role())};
                auto b = ZeroTest::recombine(rhos, eng, pools.party1, ch);
                b1[i] = b[0].v;
            }
        });

    for (u64 i = 0; i < kRows; ++i) {
        SCOPED_TRACE("row " + std::to_string(i) + " D=" + std::to_string(rows[i].D.v) +
                     " r=" + std::to_string(rows[i].r));

        ASSERT_EQ(z0[i].v, z1[i].v) << "both parties must open the SAME z";
        const Fp want_z = rows[i].D.add(Fp(rows[i].r));
        EXPECT_EQ(z0[i].v, want_z.v) << "z = D + r must match the pinned masked-opening value";
        EXPECT_LT(z0[i].v, Fp::P) << "opened z must be canonical";

        // Explicit canonical digit split of z (requirement 2's "second
        // clause"): each digit within its bit width, and the weighted-sum
        // reconstruction of the four digits equals z exactly.
        const auto digits = ZeroTest::digit_split(z0[i]);
        EXPECT_LT(digits[0], u64(1) << 16);
        EXPECT_LT(digits[1], u64(1) << 16);
        EXPECT_LT(digits[2], u64(1) << 16);
        EXPECT_LT(digits[3], u64(1) << 13);
        const u64 recon = digits[0] | (digits[1] << 16) | (digits[2] << 32) | (digits[3] << 48);
        EXPECT_EQ(recon, z0[i].v) << "digit split must reconstruct z exactly";

        const Fp b = Fp(b0[i]).add(Fp(b1[i]));
        EXPECT_EQ(b.v, rows[i].want_b) << "reconstructed b mismatch";
    }
}

// ---------------------------------------------------------------------------
// TV-F3 — non-canonical D or mask share (raw value >= p, constructed
// directly via struct init, bypassing serdes entirely -- read_fp aborts on
// >= p at the wire layer, so this case cannot arrive any other way) aborts
// at ZeroTest's own entry guard (R-TVF3's FT3-in-the-gate posture). Pure
// local call -- no Channel, no networking, so no fork-after-threads hazard
// with EXPECT_DEATH's default (fork-based) death test style.
// ---------------------------------------------------------------------------

TEST(GatesZtest, TVF3_NonCanonicalInputAbortsAtGateEntry) {
    // This binary's OTHER tests (ZT-1/ZT-2 above, kat_minors.cpp) open real
    // coproto/asio Channels whose background threads can still be alive
    // when this test runs (gtest does not tear process state down between
    // TESTs in one binary) -- gtest's default "fast" death-test style
    // forks the live process, which is the exact fork-after-threads hazard
    // test/integration/w24_pool_gate.cpp's header comment warns about.
    // "threadsafe" re-execs a fresh copy of the binary for the death
    // statement instead of forking, sidestepping that hazard entirely.
    ::testing::GTEST_FLAG(death_test_style) = "threadsafe";

    const Fp bad{Fp::P};
    const Fp ok{0};

    ZtGate g_ok_mask{};
    g_ok_mask.corr_id = 0;
    g_ok_mask.mask_m = Share{ok};
    EXPECT_DEATH(ZeroTest::check_canonical_entry(Share{bad}, g_ok_mask), "")
        << "non-canonical D share must abort at entry";

    ZtGate g_bad_mask{};
    g_bad_mask.corr_id = 0;
    g_bad_mask.mask_m = Share{bad};
    EXPECT_DEATH(ZeroTest::check_canonical_entry(Share{ok}, g_bad_mask), "")
        << "non-canonical gate mask share must abort at entry";
}

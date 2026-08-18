// test/protocols/kat_setup.cpp — Task 16's formal SC/FC suite for
// protocols/setup.hpp (task-16-brief.md, W5.1): productionized correlation
// provisioning (Setup::run / Setup::refill_offline) over the W2.1 (ztgate)
// and W2.2 (vole/Beaver) pipelines, now hosted natively in
// include/sympsica/protocols/detail/{ztgate_pipeline,vole_beaver,setup_ot}.hpp
// (R-HOST consolidation).
//
// Test IDs carried in the test names:
//   SETUP-1  [SC1] — Setup::run fills both pools to exactly PoolSizes;
//                     corr_ids globally unique; both parties agree on the
//                     corr_id sequence.
//   SETUP-2  [SC2] — >= 16 Setup-produced triples: c == a*b mod p.
//   SETUP-3  [SC3] — >= 4 Setup-produced ZtGates: full-domain DPF
//                     reconstruction hits payload 1 exactly at the mask's
//                     four digit points.
//   SETUP-4  [SC4] — refill_offline restores remaining() to target; new
//                     corr_ids continue the global sequence; consumed ids
//                     never reappear; cross-refill uniqueness holds.
//   SETUP-5  [SC5, CLM-B-lite] — PkOpCounter > 0 after Setup::run; EXACTLY
//                     CONSTANT across >= 3 refill_offline calls and pool
//                     consumption.
//   SETUP-6  [SC6] — R-METER offline byte counts (Setup::run and one
//                     refill_offline), recorded via coproto::Socket's own
//                     counters and printed for task-16-report.md.
//   TVF10_ProductionLeg      [FC1] — take_by_id() twice on a
//                     Setup-produced corr_id aborts.
//   CrossRefillDuplicate     [FC2] — refilling with a hand-constructed item
//                     whose corr_id was already produced by Setup aborts.
//   CLMBNonVacuity           [FC3] — a direct second call to the internal
//                     base-OT entry point increases PkOpCounter (proves
//                     SETUP-5's constancy assertion is not vacuously true).
//   MaskCanonicalityGuard    [FC4] — a forced 61-bit mask == 2^61-1, fed via
//                     the production PipelineOpts::forced_mask_halves
//                     test-support knob, is rejected and resampled (Phase-2
//                     semantics), never silently accepted.
//
// Harness: SC1-6/FC1-3 use a real localhost TCP Channel pair on two OS
// threads (kat_minors.cpp / kat_ztest.cpp pattern) since Setup::run drives
// ch.socket() directly. FC4 uses the in-process LocalAsyncSocket harness
// (w24_boundary_masks.cpp's TwoParty pattern) since it calls
// ztgate::generate_ztgates directly, matching that file's own precedent for
// forced-mask injection tests.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "coproto/Socket/LocalAsyncSock.h"
#include "cryptoTools/Crypto/PRNG.h"
// macoro/sync_wait.h uses std::source_location/basic_traceable but does not
// include macoro/trace.h itself; every other file in this project that
// drives a real coproto socket (asio or LocalAsyncSocket) through
// macoro::sync_wait hits the same gap and supplies it directly --
// test/integration/w24_pool_gate.cpp's header comment has the full
// rationale.
#include "macoro/trace.h"
#include "macoro/sync_wait.h"
#include "macoro/when_all.h"

#include "sympsica/core/pools.hpp"
#include "sympsica/core/share.hpp"
#include "sympsica/protocols/detail/setup_ot.hpp"
#include "sympsica/protocols/detail/ztgate_pipeline.hpp"
#include "sympsica/protocols/setup.hpp"
#include "sympsica/utils/coeff_ctx.hpp"
#include "sympsica/utils/field.hpp"
#include "sympsica/utils/net.hpp"
#include "sympsica/utils/params.hpp"

using namespace sympsica;

namespace {

namespace zt = sympsica::ztgate;
namespace oc = osuCrypto;

// ---------------------------------------------------------------------------
// Two-party TCP harness (kat_minors.cpp / kat_ztest.cpp pattern): the
// calling thread dials as client (Receiver), a spawned thread listens as
// server (Sender). A fresh port per call avoids TIME_WAIT collisions
// between the many sequential tests in this binary; base 49100 stays clear
// of every other test binary's port range (net_smoke 41231, w24_pool_gate
// 45231, gates' kat_minors/kat_ztest/kat_symdiff 47100-47499,
// net_exchange 48100+) so this binary can run concurrently with them under
// `ctest -j`.
// ---------------------------------------------------------------------------

std::atomic<int> g_port_counter{49100};

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

// Small PoolSizes throughout (R-SCALE: bench-scale 90112/8192 must never run
// in tests).
constexpr PoolSizes kSmallSizes{/*triples=*/64, /*gates=*/16};

// Runs Setup::run on both parties over one fresh TCP pair, returning both
// parties' resulting Pools.
std::pair<Pools, Pools> run_setup_both(const Params& params, PoolSizes sizes) {
    Pools pool_r, pool_s;
    run_two_party(
        next_address(),
        [&](Channel& ch) { pool_r = Setup::run(Role::Receiver, ch, params, sizes); },
        [&](Channel& ch) { pool_s = Setup::run(Role::Sender, ch, params, sizes); });
    return {std::move(pool_r), std::move(pool_s)};
}

// ---------------------------------------------------------------------------
// FC4's in-process LocalAsyncSocket harness (w24_boundary_masks.cpp's
// TwoParty pattern, reused directly for the forced-mask injection leg).
// ---------------------------------------------------------------------------

void eval(macoro::task<>& t0, macoro::task<>& t1) {
    auto r = macoro::sync_wait(macoro::when_all_ready(std::move(t0), std::move(t1)));
    std::get<0>(r).result();
    std::get<1>(r).result();
}

u64 pool_size_with_slack(u64 gates, u64 extra_rejections) {
    return zt::kOtsPerGate * gates + zt::kAndGates * extra_rejections;
}

} // namespace

// ---------------------------------------------------------------------------
// SETUP-1 [SC1]: fills both pools to exactly PoolSizes; corr_ids globally
// unique; both parties agree on the corr_id sequence.
// ---------------------------------------------------------------------------

TEST(Setup, SETUP1_FillsExactlyPoolSizesAndPartiesAgreeOnCorrIdSequence) {
    Params params = Params::instantiate();
    auto [pool_r, pool_s] = run_setup_both(params, kSmallSizes);

    EXPECT_EQ(pool_r.triples.remaining(), kSmallSizes.triples);
    EXPECT_EQ(pool_s.triples.remaining(), kSmallSizes.triples);
    EXPECT_EQ(pool_r.gates.remaining(), kSmallSizes.gates);
    EXPECT_EQ(pool_s.gates.remaining(), kSmallSizes.gates);

    // Drain both parties' triple pools in FIFO order, pairing corr_ids and
    // checking global uniqueness on each side.
    std::set<u64> seen_r, seen_s;
    for (std::size_t i = 0; i < kSmallSizes.triples; ++i) {
        Triple t_r = pool_r.triples.take();
        Triple t_s = pool_s.triples.take();
        EXPECT_EQ(t_r.corr_id, t_s.corr_id) << "triple " << i << ": parties disagree on corr_id";
        EXPECT_TRUE(seen_r.insert(t_r.corr_id).second) << "party R duplicate corr_id";
        EXPECT_TRUE(seen_s.insert(t_s.corr_id).second) << "party S duplicate corr_id";
    }
    EXPECT_EQ(seen_r.size(), kSmallSizes.triples);

    std::set<u64> seen_gates_r, seen_gates_s;
    for (std::size_t i = 0; i < kSmallSizes.gates; ++i) {
        ZtGate g_r = pool_r.gates.take();
        ZtGate g_s = pool_s.gates.take();
        EXPECT_EQ(g_r.corr_id, g_s.corr_id) << "gate " << i << ": parties disagree on corr_id";
        EXPECT_TRUE(seen_gates_r.insert(g_r.corr_id).second) << "party R duplicate corr_id";
        EXPECT_TRUE(seen_gates_s.insert(g_s.corr_id).second) << "party S duplicate corr_id";
    }
    EXPECT_EQ(seen_gates_r.size(), kSmallSizes.gates);

    // The two id spaces (triples vs. gates) are independent pools -- no
    // cross-type uniqueness is claimed or required (core/pools.hpp: separate
    // CorrelationPool<Triple>/CorrelationPool<ZtGate> instantiations).
}

// ---------------------------------------------------------------------------
// SETUP-2 [SC2]: >= 16 Setup-produced triples reconstruct c == a*b mod p.
// ---------------------------------------------------------------------------

TEST(Setup, SETUP2_ProductCorrectnessAcrossBothParties) {
    Params params = Params::instantiate();
    auto [pool_r, pool_s] = run_setup_both(params, kSmallSizes);

    constexpr std::size_t kSamples = 20; // >= 16 per SC2
    ASSERT_GE(kSmallSizes.triples, kSamples);
    for (std::size_t i = 0; i < kSamples; ++i) {
        Triple t_r = pool_r.triples.take();
        Triple t_s = pool_s.triples.take();
        ASSERT_EQ(t_r.corr_id, t_s.corr_id) << "sample " << i;

        Fp a = t_r.a.v.add(t_s.a.v);
        Fp b = t_r.b.v.add(t_s.b.v);
        Fp c = t_r.c.v.add(t_s.c.v);
        EXPECT_EQ(c.v, a.mul(b).v) << "sample " << i << " (corr_id " << t_r.corr_id << ")";
    }
}

// ---------------------------------------------------------------------------
// SETUP-3 [SC3]: >= 4 Setup-produced ZtGates: reconstruct the mask, expand
// both parties' DPF keys over the full domain, and confirm the reconstructed
// leaf-share sum equals the payload-1 indicator [leaf == mask_digit] at
// every one of the four trees' 2^16 leaves (not just the target digit) --
// the Phase-2 ZT5 verification leg, now against PRODUCTION gates.
// ---------------------------------------------------------------------------

TEST(Setup, SETUP3_GateCorrectnessAgainstProductionGates) {
    Params params = Params::instantiate();
    auto [pool_r, pool_s] = run_setup_both(params, kSmallSizes);

    constexpr std::size_t kSamples = 4; // >= 4 per SC3
    ASSERT_GE(kSmallSizes.gates, kSamples);

    sympsica::CoeffCtxFp61 ctx;
    for (std::size_t i = 0; i < kSamples; ++i) {
        ZtGate g_r = pool_r.gates.take();
        ZtGate g_s = pool_s.gates.take();
        ASSERT_EQ(g_r.corr_id, g_s.corr_id) << "sample " << i;

        Fp m = g_r.mask_m.v.add(g_s.mask_m.v);
        auto want_digits = zt::digit_split(m.v);

        const u64 n = zt::kNumDigits * zt::kDomain;
        std::array<oc::AlignedUnVector<Fp>, 2> shares;
        std::array<std::vector<oc::u8>, 2> tags;
        shares[0].resize(n);
        shares[1].resize(n);
        tags[0].assign(n, 0);
        tags[1].assign(n, 0);
        auto sink = [&](int p) {
            return [&, p](auto k, auto idx, auto&& v, oc::block t) {
                shares[p][k * zt::kDomain + idx] = v;
                tags[p][k * zt::kDomain + idx] = t.template get<oc::u8>(0) & 1;
            };
        };
        oc::RegularDpf<Fp, sympsica::CoeffCtxFp61>::expand(0, zt::kDomain, g_r.key, sink(0), ctx);
        oc::RegularDpf<Fp, sympsica::CoeffCtxFp61>::expand(1, zt::kDomain, g_s.key, sink(1), ctx);

        u64 bad_value = 0, bad_tag = 0;
        for (u64 k = 0; k < zt::kNumDigits; ++k) {
            for (u64 leaf = 0; leaf < zt::kDomain; ++leaf) {
                const u64 idx = k * zt::kDomain + leaf;
                const u64 want = (leaf == want_digits[k]) ? 1u : 0u;
                const u64 recon_value = shares[0][idx].add(shares[1][idx]).v;
                const u64 recon_tag = tags[0][idx] ^ tags[1][idx];
                bad_value += (recon_value != want);
                bad_tag += (recon_tag != want);
            }
        }
        EXPECT_EQ(bad_value, 0u) << "sample " << i << " (corr_id " << g_r.corr_id
                                  << "): point-function reconstruction, full domain";
        EXPECT_EQ(bad_tag, 0u) << "sample " << i << ": tag reconstruction, full domain";
    }
}

// ---------------------------------------------------------------------------
// SETUP-4 [SC4]: refill_offline restores remaining() to target; new corr_ids
// continue the global sequence; consumed ids never reappear; cross-refill
// uniqueness holds (pool's own validation passes, i.e. no abort).
// ---------------------------------------------------------------------------

TEST(Setup, SETUP4_RefillRestoresTargetAndContinuesCorrIdSequence) {
    Params params = Params::instantiate();
    const PoolSizes initial{/*triples=*/8, /*gates=*/4};
    const PoolSizes target{/*triples=*/24, /*gates=*/10};

    Pools pool_r, pool_s;
    run_two_party(
        next_address(),
        [&](Channel& ch) { pool_r = Setup::run(Role::Receiver, ch, params, initial); },
        [&](Channel& ch) { pool_s = Setup::run(Role::Sender, ch, params, initial); });

    // Consume part of both pools before refilling (SC4: "after consuming
    // part of the pool").
    std::set<u64> consumed_triple_ids, consumed_gate_ids;
    for (int i = 0; i < 3; ++i) {
        Triple t = pool_r.triples.take();
        consumed_triple_ids.insert(t.corr_id);
        (void)pool_s.triples.take();
    }
    for (int i = 0; i < 2; ++i) {
        ZtGate g = pool_r.gates.take();
        consumed_gate_ids.insert(g.corr_id);
        (void)pool_s.gates.take();
    }
    const u64 next_corr_id_before = pool_r.next_corr_id;
    ASSERT_EQ(pool_r.next_corr_id, pool_s.next_corr_id)
        << "both parties must have generated the same number of correlations from run()";

    run_two_party(
        next_address(),
        [&](Channel& ch) { Setup::refill_offline(pool_r, Role::Receiver, ch, params, target); },
        [&](Channel& ch) { Setup::refill_offline(pool_s, Role::Sender, ch, params, target); });

    EXPECT_EQ(pool_r.triples.remaining(), target.triples);
    EXPECT_EQ(pool_s.triples.remaining(), target.triples);
    EXPECT_EQ(pool_r.gates.remaining(), target.gates);
    EXPECT_EQ(pool_s.gates.remaining(), target.gates);

    // New corr_ids continue the global sequence (R-CORRID): every id
    // generated after the refill is >= the pre-refill counter, and none of
    // them collides with an id already consumed before the refill.
    EXPECT_GT(pool_r.next_corr_id, next_corr_id_before);
    while (pool_r.triples.remaining() > 0) {
        Triple t = pool_r.triples.take();
        EXPECT_FALSE(consumed_triple_ids.count(t.corr_id))
            << "a consumed corr_id reappeared after refill: " << t.corr_id;
    }
    while (pool_r.gates.remaining() > 0) {
        ZtGate g = pool_r.gates.take();
        EXPECT_FALSE(consumed_gate_ids.count(g.corr_id))
            << "a consumed corr_id reappeared after refill: " << g.corr_id;
    }
    // Cross-refill uniqueness holds by construction: the refill() calls
    // above inside Setup::refill_offline already ran CorrelationPool's own
    // validation (pools.hpp/pools.cpp) without aborting -- had a duplicate
    // corr_id been produced, this test would have died before reaching
    // this point (FC2 exercises that abort path directly, on purpose).
}

// ---------------------------------------------------------------------------
// SETUP-5 [SC5, CLM-B-lite]: PkOpCounter > 0 after Setup::run; EXACTLY
// CONSTANT across >= 3 refill_offline calls and across pool
// consumption/take operations.
// ---------------------------------------------------------------------------

TEST(Setup, SETUP5_PkOpCounterConstantAcrossRefillsAndConsumption) {
    Params params = Params::instantiate();
    const PoolSizes initial{/*triples=*/4, /*gates=*/2};

    Pools pool_r, pool_s;
    run_two_party(
        next_address(),
        [&](Channel& ch) { pool_r = Setup::run(Role::Receiver, ch, params, initial); },
        [&](Channel& ch) { pool_s = Setup::run(Role::Sender, ch, params, initial); });

    const u64 after_setup = PkOpCounter::value();
    EXPECT_GT(after_setup, 0u) << "Setup::run must run at least one base-OT operation";

    // Pool consumption must not touch the counter.
    (void)pool_r.triples.take();
    (void)pool_s.triples.take();
    EXPECT_EQ(PkOpCounter::value(), after_setup);

    // >= 3 refill_offline calls, each growing the target a bit further.
    PoolSizes target{/*triples=*/initial.triples, /*gates=*/initial.gates};
    for (int round = 0; round < 3; ++round) {
        target.triples += 4;
        target.gates += 2;
        run_two_party(
            next_address(),
            [&](Channel& ch) { Setup::refill_offline(pool_r, Role::Receiver, ch, params, target); },
            [&](Channel& ch) { Setup::refill_offline(pool_s, Role::Sender, ch, params, target); });
        EXPECT_EQ(PkOpCounter::value(), after_setup) << "refill round " << round;
    }
}

// ---------------------------------------------------------------------------
// SETUP-6 [SC6, R-METER]: offline byte counts for Setup::run and one
// refill_offline call, read from coproto::Socket's own counters -- Setup's
// traffic flows over Channel::socket() directly (same as the Phase-2
// pipelines it productionizes), so Channel::bytes_sent() does NOT observe
// it (net.hpp's documented "known meter bypass"). SPLIT: this measurement
// is "offline" traffic (Setup/refill provisioning); "online" traffic is
// whatever a later query round sends through Channel::send()/recv(),
// counted by Channel::bytes_sent() -- a DIFFERENT counter, on purpose,
// per R-METER's schema-scope requirement. Numbers are printed for
// task-16-report.md; this test's pass/fail is a sanity bound only.
// ---------------------------------------------------------------------------

TEST(Setup, SETUP6_OfflineByteCountsViaCoprotoSocketCounters) {
    Params params = Params::instantiate();

    u64 setup_bytes_r = 0, setup_bytes_s = 0;
    Pools pool_r, pool_s;
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            const u64 before = static_cast<u64>(ch.socket().bytesSent());
            pool_r = Setup::run(Role::Receiver, ch, params, kSmallSizes);
            setup_bytes_r = static_cast<u64>(ch.socket().bytesSent()) - before;
        },
        [&](Channel& ch) {
            const u64 before = static_cast<u64>(ch.socket().bytesSent());
            pool_s = Setup::run(Role::Sender, ch, params, kSmallSizes);
            setup_bytes_s = static_cast<u64>(ch.socket().bytesSent()) - before;
        });

    PoolSizes target{kSmallSizes.triples + 16, kSmallSizes.gates + 4};
    u64 refill_bytes_r = 0, refill_bytes_s = 0;
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            const u64 before = static_cast<u64>(ch.socket().bytesSent());
            Setup::refill_offline(pool_r, Role::Receiver, ch, params, target);
            refill_bytes_r = static_cast<u64>(ch.socket().bytesSent()) - before;
        },
        [&](Channel& ch) {
            const u64 before = static_cast<u64>(ch.socket().bytesSent());
            Setup::refill_offline(pool_s, Role::Sender, ch, params, target);
            refill_bytes_s = static_cast<u64>(ch.socket().bytesSent()) - before;
        });

    std::printf(
        "{\"setup6_offline_bytes\":{\"setup_r\":%llu,\"setup_s\":%llu,"
        "\"refill_r\":%llu,\"refill_s\":%llu,\"pool_sizes\":{\"triples\":%zu,\"gates\":%zu},"
        "\"refill_target\":{\"triples\":%zu,\"gates\":%zu}}}\n",
        (unsigned long long)setup_bytes_r, (unsigned long long)setup_bytes_s,
        (unsigned long long)refill_bytes_r, (unsigned long long)refill_bytes_s,
        kSmallSizes.triples, kSmallSizes.gates, target.triples, target.gates);
    std::fflush(stdout);

    EXPECT_GT(setup_bytes_r, 0u);
    EXPECT_GT(setup_bytes_s, 0u);
    EXPECT_GT(refill_bytes_r, 0u);
    EXPECT_GT(refill_bytes_s, 0u);
}

// ---------------------------------------------------------------------------
// FC1 [TV-F10 production leg]: take_by_id() twice on a Setup-produced
// corr_id aborts.
// ---------------------------------------------------------------------------

TEST(SetupDeathTest, TVF10_ProductionLeg_DoubleTakeByIdOnSetupCorrIdAborts) {
    Params params = Params::instantiate();
    auto [pool_r, pool_s] = run_setup_both(params, kSmallSizes);
    (void)pool_s;

    Triple t = pool_r.triples.take();
    EXPECT_DEATH({ pool_r.triples.take_by_id(t.corr_id); }, "already consumed");
}

// ---------------------------------------------------------------------------
// FC2 [cross-refill dup]: refilling with a hand-constructed item whose
// corr_id was already produced by Setup aborts.
// ---------------------------------------------------------------------------

TEST(SetupDeathTest, CrossRefillDuplicate_HandConstructedItemReusingSetupCorrIdAborts) {
    Params params = Params::instantiate();
    auto [pool_r, pool_s] = run_setup_both(params, kSmallSizes);
    (void)pool_s;

    Triple t = pool_r.triples.take(); // consume corr_id t.corr_id
    std::vector<Triple> forged;
    forged.push_back(Triple{t.corr_id, Share{Fp(1)}, Share{Fp(1)}, Share{Fp(1)}});
    EXPECT_DEATH({ pool_r.triples.refill(std::move(forged)); }, "duplicate corr_id");
}

// ---------------------------------------------------------------------------
// FC3 [CLM-B non-vacuity]: a direct second call to the internal base-OT
// entry point (a fresh SetupOtState, unrelated to any Pools) increases
// PkOpCounter -- proving SETUP-5's constancy assertion is capable of
// firing, not vacuously true. Scoping note (per the brief): production
// code (Setup::run/refill_offline) never calls run_base_ots a second time
// on the same Pools; this test calls the internal entry point directly,
// which W6.5's own assertions (out of this task's scope) are what would
// catch a REAL regression.
// ---------------------------------------------------------------------------

TEST(Setup, FC3_CLMBNonVacuity_SecondBaseOtCallIncreasesPkOpCounter) {
    Params params = Params::instantiate();
    // Establish a baseline via a real Setup::run first (so the counter is
    // already > 0, matching SETUP-5's own precondition).
    auto [pool_r, pool_s] = run_setup_both(params, PoolSizes{2, 1});
    (void)pool_r;
    (void)pool_s;
    const u64 before = PkOpCounter::value();

    detail::SetupOtState st_r, st_s;
    run_two_party(
        next_address(),
        [&](Channel& ch) { macoro::sync_wait(detail::run_base_ots(Role::Receiver, st_r, ch.socket())); },
        [&](Channel& ch) { macoro::sync_wait(detail::run_base_ots(Role::Sender, st_s, ch.socket())); });

    EXPECT_GT(PkOpCounter::value(), before)
        << "a direct second base-OT execution must be observable by PkOpCounter";
}

// ---------------------------------------------------------------------------
// FC4 [mask canonicality guard]: a forced 61-bit mask == 2^61-1, fed via the
// production PipelineOpts::forced_mask_halves test-support knob, is rejected
// and resampled by the SAME production pipeline Setup calls (Phase-2
// boundary-mask precedent, test/integration/w24_boundary_masks.cpp) -- the
// final mask reaching the output is NEVER 2^61-1.
// ---------------------------------------------------------------------------

TEST(Setup, FC4_ForcedMaskEqualToPMinusOneIsRejectedAndResampled) {
    // Split the forced value r = 2^61-1 non-trivially so neither party holds
    // it in the clear (Task-5/w24_boundary_masks.cpp convention).
    const u64 r = zt::kRejectedMask; // == Fp::P == 2^61-1
    const u64 s_half = 0x0AAAAAAAAAAAAAAAull & Fp::P;
    const u64 r_half = r ^ s_half;

    std::array<coproto::LocalAsyncSocket, 2> sock = coproto::LocalAsyncSocket::makePair();
    std::array<zt::OtPool, 2> pool;
    std::array<oc::PRNG, 2> ot_prng{oc::PRNG(oc::block(916, 0)), oc::PRNG(oc::block(916, 1))};

    auto role = [](int p) { return p == 0 ? zt::Role::Receiver : zt::Role::Sender; };

    // extra_rejections slack: this forced mask IS guaranteed to be rejected
    // at least once (unlike the boundary-mask test's 0/1/p-1 cases, which
    // are never rejected), so the pool must cover the resample round too.
    const u64 ots = pool_size_with_slack(/*gates=*/1, /*extra_rejections=*/4);
    {
        auto t0 = zt::generate_ot_pool(role(0), ots, ot_prng[0], sock[0], pool[0]);
        auto t1 = zt::generate_ot_pool(role(1), ots, ot_prng[1], sock[1], pool[1]);
        eval(t0, t1);
    }

    std::array<zt::PipelineOpts, 2> opts;
    opts[0].count = 1;
    opts[0].forced_mask_halves = {r_half};
    opts[1].count = 1;
    opts[1].forced_mask_halves = {s_half};
    std::array<oc::PRNG, 2> prng{oc::PRNG(oc::block(917, 0)), oc::PRNG(oc::block(917, 1))};
    std::array<std::vector<zt::ZtGateOut>, 2> out;
    std::array<zt::PipelineStats, 2> stats;
    {
        auto t0 = zt::generate_ztgates(role(0), sock[0], opts[0], prng[0], pool[0], out[0], stats[0]);
        auto t1 = zt::generate_ztgates(role(1), sock[1], opts[1], prng[1], pool[1], out[1], stats[1]);
        eval(t0, t1);
    }

    // Phase-2 semantics: rejected and resampled, not silently accepted.
    EXPECT_GE(stats[0].rejected, 1u);
    EXPECT_GE(stats[0].resample_rounds, 1u);
    EXPECT_EQ(stats[0].rejected, stats[1].rejected);
    EXPECT_EQ(stats[0].resample_rounds, stats[1].resample_rounds);

    const u64 final_mask = out[0][0].mask_half ^ out[1][0].mask_half;
    EXPECT_NE(final_mask, r) << "the forced p-1 mask must never reach the output unchanged";
    EXPECT_LT(final_mask, Fp::P) << "the resampled mask must be canonical";
}

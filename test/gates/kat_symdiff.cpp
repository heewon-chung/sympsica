// test/gates/kat_symdiff.cpp — Task 14 (task-14-brief.md, W4.4/W4.5): the
// formal SD-1..SD-4/TV-F13 KAT suite for gates/symdiff.hpp
// (SymDiffEvaluator), plus the M0 instrumentation pin (W4.5) and a
// batched-schedule regression guard (BatchedMinorsMatchMinorCircuit
// PerBucketRandomTrials). Controller ruling (task-14-report.md addendum):
// gates/symdiff.cpp delegates rounds 1-2 to gates/minors.hpp's
// MinorCircuit::eval_batch -- the schedule's ONE implementation, per the
// design doc's binding single-source-of-truth requirement -- rather than
// re-hosting the m-index schedule itself.
//
// Test IDs carried in the test names:
//   SD-1 [SEED-FIXED] — synthetic buckets t in {0..4} + a padding row (all-
//                        zero shares on BOTH parties): shares sum to t;
//                        padding -> 0.
//   SD-2 [SEED-FIXED] — t = T = 4 exactly (pivot at bound): t=4 recovered.
//   SD-3 [IDENTITY]   — batch of B >= 2 buckets: exactly 8 communication
//                        rounds total (Channel::sends_count() delta).
//   SD-4 [CONCRETE]   — per-bucket wire cost: 44 triples / 184 field
//                        elements (both parties' outgoing, summed) / 8
//                        rounds / 1472 bytes; this is also the M0 report's
//                        pinned-row instrumentation (W4.5).
//   TV-F13            — evaluating buckets sequentially (per-bucket calls)
//                        does NOT give the batched 8-round total (8 rounds
//                        is per layer-batched QUERY, not per bucket).
//
// Fresh ZtGates come from the REAL Phase-2 pipeline (converted via
// ztgate_convert.hpp); triples are DEALER-style (Task-13 precedent) for
// every test except the ONE labeled real-VOLE composition test
// (requirement 4). Syndrome fixtures come from test/fixtures/sd0.fixture
// (R-SYNDROMES).

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "coproto/Socket/LocalAsyncSock.h"
#include "cryptoTools/Crypto/PRNG.h"
// macoro/sync_wait.h uses std::source_location/basic_traceable but does not
// include macoro/trace.h itself -- same gap kat_ztest.cpp/kat_minors.cpp
// already document and work around.
#include "macoro/trace.h"
#include "macoro/sync_wait.h"
#include "macoro/when_all.h"

#include "../integration/vole_beaver.hpp"
#include "../integration/ztgate_pipeline.hpp"
#include "../utils/fixture_support.hpp"
#include "sympsica/core/pools.hpp"
#include "sympsica/core/share.hpp"
#include "sympsica/gates/beaver.hpp"
#include "sympsica/gates/minors.hpp"
#include "sympsica/gates/symdiff.hpp"
#include "sympsica/gates/ztest.hpp"
#include "sympsica/utils/field.hpp"
#include "sympsica/utils/net.hpp"
#include "ztgate_convert.hpp"

using namespace sympsica;

namespace {

namespace zt = sympsica::ztgate;
namespace zvole = sympsica::vole;
namespace oc = osuCrypto;

// ---------------------------------------------------------------------------
// Two-party TCP harness (net_smoke.cpp / kat_minors.cpp / kat_ztest.cpp
// pattern).
// ---------------------------------------------------------------------------

std::atomic<int> g_port_counter{47400};

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
// Dealer-style test triples (Task-13 precedent).
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

// Splits ONE bucket's depth-7 syndrome vector into two parties' Share[7]
// arrays (a random additive split -- the padding-row case instead
// constructs literal Share{Fp(0)} on both sides directly, see SD-1 below).
std::pair<std::array<Share, 7>, std::array<Share, 7>> split_d(const std::array<Fp, 7>& d,
                                                                std::mt19937_64& rng) {
    std::uniform_int_distribution<u64> dist(0, Fp::P - 1);
    std::array<Share, 7> s0{}, s1{};
    for (int k = 0; k < 7; ++k) {
        Fp r(dist(rng));
        s0[k] = Share{r};
        s1[k] = Share{d[k].sub(r)};
    }
    return {s0, s1};
}

// ---------------------------------------------------------------------------
// Real ZtGate generation via the Phase-2 pipeline (in-process,
// LocalAsyncSocket), converted to production sympsica::ZtGate -- same
// helper as kat_ztest.cpp's own copy (each test/gates/*.cpp file owns its
// small harness, per this codebase's existing precedent).
// ---------------------------------------------------------------------------

u64 zt_pool_size(u64 gates, u64 extra_rejections) {
    return zt::kOtsPerGate * gates + zt::kAndGates * extra_rejections;
}

void eval_local(macoro::task<>& t0, macoro::task<>& t1) {
    auto r = macoro::sync_wait(macoro::when_all_ready(std::move(t0), std::move(t1)));
    std::get<0>(r).result();
    std::get<1>(r).result();
}

std::array<std::vector<ZtGate>, 2> gen_ztgates(u64 count, u64 seed) {
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
    opts[0].count = count;
    opts[1].count = count;
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

// ---------------------------------------------------------------------------
// sd0.fixture (R-SYNDROMES): 5 rows, t = 0..4 in order (row index == t).
// ---------------------------------------------------------------------------

struct SdRow {
    std::array<Fp, 7> d;
    u64 t;
};

std::vector<SdRow> load_sd_rows() {
    sympsica_test::Fixture fx(sympsica_test::fixture_path("test/fixtures/sd0.fixture"));
    auto rows = fx.all("sd");
    std::vector<SdRow> out;
    out.reserve(rows.size());
    for (const auto& row : rows) {
        SYMPSICA_REQUIRE(row.size() == 13, "load_sd_rows: unexpected sd row width");
        SdRow r{};
        for (int k = 0; k < 7; ++k) r.d[k] = Fp(std::stoull(row[1 + k]));
        r.t = std::stoull(row[12]);
        out.push_back(r);
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// SD-1 [SEED-FIXED] — synthetic buckets t in {0..4} + a padding row (all-
// zero shares on BOTH parties, R-SYNDROMES): shares sum to t; padding -> 0.
// ---------------------------------------------------------------------------

TEST(GatesSymdiff, SD1_SyntheticBucketsAllTValuesPlusPadding) {
    auto rows = load_sd_rows();
    ASSERT_EQ(rows.size(), 5u);
    for (u64 i = 0; i < 5; ++i) ASSERT_EQ(rows[i].t, i) << "sd0.fixture row " << i;

    const u64 B = 6; // rows[0..4] (t=0..4) + one literal-zero padding bucket
    std::vector<u32> betas(B);
    for (u64 b = 0; b < B; ++b) betas[b] = static_cast<u32>(b);

    std::mt19937_64 rng(0x5D010000ull);
    std::vector<std::array<Share, 7>> syn0(B), syn1(B);
    for (u64 b = 0; b < 5; ++b) {
        auto [s0, s1] = split_d(rows[b].d, rng);
        syn0[b] = s0;
        syn1[b] = s1;
    }
    // Padding row: LITERAL zero shares on both parties (R-SYNDROMES's
    // "all-zero-share row"), not a random split of zero.
    for (int k = 0; k < 7; ++k) {
        syn0[5][k] = Share{Fp(0)};
        syn1[5][k] = Share{Fp(0)};
    }

    auto gates = gen_ztgates(4 * B, 0x5D020000ull);
    ZtGatePool ztp0, ztp1;
    ztp0.refill(std::move(gates[0]));
    ztp1.refill(std::move(gates[1]));
    auto pools = make_dealer_pools(44 * B, 0x5D030000ull);

    std::vector<Share> out0, out1;
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            BeaverEngine eng(Role::Receiver);
            out0 = SymDiffEvaluator::eval_buckets(betas, syn0, eng, pools.party0, ztp0, ch);
        },
        [&](Channel& ch) {
            BeaverEngine eng(Role::Sender);
            out1 = SymDiffEvaluator::eval_buckets(betas, syn1, eng, pools.party1, ztp1, ch);
        });

    ASSERT_EQ(out0.size(), B);
    ASSERT_EQ(out1.size(), B);
    for (u64 b = 0; b < 5; ++b) {
        Fp t = out0[b].v.add(out1[b].v);
        EXPECT_EQ(t.v, rows[b].t) << "bucket " << b << " (t=" << rows[b].t << ")";
    }
    Fp t_pad = out0[5].v.add(out1[5].v);
    EXPECT_EQ(t_pad.v, 0u) << "padding bucket (all-zero shares) must recover t=0";
}

// ---------------------------------------------------------------------------
// SD-2 [SEED-FIXED] — t = T = 4 exactly (pivot at bound): t=4 recovered.
// ---------------------------------------------------------------------------

TEST(GatesSymdiff, SD2_PivotAtBoundTEqualsFour) {
    auto rows = load_sd_rows();
    ASSERT_EQ(rows.size(), 5u);
    const auto& row4 = rows[4];
    ASSERT_EQ(row4.t, 4u);

    std::mt19937_64 rng(0x5D040000ull);
    auto [s0, s1] = split_d(row4.d, rng);
    std::vector<u32> betas{0};
    std::vector<std::array<Share, 7>> syn0{s0}, syn1{s1};

    auto gates = gen_ztgates(4, 0x5D050000ull);
    ZtGatePool ztp0, ztp1;
    ztp0.refill(std::move(gates[0]));
    ztp1.refill(std::move(gates[1]));
    auto pools = make_dealer_pools(44, 0x5D060000ull);

    std::vector<Share> out0, out1;
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            BeaverEngine eng(Role::Receiver);
            out0 = SymDiffEvaluator::eval_buckets(betas, syn0, eng, pools.party0, ztp0, ch);
        },
        [&](Channel& ch) {
            BeaverEngine eng(Role::Sender);
            out1 = SymDiffEvaluator::eval_buckets(betas, syn1, eng, pools.party1, ztp1, ch);
        });

    ASSERT_EQ(out0.size(), 1u);
    ASSERT_EQ(out1.size(), 1u);
    Fp t = out0[0].v.add(out1[0].v);
    EXPECT_EQ(t.v, 4u);
}

// ---------------------------------------------------------------------------
// SD-3 [IDENTITY] — batch of B >= 2 buckets: exactly 8 communication
// rounds total (Channel::sends_count() delta, R-ROUNDS).
// ---------------------------------------------------------------------------

TEST(GatesSymdiff, SD3_BatchOfTwoOrMoreBucketsExactlyEightRoundsTotal) {
    auto rows = load_sd_rows();
    const u64 B = 3;
    const std::array<u64, 3> pick{1, 2, 4};
    std::vector<u32> betas{0, 1, 2};

    std::mt19937_64 rng(0x5D070000ull);
    std::vector<std::array<Share, 7>> syn0(B), syn1(B);
    for (u64 b = 0; b < B; ++b) {
        auto [s0, s1] = split_d(rows[pick[b]].d, rng);
        syn0[b] = s0;
        syn1[b] = s1;
    }

    auto gates = gen_ztgates(4 * B, 0x5D080000ull);
    ZtGatePool ztp0, ztp1;
    ztp0.refill(std::move(gates[0]));
    ztp1.refill(std::move(gates[1]));
    auto pools = make_dealer_pools(44 * B, 0x5D090000ull);

    std::vector<Share> out0, out1;
    u64 rounds0 = 0, rounds1 = 0;
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            BeaverEngine eng(Role::Receiver);
            const u64 before = ch.sends_count();
            out0 = SymDiffEvaluator::eval_buckets(betas, syn0, eng, pools.party0, ztp0, ch);
            rounds0 = ch.sends_count() - before;
        },
        [&](Channel& ch) {
            BeaverEngine eng(Role::Sender);
            const u64 before = ch.sends_count();
            out1 = SymDiffEvaluator::eval_buckets(betas, syn1, eng, pools.party1, ztp1, ch);
            rounds1 = ch.sends_count() - before;
        });

    EXPECT_EQ(rounds0, 8u) << "Receiver's send() count must be exactly 8, batch size B=" << B;
    EXPECT_EQ(rounds1, 8u) << "Sender's send() count must be exactly 8, batch size B=" << B;

    ASSERT_EQ(out0.size(), B);
    ASSERT_EQ(out1.size(), B);
    for (u64 b = 0; b < B; ++b) {
        Fp t = out0[b].v.add(out1[b].v);
        EXPECT_EQ(t.v, rows[pick[b]].t) << "bucket " << b;
    }
}

// ---------------------------------------------------------------------------
// SD-4 [CONCRETE] / M0 instrumentation (W4.5) — per-bucket wire cost:
// exactly 184 field elements (1472 B, both parties' outgoing summed) / 44
// triples / 8 rounds, pinned for a 2-bucket batch (88 triples/party, 368
// elements both-parties, 8 rounds -- the SC line's "(88/368/8)").
// ---------------------------------------------------------------------------

TEST(GatesSymdiff, SD4_M0_WireTripleAndRoundCountPinnedRow) {
    auto rows = load_sd_rows();
    const u64 B = 2;
    const std::array<u64, 2> pick{2, 4};
    std::vector<u32> betas{0, 1};

    std::mt19937_64 rng(0x5D0A0000ull);
    std::vector<std::array<Share, 7>> syn0(B), syn1(B);
    for (u64 b = 0; b < B; ++b) {
        auto [s0, s1] = split_d(rows[pick[b]].d, rng);
        syn0[b] = s0;
        syn1[b] = s1;
    }

    auto gates = gen_ztgates(4 * B, 0x5D0B0000ull);
    ZtGatePool ztp0, ztp1;
    ztp0.refill(std::move(gates[0]));
    ztp1.refill(std::move(gates[1]));
    auto pools = make_dealer_pools(44 * B, 0x5D0C0000ull);

    u64 triples0 = 0, triples1 = 0, rounds0 = 0, rounds1 = 0, bytes0 = 0, bytes1 = 0;
    std::vector<Share> out0, out1;
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            BeaverEngine eng(Role::Receiver);
            const u64 rem_before = pools.party0.remaining();
            const u64 sc_before = ch.sends_count();
            ByteMeter meter(ch);
            out0 = SymDiffEvaluator::eval_buckets(betas, syn0, eng, pools.party0, ztp0, ch);
            bytes0 = meter.bytes();
            triples0 = rem_before - pools.party0.remaining();
            rounds0 = ch.sends_count() - sc_before;
        },
        [&](Channel& ch) {
            BeaverEngine eng(Role::Sender);
            const u64 rem_before = pools.party1.remaining();
            const u64 sc_before = ch.sends_count();
            ByteMeter meter(ch);
            out1 = SymDiffEvaluator::eval_buckets(betas, syn1, eng, pools.party1, ztp1, ch);
            bytes1 = meter.bytes();
            triples1 = rem_before - pools.party1.remaining();
            rounds1 = ch.sends_count() - sc_before;
        });

    EXPECT_EQ(triples0, 44u * B) << "triples consumed, Receiver's own pool";
    EXPECT_EQ(triples1, 44u * B) << "triples consumed, Sender's own pool";
    EXPECT_EQ(rounds0, 8u);
    EXPECT_EQ(rounds1, 8u);
    EXPECT_EQ(bytes0, 736u * B) << "outgoing bytes, Receiver (92 elements/bucket * 8 bytes)";
    EXPECT_EQ(bytes1, 736u * B) << "outgoing bytes, Sender";

    // Per-bucket rate, derived from the B=2 run and asserted directly
    // against W4.5's pinned row.
    const u64 bytes_both_parties = bytes0 + bytes1;
    const u64 elements_both_parties_per_bucket = bytes_both_parties / (8 * B);
    const u64 bytes_both_parties_per_bucket = bytes_both_parties / B;
    const u64 triples_per_bucket = triples0 / B;
    EXPECT_EQ(elements_both_parties_per_bucket, 184u);
    EXPECT_EQ(bytes_both_parties_per_bucket, 1472u);
    EXPECT_EQ(triples_per_bucket, 44u);

    std::fprintf(
        stderr,
        "[M0] pinned row: {triples:44, elements:184, rounds:8, bytes:1472} (per bucket) -- "
        "B=%llu actual: triples=%llu/party (%llu total), elements=%llu both-parties, "
        "rounds=%llu (BOTH parties), bytes=%llu both-parties. "
        "Paper-bound delta: mu_impl=29 minors mults < mu(4)<=40 (paper bound); "
        "this gate's implementation total 44 triples < paper's 55-triple upper bound at "
        "mu=40; 184 field elements < paper's 228-element upper bound. The 55/228 figures "
        "in the manuscript are UPPER BOUNDS at mu=40, not the tight implementation "
        "constants pinned here -- flag for the manuscript constants note.\n",
        (unsigned long long)B, (unsigned long long)triples0, (unsigned long long)(triples0 + triples1),
        (unsigned long long)(bytes_both_parties / 8), (unsigned long long)rounds0,
        (unsigned long long)bytes_both_parties);

    ASSERT_EQ(out0.size(), B);
    ASSERT_EQ(out1.size(), B);
    for (u64 b = 0; b < B; ++b) {
        Fp t = out0[b].v.add(out1[b].v);
        EXPECT_EQ(t.v, rows[pick[b]].t) << "bucket " << b;
    }
}

// ---------------------------------------------------------------------------
// TV-F13 — evaluating buckets SEQUENTIALLY (one eval_buckets(B=1) call per
// bucket) does NOT give the batched 8-round total: 8 rounds is per
// LAYER-BATCHED query, not per bucket, so SD-3's "exactly 8 rounds" claim
// FAILS against this usage (2 sequential single-bucket calls cost 16
// rounds, not 8).
// ---------------------------------------------------------------------------

TEST(GatesSymdiff, TVF13_SequentialPerBucketEvaluationDoesNotGiveEightRoundsTotal) {
    auto rows = load_sd_rows();
    const u64 B = 2;
    const std::array<u64, 2> pick{1, 3};

    std::mt19937_64 rng(0x5D0D0000ull);
    std::vector<std::array<Share, 7>> syn0(B), syn1(B);
    for (u64 b = 0; b < B; ++b) {
        auto [s0, s1] = split_d(rows[pick[b]].d, rng);
        syn0[b] = s0;
        syn1[b] = s1;
    }

    auto gates = gen_ztgates(4 * B, 0x5D0E0000ull);
    ZtGatePool ztp0, ztp1;
    ztp0.refill(std::move(gates[0]));
    ztp1.refill(std::move(gates[1]));
    auto pools = make_dealer_pools(44 * B, 0x5D0F0000ull);

    u64 rounds0 = 0, rounds1 = 0;
    std::vector<Share> outs0(B), outs1(B);
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            BeaverEngine eng(Role::Receiver);
            const u64 before = ch.sends_count();
            for (u64 b = 0; b < B; ++b) {
                std::vector<u32> beta1{static_cast<u32>(b)};
                std::vector<std::array<Share, 7>> syn1v{syn0[b]};
                auto out = SymDiffEvaluator::eval_buckets(beta1, syn1v, eng, pools.party0, ztp0, ch);
                outs0[b] = out[0];
            }
            rounds0 = ch.sends_count() - before;
        },
        [&](Channel& ch) {
            BeaverEngine eng(Role::Sender);
            const u64 before = ch.sends_count();
            for (u64 b = 0; b < B; ++b) {
                std::vector<u32> beta1{static_cast<u32>(b)};
                std::vector<std::array<Share, 7>> syn1v{syn1[b]};
                auto out = SymDiffEvaluator::eval_buckets(beta1, syn1v, eng, pools.party1, ztp1, ch);
                outs1[b] = out[0];
            }
            rounds1 = ch.sends_count() - before;
        });

    EXPECT_EQ(rounds0, 8u * B) << "sequential per-bucket calls: rounds accumulate PER call";
    EXPECT_EQ(rounds1, 8u * B);
    EXPECT_NE(rounds0, 8u) << "the SD-3 claim (exactly 8 rounds TOTAL) must FAIL against "
                              "sequential per-bucket evaluation";

    for (u64 b = 0; b < B; ++b) {
        Fp t = outs0[b].v.add(outs1[b].v);
        EXPECT_EQ(t.v, rows[pick[b]].t) << "bucket " << b;
    }
}

// ---------------------------------------------------------------------------
// Regression guard: SymDiffEvaluator::eval_buckets's minors rounds (which
// call MinorCircuit::eval_batch) are cross-checked against MinorCircuit::
// eval's single-bucket path (kat_minors.cpp's MIN-1..5 suite) on random
// trials, via the reconstructed t each path produces. NOTE (controller
// ruling, task-14-report.md addendum): now that gates/symdiff.cpp
// delegates to MinorCircuit::eval_batch instead of re-hosting the
// schedule, eval() ITSELF is a thin wrapper calling eval_batch with a
// batch of one (minors.cpp) -- so this test is "trivially true by
// construction" in the sense that both paths ultimately run the exact
// same eval_batch code. Kept anyway (per the controller's "keep or
// simplify, your call") as a black-box regression guard on the PUBLIC
// eval()/eval_buckets() contract: it would still catch a future change
// that broke that equivalence (e.g. eval() stopping being a thin wrapper,
// or eval_buckets() diverging from eval_batch's per-bucket semantics)
// without relying on any white-box knowledge of the current
// implementation.
// ---------------------------------------------------------------------------

TEST(GatesSymdiff, BatchedMinorsMatchMinorCircuitPerBucketRandomTrials) {
    constexpr u64 kTrials = 15;
    std::mt19937_64 rng(0x5D100000ull);
    std::uniform_int_distribution<u64> dist(0, Fp::P - 1);

    std::vector<std::array<Fp, 7>> d_all(kTrials);
    for (auto& d : d_all)
        for (auto& x : d) x = Fp(dist(rng));

    // (a) golden: per-bucket MinorCircuit::eval + t_of, dealer triples.
    std::vector<u64> t_golden(kTrials);
    {
        auto pools = make_dealer_pools(29 * kTrials, 0x5D110000ull);
        std::vector<std::vector<Share>> d0_all(kTrials), d1_all(kTrials);
        for (u64 i = 0; i < kTrials; ++i) {
            std::vector<Fp> plain(d_all[i].begin(), d_all[i].end());
            auto [s0, s1] = split_shares(plain, 0x5D120000ull + i);
            d0_all[i] = s0;
            d1_all[i] = s1;
        }
        std::vector<std::array<Share, 4>> out0(kTrials), out1(kTrials);
        run_two_party(
            next_address(),
            [&](Channel& ch) {
                BeaverEngine eng(Role::Receiver);
                for (u64 i = 0; i < kTrials; ++i)
                    out0[i] = MinorCircuit::eval(d0_all[i], eng, pools.party0, ch);
            },
            [&](Channel& ch) {
                BeaverEngine eng(Role::Sender);
                for (u64 i = 0; i < kTrials; ++i)
                    out1[i] = MinorCircuit::eval(d1_all[i], eng, pools.party1, ch);
            });
        for (u64 i = 0; i < kTrials; ++i) {
            std::array<Fp, 4> D{};
            for (int k = 0; k < 4; ++k) D[k] = out0[i][k].v.add(out1[i][k].v);
            t_golden[i] = MinorCircuit::t_of(D);
        }
    }

    // (b) SymDiffEvaluator::eval_buckets batched, SAME d values (freshly
    // re-split, independent randomness -- doesn't matter, same secret).
    std::vector<u32> betas(kTrials);
    for (u64 i = 0; i < kTrials; ++i) betas[i] = static_cast<u32>(i);
    std::mt19937_64 rng2(0x5D130000ull);
    std::vector<std::array<Share, 7>> syn0(kTrials), syn1(kTrials);
    for (u64 i = 0; i < kTrials; ++i) {
        auto [s0, s1] = split_d(d_all[i], rng2);
        syn0[i] = s0;
        syn1[i] = s1;
    }

    auto gates = gen_ztgates(4 * kTrials, 0x5D140000ull);
    ZtGatePool ztp0, ztp1;
    ztp0.refill(std::move(gates[0]));
    ztp1.refill(std::move(gates[1]));
    auto pools2 = make_dealer_pools(44 * kTrials, 0x5D150000ull);

    std::vector<Share> out0, out1;
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            BeaverEngine eng(Role::Receiver);
            out0 = SymDiffEvaluator::eval_buckets(betas, syn0, eng, pools2.party0, ztp0, ch);
        },
        [&](Channel& ch) {
            BeaverEngine eng(Role::Sender);
            out1 = SymDiffEvaluator::eval_buckets(betas, syn1, eng, pools2.party1, ztp1, ch);
        });

    ASSERT_EQ(out0.size(), kTrials);
    u64 bad = 0;
    for (u64 i = 0; i < kTrials; ++i) {
        Fp t = out0[i].v.add(out1[i].v);
        if (t.v != t_golden[i]) {
            ++bad;
            ADD_FAILURE() << "trial " << i << ": batched-schedule t=" << t.v
                           << " MinorCircuit-schedule t=" << t_golden[i];
        }
    }
    EXPECT_EQ(bad, 0u) << "the batched m-index schedule diverged from MinorCircuit::eval's "
                           "independently-tested schedule on at least one trial";
}

// ---------------------------------------------------------------------------
// LABELED end-to-end composition test (task-14-brief.md requirement 4):
// the ONE test in this file where the TriplePool is filled through the
// PRODUCTION VOLE path (vole_beaver::beaver_triples) rather than dealer
// triples; ZtGatePool is already always real (a DPF key cannot be dealer-
// faked). Every OTHER test above uses dealer triples deliberately (Task-13
// precedent).
// ---------------------------------------------------------------------------

TEST(GatesSymdiff, SymDiff_RealVoleTriplesAndRealPipelineGatesEndToEnd) {
    auto rows = load_sd_rows();
    const u64 B = 2;
    const std::array<u64, 2> pick{1, 4};
    std::vector<u32> betas{0, 1};

    std::mt19937_64 rng(0x5D160000ull);
    std::vector<std::array<Share, 7>> syn0(B), syn1(B);
    for (u64 b = 0; b < B; ++b) {
        auto [s0, s1] = split_d(rows[pick[b]].d, rng);
        syn0[b] = s0;
        syn1[b] = s1;
    }

    auto gates = gen_ztgates(4 * B, 0x5D170000ull);
    ZtGatePool ztp0, ztp1;
    ztp0.refill(std::move(gates[0]));
    ztp1.refill(std::move(gates[1]));

    constexpr u64 kNeeded = 44 * 2; // 44 triples/bucket * B=2

    std::vector<Share> out0, out1;
    run_two_party(
        next_address(),
        [&](Channel& ch) { // Receiver
            oc::PRNG ot_prng(oc::block(0x5D18ull, 0));
            zt::OtPool ot_pool;
            macoro::sync_wait(zt::generate_ot_pool(zt::Role::Receiver, kNeeded * 2 * zvole::kOlePerCallOts,
                                                    ot_prng, ch.socket(), ot_pool));
            oc::PRNG beaver_prng(oc::block(0x5D19ull, 0));
            zvole::BeaverBatch batch;
            macoro::sync_wait(
                zvole::beaver_triples(zt::Role::Receiver, kNeeded, beaver_prng, ot_pool, ch.socket(), batch));

            TriplePool pool;
            std::vector<Triple> triples;
            triples.reserve(kNeeded);
            for (u64 i = 0; i < kNeeded; ++i)
                triples.push_back(Triple{i, Share{batch.a[i]}, Share{batch.b[i]}, Share{batch.c[i]}});
            pool.refill(std::move(triples));

            BeaverEngine eng(Role::Receiver);
            out0 = SymDiffEvaluator::eval_buckets(betas, syn0, eng, pool, ztp0, ch);
        },
        [&](Channel& ch) { // Sender
            oc::PRNG ot_prng(oc::block(0x5D18ull, 1));
            zt::OtPool ot_pool;
            macoro::sync_wait(zt::generate_ot_pool(zt::Role::Sender, kNeeded * 2 * zvole::kOlePerCallOts,
                                                    ot_prng, ch.socket(), ot_pool));
            oc::PRNG beaver_prng(oc::block(0x5D19ull, 1));
            zvole::BeaverBatch batch;
            macoro::sync_wait(
                zvole::beaver_triples(zt::Role::Sender, kNeeded, beaver_prng, ot_pool, ch.socket(), batch));

            TriplePool pool;
            std::vector<Triple> triples;
            triples.reserve(kNeeded);
            for (u64 i = 0; i < kNeeded; ++i)
                triples.push_back(Triple{i, Share{batch.a[i]}, Share{batch.b[i]}, Share{batch.c[i]}});
            pool.refill(std::move(triples));

            BeaverEngine eng(Role::Sender);
            out1 = SymDiffEvaluator::eval_buckets(betas, syn1, eng, pool, ztp1, ch);
        });

    ASSERT_EQ(out0.size(), B);
    ASSERT_EQ(out1.size(), B);
    for (u64 b = 0; b < B; ++b) {
        Fp t = out0[b].v.add(out1[b].v);
        EXPECT_EQ(t.v, rows[pick[b]].t) << "bucket " << b;
    }
}

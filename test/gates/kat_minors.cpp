// test/gates/kat_minors.cpp — Task 13 (task-13-brief.md, W4.1/W4.2): the
// formal MIN-1..5 KAT suite for gates/beaver.hpp (BeaverEngine) and
// gates/minors.hpp (MinorCircuit), plus a designated-party affine() pin
// (entry obligation b) and BeaverEngine's generic wire-cost pin.
//
// Test IDs carried in the test names:
//   MIN-1 [IDENTITY]           — rank-1 identity: D_1 = x, D_2 = 0.
//   MIN-2 [CONCRETE]           — the F_101 worked row (100,95,0,0); via a
//                                 plaintext helper mirroring the SAME m/T/B
//                                 combos, since MinorCircuit is hardwired to
//                                 the production Fp (p=2^61-1).
//   MIN-3 [SEED-FIXED]         — interior accidental zero (D_1=0, t in
//                                 {2,3,4}): the max rule (TV-F7) must still
//                                 recover the true t.
//   MIN-4 [SEED-FIXED]         — 500 random signed sets (100 per t in
//                                 {0..4}): recovered t exact vs reference.py.
//   MIN-5 [CONCRETE]           — triple-consumption count of one
//                                 MinorCircuit::eval == 29.
//
// Triple sourcing (task-13-brief.md requirement 1): every test above uses
// DEALER-STYLE test triples (locally sampled a,b,c=a*b, split into shares)
// -- faster, and explicitly permitted for correctness KATs. Exactly ONE
// test (labeled below) instead runs a small batch through REAL VOLE-
// generated triples (test/integration/vole_beaver.hpp), proving the
// production triple-generation path composes with the production
// BeaverEngine/MinorCircuit path end-to-end.
//
// Two-party harness: real localhost TCP Channel pairs, two OS threads
// (test/utils/net_smoke.cpp's connect-with-retry pattern), one fresh port
// per call so sequential tests in this binary never collide.

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

#include "cryptoTools/Crypto/PRNG.h"
// macoro/sync_wait.h uses std::source_location/basic_traceable but does not
// include macoro/trace.h itself; other files in this project that drive a
// real coproto::asio Channel through macoro::sync_wait (not
// coproto::LocalAsyncSocket) hit the same gap and supply it directly --
// test/integration/w24_pool_gate.cpp's header comment has the full
// rationale.
#include "macoro/trace.h"
#include "macoro/sync_wait.h"

#include "../integration/vole_beaver.hpp"
#include "../integration/ztgate_pipeline.hpp"
#include "../utils/fixture_support.hpp"
#include "sympsica/core/pools.hpp"
#include "sympsica/core/share.hpp"
#include "sympsica/gates/beaver.hpp"
#include "sympsica/gates/minors.hpp"
#include "sympsica/utils/field.hpp"
#include "sympsica/utils/net.hpp"

using namespace sympsica;

namespace {

namespace zt = sympsica::ztgate;
namespace zvole = sympsica::vole;
namespace oc = osuCrypto;

// ---------------------------------------------------------------------------
// Two-party TCP harness (net_smoke.cpp pattern): the calling thread dials as
// client (Receiver), a spawned thread listens as server (Sender). A fresh
// port per call avoids TIME_WAIT collisions between the many sequential
// tests in this binary.
// ---------------------------------------------------------------------------

std::atomic<int> g_port_counter{47100};

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

// Runs `receiver_fn(Channel&)` on the calling thread (TCP client, Role::
// Receiver) and `sender_fn(Channel&)` on a spawned thread (TCP server,
// Role::Sender), joining before returning.
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
// Dealer-style test triples (task-13-brief.md requirement 1): locally
// sample (a,b,c=a*b) in full, split into additive shares, hand each party
// its own pre-filled TriplePool. Faster than real VOLE-generated triples
// and explicitly permitted for correctness KATs.
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

// Splits a plaintext Fp vector into two parties' Share vectors (test-only
// secret sharing, not a protocol primitive).
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
// MIN-2's plaintext helper: MinorCircuit is hardwired to the production Fp
// (p=2^61-1), so it cannot run directly over the toy field F_101 the doc's
// worked row uses. This mirrors MinorCircuit::eval's EXACT m/T/B/n schedule
// (task-13-brief.md requirement 5) by hand over plain F_101 integers.
// ---------------------------------------------------------------------------

constexpr long long kToyP = 101;
long long toy_mod(long long v) {
    v %= kToyP;
    if (v < 0) v += kToyP;
    return v;
}
long long toy_mul(long long a, long long b) { return toy_mod(a * b); }
long long toy_sub(long long a, long long b) { return toy_mod(a - b); }

std::array<long long, 4> min_plaintext_f101(const std::array<long long, 7>& d) {
    long long m[20] = {
        toy_mul(d[0], d[2]), toy_mul(d[1], d[1]), toy_mul(d[0], d[3]), toy_mul(d[1], d[2]),
        toy_mul(d[0], d[4]), toy_mul(d[1], d[3]), toy_mul(d[2], d[2]), toy_mul(d[1], d[4]),
        toy_mul(d[2], d[3]), toy_mul(d[2], d[4]), toy_mul(d[3], d[3]), toy_mul(d[2], d[5]),
        toy_mul(d[3], d[4]), toy_mul(d[2], d[6]), toy_mul(d[3], d[5]), toy_mul(d[4], d[4]),
        toy_mul(d[3], d[6]), toy_mul(d[4], d[5]), toy_mul(d[4], d[6]), toy_mul(d[5], d[5]),
    };
    long long T12 = toy_sub(m[0], m[1]), T13 = toy_sub(m[2], m[3]), T14 = toy_sub(m[4], m[5]);
    long long T23 = toy_sub(m[5], m[6]), T24 = toy_sub(m[7], m[8]), T34 = toy_sub(m[9], m[10]);
    long long B12 = toy_sub(m[9], m[10]), B13 = toy_sub(m[11], m[12]), B14 = toy_sub(m[13], m[14]);
    long long B23 = toy_sub(m[14], m[15]), B24 = toy_sub(m[16], m[17]), B34 = toy_sub(m[18], m[19]);
    long long A3 = T34, B3 = T24, C3 = T23;

    long long D1 = d[0];
    long long D2 = T12;
    long long n0 = toy_mul(d[0], A3), n1 = toy_mul(d[1], B3), n2 = toy_mul(d[2], C3);
    long long n3 = toy_mul(T12, B34), n4 = toy_mul(T13, B24), n5 = toy_mul(T14, B23);
    long long n6 = toy_mul(T23, B14), n7 = toy_mul(T24, B13), n8 = toy_mul(T34, B12);
    long long D3 = toy_mod(n0 - n1 + n2);
    long long D4 = toy_mod(n3 - n4 + n5 + n6 - n7 + n8);
    return {D1, D2, D3, D4};
}

} // namespace

// ---------------------------------------------------------------------------
// affine() pin (entry obligation b): designated-party convention -- only
// ONE party's call adds `b`; reconstruct and assert a*x + b. Pure local
// computation (no network needed for this one).
// ---------------------------------------------------------------------------

TEST(GatesBeaver, AffineDesignatedPartyConvention) {
    std::mt19937_64 rng(0xA1F1A1F1ull);
    std::uniform_int_distribution<u64> dist(0, Fp::P - 1);
    for (int trial = 0; trial < 50; ++trial) {
        SCOPED_TRACE("trial " + std::to_string(trial));
        Fp x(dist(rng)), a(dist(rng)), b(dist(rng));
        Fp x0(dist(rng));
        Fp x1 = x.sub(x0);

        // designated-party convention: only party 0's call passes b.
        Share r0 = affine(a, Share{x0}, b);
        Share r1 = affine(a, Share{x1}, Fp(0));
        Fp reconstructed = r0.v.add(r1.v);
        Fp want = a.mul(x).add(b);
        EXPECT_EQ(reconstructed.v, want.v);
    }
}

// ---------------------------------------------------------------------------
// BeaverEngine generic wire-cost pin (requirement 3): one mul(N) call sends
// exactly 2N field elements (16N bytes) per party, in ONE round, and
// consumes exactly N triples. Also proves correctness (c == a*b for every
// product), which the MIN-5 wire-cost reasoning below composes from.
// ---------------------------------------------------------------------------

TEST(GatesBeaver, WireCostOneRoundSendsExactly2NFieldElements) {
    constexpr u64 kN = 7;
    auto pools = make_dealer_pools(kN, 0xB33FB33Full);

    std::mt19937_64 rng(0xB340ull);
    std::uniform_int_distribution<u64> dist(0, Fp::P - 1);
    std::vector<Fp> xs(kN), ys(kN);
    for (u64 i = 0; i < kN; ++i) {
        xs[i] = Fp(dist(rng));
        ys[i] = Fp(dist(rng));
    }
    auto [x0, x1] = split_shares(xs, 0xB341ull);
    auto [y0, y1] = split_shares(ys, 0xB342ull);

    const u64 rem0_before = pools.party0.remaining();
    const u64 rem1_before = pools.party1.remaining();

    std::vector<Share> z0, z1;
    u64 bytes0 = 0, bytes1 = 0;
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            BeaverEngine eng(Role::Receiver);
            ByteMeter meter(ch);
            z0 = eng.mul(x0, y0, pools.party0, ch);
            bytes0 = meter.bytes();
        },
        [&](Channel& ch) {
            BeaverEngine eng(Role::Sender);
            ByteMeter meter(ch);
            z1 = eng.mul(x1, y1, pools.party1, ch);
            bytes1 = meter.bytes();
        });

    EXPECT_EQ(bytes0, 16 * kN);
    EXPECT_EQ(bytes1, 16 * kN);
    EXPECT_EQ(rem0_before - pools.party0.remaining(), kN);
    EXPECT_EQ(rem1_before - pools.party1.remaining(), kN);

    ASSERT_EQ(z0.size(), kN);
    ASSERT_EQ(z1.size(), kN);
    for (u64 i = 0; i < kN; ++i) {
        Fp got = z0[i].v.add(z1[i].v);
        Fp want = xs[i].mul(ys[i]);
        EXPECT_EQ(got.v, want.v) << "product " << i;
    }
}

// ---------------------------------------------------------------------------
// MIN-1 [IDENTITY] — rank-1 identity: for any x != 0, D_1 = x, D_2 = 0.
// ---------------------------------------------------------------------------

TEST(GatesMinors, MIN1_RankOneIdentity) {
    constexpr int kTrials = 20;
    std::mt19937_64 rng(0x111111ull);
    std::uniform_int_distribution<u64> dist(1, Fp::P - 1); // != 0

    std::vector<Fp> xs(kTrials);
    std::vector<std::vector<Fp>> d_all(kTrials);
    for (int trial = 0; trial < kTrials; ++trial) {
        xs[trial] = Fp(dist(rng));
        d_all[trial].resize(7);
        for (int k = 0; k < 7; ++k) d_all[trial][k] = xs[trial].pow(static_cast<u64>(k + 1));
    }

    auto pools = make_dealer_pools(29 * static_cast<u64>(kTrials), 0x111200ull);
    std::vector<std::vector<Share>> d0_all(kTrials), d1_all(kTrials);
    for (int trial = 0; trial < kTrials; ++trial) {
        auto [s0, s1] = split_shares(d_all[trial], 0x111300ull + static_cast<u64>(trial));
        d0_all[trial] = std::move(s0);
        d1_all[trial] = std::move(s1);
    }

    std::vector<std::array<Share, 4>> out0(kTrials), out1(kTrials);
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            BeaverEngine eng(Role::Receiver);
            for (int trial = 0; trial < kTrials; ++trial)
                out0[trial] = MinorCircuit::eval(d0_all[trial], eng, pools.party0, ch);
        },
        [&](Channel& ch) {
            BeaverEngine eng(Role::Sender);
            for (int trial = 0; trial < kTrials; ++trial)
                out1[trial] = MinorCircuit::eval(d1_all[trial], eng, pools.party1, ch);
        });

    for (int trial = 0; trial < kTrials; ++trial) {
        SCOPED_TRACE("trial " + std::to_string(trial));
        std::array<Fp, 4> D{};
        for (int i = 0; i < 4; ++i) D[i] = out0[trial][i].v.add(out1[trial][i].v);
        EXPECT_NE(D[0].v, 0u);
        EXPECT_EQ(D[0].v, xs[trial].v);
        EXPECT_EQ(D[1].v, 0u);
        EXPECT_EQ(MinorCircuit::t_of(D), 1u);
    }
}

// ---------------------------------------------------------------------------
// MIN-2 [CONCRETE] — the F_101 worked row: signed difference {2(+), 3(-)},
// t=2: D_1=100, D_2=95, D_3=0, D_4=0. Via the plaintext helper above (see
// requirement 5: MinorCircuit is Fp-hardwired to p=2^61-1).
// ---------------------------------------------------------------------------

TEST(GatesMinors, MIN2_ConcreteF101Row) {
    std::array<long long, 7> d{};
    for (int k = 1; k <= 7; ++k) {
        long long p2 = 1, p3 = 1;
        for (int i = 0; i < k; ++i) {
            p2 = toy_mul(p2, 2);
            p3 = toy_mul(p3, 3);
        }
        d[k - 1] = toy_sub(p2, p3);
    }
    ASSERT_EQ(d[0], 100);
    ASSERT_EQ(d[1], 96);
    ASSERT_EQ(d[2], 82);
    ASSERT_EQ(d[3], 36);
    ASSERT_EQ(d[4], 92);

    auto D = min_plaintext_f101(d);
    EXPECT_EQ(D[0], 100);
    EXPECT_EQ(D[1], 95);
    EXPECT_EQ(D[2], 0);
    EXPECT_EQ(D[3], 0);
}

// ---------------------------------------------------------------------------
// MIN-3 [SEED-FIXED] — interior accidental zero (D_1=0, t in {2,3,4},
// found by reference.py search per Remark 1): the max rule (TV-F7) must
// still recover the true t, not the first-zero index. Runs the REAL
// two-party MinorCircuit/BeaverEngine over dealer triples.
// ---------------------------------------------------------------------------

TEST(GatesMinors, MIN3_InteriorAccidentalZero_MaxRuleTVF7) {
    sympsica_test::Fixture fx(sympsica_test::fixture_path("test/fixtures/min0.fixture"));
    const auto rows = fx.all("min3");
    ASSERT_EQ(rows.size(), fx.u64_at("min3_count"));
    ASSERT_FALSE(rows.empty());

    const std::size_t n_cases = rows.size();
    std::vector<std::vector<Fp>> d_all(n_cases);
    std::vector<std::array<Fp, 4>> D_expected(n_cases);
    std::vector<u64> t_expected(n_cases);
    for (std::size_t ri = 0; ri < n_cases; ++ri) {
        const auto& row = rows[ri];
        ASSERT_EQ(row.size(), 13u) << "min3 row " << ri;
        d_all[ri].resize(7);
        for (int k = 0; k < 7; ++k) d_all[ri][k] = Fp(std::stoull(row[1 + k]));
        for (int i = 0; i < 4; ++i) D_expected[ri][i] = Fp(std::stoull(row[8 + i]));
        t_expected[ri] = std::stoull(row[12]);

        // The fixture must genuinely be an interior-zero case: D_1 == 0
        // (the deliberate construction) while D_[t_expected] != 0.
        ASSERT_TRUE(D_expected[ri][0].is_zero()) << "min3 row " << ri << ": D_1 must be zero";
        ASSERT_FALSE(D_expected[ri][t_expected[ri] - 1].is_zero())
            << "min3 row " << ri << ": D_[t] must be nonzero";
    }

    auto pools = make_dealer_pools(29 * static_cast<u64>(n_cases), 0x330000ull);
    std::vector<std::vector<Share>> d0_all(n_cases), d1_all(n_cases);
    for (std::size_t ri = 0; ri < n_cases; ++ri) {
        auto [s0, s1] = split_shares(d_all[ri], 0x340000ull + ri);
        d0_all[ri] = std::move(s0);
        d1_all[ri] = std::move(s1);
    }

    std::vector<std::array<Share, 4>> out0(n_cases), out1(n_cases);
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            BeaverEngine eng(Role::Receiver);
            for (std::size_t ri = 0; ri < n_cases; ++ri)
                out0[ri] = MinorCircuit::eval(d0_all[ri], eng, pools.party0, ch);
        },
        [&](Channel& ch) {
            BeaverEngine eng(Role::Sender);
            for (std::size_t ri = 0; ri < n_cases; ++ri)
                out1[ri] = MinorCircuit::eval(d1_all[ri], eng, pools.party1, ch);
        });

    for (std::size_t ri = 0; ri < n_cases; ++ri) {
        SCOPED_TRACE("min3 row " + std::to_string(ri) + " (t=" + std::to_string(t_expected[ri]) + ")");
        std::array<Fp, 4> D{};
        for (int i = 0; i < 4; ++i) D[i] = out0[ri][i].v.add(out1[ri][i].v);
        for (int i = 0; i < 4; ++i) EXPECT_EQ(D[i].v, D_expected[ri][i].v) << "D[" << i << "]";

        u64 t_got = MinorCircuit::t_of(D);
        EXPECT_EQ(t_got, t_expected[ri])
            << "max rule must recover the true t despite the interior D_1=0 zero "
               "(a first-zero rule would wrongly report t=0 here -- TV-F7)";
    }
}

// --- TV-F7 (negative-as-positive-assert, task-20-brief.md W6.1 / R6-FSUITE:
// "a row with no wrong construction yet is NEW WORK"). MIN-3 above only
// PINS the correct (max-rule) t and documents the rejected "first zero
// minor" rule in a comment; this test makes that rejected rule EXECUTABLE
// on MIN-3's own interior-zero fixture rows and asserts it disagrees with
// the true t on every one of them.
TEST(GatesMinors, TVF7_FirstZeroRuleMisreadsInteriorZeroCases) {
    sympsica_test::Fixture fx(sympsica_test::fixture_path("test/fixtures/min0.fixture"));
    const auto rows = fx.all("min3");
    ASSERT_FALSE(rows.empty());

    bool exercised_interior_zero = false;
    for (const auto& row : rows) {
        ASSERT_EQ(row.size(), 13u);
        std::array<Fp, 4> D{};
        for (int i = 0; i < 4; ++i) D[i] = Fp(std::stoull(row[8 + i]));
        const u64 t_expected = std::stoull(row[12]);

        if (!D[0].is_zero()) continue; // not an interior-zero case: TV-F7 does not apply
        exercised_interior_zero = true;

        // The rejected rule: t = index (1-based) of the FIRST zero D_i,
        // or 0 if none are zero. D[0] is zero by construction here, so
        // this rule always reports t=1 on these rows.
        u64 first_zero_t = 0;
        for (int i = 0; i < 4; ++i) {
            if (D[i].is_zero()) { first_zero_t = static_cast<u64>(i + 1); break; }
        }
        ASSERT_EQ(first_zero_t, 1u)
            << "test precondition: D_1 == 0 must make the first-zero rule report t=1";
        EXPECT_NE(first_zero_t, t_expected)
            << "TV-F7: the first-zero rule must misreport t on an interior-zero case whose true "
               "t (max rule) is " << t_expected;
    }
    ASSERT_TRUE(exercised_interior_zero)
        << "test precondition: min3 fixture must contain at least one interior-zero (D_1=0) row";
}

// ---------------------------------------------------------------------------
// MIN-4 [SEED-FIXED] — all t in {0..4}, 100 random signed sets each (500
// total): recovered t exact vs reference.py. Runs the REAL two-party
// MinorCircuit/BeaverEngine over dealer triples, one shared channel for all
// 500 cases.
// ---------------------------------------------------------------------------

TEST(GatesMinors, MIN4_AllTRandomSignedSetsMatchReference) {
    sympsica_test::Fixture fx(sympsica_test::fixture_path("test/fixtures/min0.fixture"));
    const auto rows = fx.all("min4");
    ASSERT_EQ(rows.size(), fx.u64_at("min4_count"));
    ASSERT_EQ(rows.size(), 500u);

    const std::size_t n_cases = rows.size();
    std::vector<std::vector<Fp>> d_all(n_cases);
    std::vector<std::array<Fp, 4>> D_expected(n_cases);
    std::vector<u64> t_expected(n_cases);
    for (std::size_t ri = 0; ri < n_cases; ++ri) {
        const auto& row = rows[ri];
        ASSERT_EQ(row.size(), 13u) << "min4 row " << ri;
        d_all[ri].resize(7);
        for (int k = 0; k < 7; ++k) d_all[ri][k] = Fp(std::stoull(row[1 + k]));
        for (int i = 0; i < 4; ++i) D_expected[ri][i] = Fp(std::stoull(row[8 + i]));
        t_expected[ri] = std::stoull(row[12]);
    }

    auto pools = make_dealer_pools(29 * static_cast<u64>(n_cases), 0x440000ull);
    std::vector<std::vector<Share>> d0_all(n_cases), d1_all(n_cases);
    for (std::size_t ri = 0; ri < n_cases; ++ri) {
        auto [s0, s1] = split_shares(d_all[ri], 0x450000ull + ri);
        d0_all[ri] = std::move(s0);
        d1_all[ri] = std::move(s1);
    }

    std::vector<std::array<Share, 4>> out0(n_cases), out1(n_cases);
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            BeaverEngine eng(Role::Receiver);
            for (std::size_t ri = 0; ri < n_cases; ++ri)
                out0[ri] = MinorCircuit::eval(d0_all[ri], eng, pools.party0, ch);
        },
        [&](Channel& ch) {
            BeaverEngine eng(Role::Sender);
            for (std::size_t ri = 0; ri < n_cases; ++ri)
                out1[ri] = MinorCircuit::eval(d1_all[ri], eng, pools.party1, ch);
        });

    u64 bad_D = 0, bad_t = 0;
    for (std::size_t ri = 0; ri < n_cases; ++ri) {
        std::array<Fp, 4> D{};
        for (int i = 0; i < 4; ++i) D[i] = out0[ri][i].v.add(out1[ri][i].v);
        for (int i = 0; i < 4; ++i) bad_D += (D[i].v != D_expected[ri][i].v);
        u64 t_got = MinorCircuit::t_of(D);
        if (t_got != t_expected[ri]) {
            ++bad_t;
            ADD_FAILURE() << "min4 row " << ri << ": t mismatch, got " << t_got << " want "
                           << t_expected[ri];
        }
    }
    EXPECT_EQ(bad_D, 0u) << "at least one D component diverged from reference.py across "
                          << n_cases << " cases";
    EXPECT_EQ(bad_t, 0u);
}

// ---------------------------------------------------------------------------
// MIN-5 [CONCRETE] — triple-consumption count of one MinorCircuit::eval ==
// exactly 29 (20 layer-1 + 9 layer-2; static_assert-pinned in
// src/gates/minors.cpp). Also pins the total wire cost over the 2 rounds:
// 16*20 + 16*9 = 464 bytes/party (BeaverEngine's generic 16*N-bytes-per-
// mul(N) formula, pinned directly above by GatesBeaver.WireCost..., composed
// over MinorCircuit's two mul() calls -- per-layer bytes are not separately
// observable from outside the opaque eval() call).
// ---------------------------------------------------------------------------

TEST(GatesMinors, MIN5_TripleConsumptionExactly29) {
    std::mt19937_64 rng(0x550000ull);
    std::uniform_int_distribution<u64> dist(1, Fp::P - 1);
    Fp x(dist(rng));
    std::vector<Fp> d(7);
    for (int k = 0; k < 7; ++k) d[k] = x.pow(static_cast<u64>(k + 1));

    auto pools = make_dealer_pools(29, 0x550100ull);
    auto [d0, d1] = split_shares(d, 0x550200ull);

    const u64 rem0_before = pools.party0.remaining();
    const u64 rem1_before = pools.party1.remaining();

    u64 bytes0 = 0, bytes1 = 0;
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            BeaverEngine eng(Role::Receiver);
            ByteMeter meter(ch);
            (void)MinorCircuit::eval(d0, eng, pools.party0, ch);
            bytes0 = meter.bytes();
        },
        [&](Channel& ch) {
            BeaverEngine eng(Role::Sender);
            ByteMeter meter(ch);
            (void)MinorCircuit::eval(d1, eng, pools.party1, ch);
            bytes1 = meter.bytes();
        });

    EXPECT_EQ(rem0_before - pools.party0.remaining(), 29u);
    EXPECT_EQ(rem1_before - pools.party1.remaining(), 29u);
    EXPECT_EQ(bytes0, 16u * 20u + 16u * 9u);
    EXPECT_EQ(bytes1, 16u * 20u + 16u * 9u);
}

// ---------------------------------------------------------------------------
// LABELED end-to-end composition test (task-13-brief.md requirement 1): the
// ONE test in this file that generates triples through the PRODUCTION VOLE
// path (test/integration/vole_beaver.hpp -> oc::NoisyVoleSender/Receiver at
// 61 bits) rather than dealer triples, refills a real TriplePool from them,
// and runs MinorCircuit through it -- proving the production
// triple-generation path composes with the production BeaverEngine/
// MinorCircuit path. Every OTHER MIN test above deliberately uses faster
// dealer triples (task-13-brief.md requirement 1 explicitly permits this
// split).
// ---------------------------------------------------------------------------

TEST(GatesMinors, MIN_RealVoleTriplesEndToEndComposesWithMinorCircuit) {
    constexpr u64 kNeeded = 29; // one MinorCircuit::eval

    Fp x(123456789ull);
    std::vector<Fp> d(7);
    for (int k = 0; k < 7; ++k) d[k] = x.pow(static_cast<u64>(k + 1));
    auto [d0, d1] = split_shares(d, 0x760000ull);

    std::array<Share, 4> out0{}, out1{};
    run_two_party(
        next_address(),
        [&](Channel& ch) { // Receiver
            oc::PRNG ot_prng(oc::block(0x7601ull, 0));
            zt::OtPool ot_pool;
            macoro::sync_wait(zt::generate_ot_pool(
                zt::Role::Receiver, kNeeded * 2 * zvole::kOlePerCallOts, ot_prng, ch.socket(), ot_pool));

            oc::PRNG beaver_prng(oc::block(0x7602ull, 0));
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
            out0 = MinorCircuit::eval(d0, eng, pool, ch);
        },
        [&](Channel& ch) { // Sender
            oc::PRNG ot_prng(oc::block(0x7601ull, 1));
            zt::OtPool ot_pool;
            macoro::sync_wait(zt::generate_ot_pool(
                zt::Role::Sender, kNeeded * 2 * zvole::kOlePerCallOts, ot_prng, ch.socket(), ot_pool));

            oc::PRNG beaver_prng(oc::block(0x7602ull, 1));
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
            out1 = MinorCircuit::eval(d1, eng, pool, ch);
        });

    std::array<Fp, 4> D{};
    for (int i = 0; i < 4; ++i) D[i] = out0[i].v.add(out1[i].v);
    EXPECT_EQ(D[0].v, x.v);
    EXPECT_EQ(D[1].v, 0u);
    EXPECT_EQ(MinorCircuit::t_of(D), 1u);
}

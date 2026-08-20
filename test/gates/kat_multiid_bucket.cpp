// test/gates/kat_multiid_bucket.cpp — Task 25 (task-25-brief.md, the
// "PLAN-REVIEW REVISIONS" section, R1-R4): closes the project-wide hole its
// own top comment identifies -- no test anywhere else in this codebase
// exercises multiple real ids landing in the same real `BucketOracle`
// bucket, accumulated through the production `core/table.hpp`
// `PowerSumTable` (via `protocols/update.hpp` `Update::apply`), and
// rank-recovered through the real `gates/minors.hpp` `MinorCircuit`, end to
// end. Placed under test/gates/ per R2 (not test/core/): the test spans
// table construction AND the minors MPC circuit, and R2's own resolution
// puts it beside kat_minors.cpp, which already owns the two-party
// TCP/dealer-triple harness this file reuses (each KAT file in this
// project duplicates that small harness rather than sharing a header --
// same precedent as test/protocols/kat_golden_cheap.cpp).
//
// --- R6-COLLIDE-PIN / R1 (draw counts corrected in plan review) ----------
// The four ids below were found by test/campaign/find_bucket_collision.cpp
// (committed, DISABLED-by-default ctest entry
// `campaign.FindFourWayBucketCollision`), a deterministic streaming search
// (fixed start id = 1, sequential draws, no PRNG) over the REAL production
// `BucketOracle` (Params::instantiate()'s frozen all-zero-salt default, m =
// 2^31 buckets). Measured this run (see task-25-report.md for the full
// captured output): a first PAIR collision at draw 23,079 (ballpark
// matches the classic birthday-pair estimate ~5.8e4 the ORIGINAL brief
// text used, natural single-draw variance), a first TRIPLE at draw
// 1,585,015, and the first FOUR-WAY collision at draw 25,248,808 (ballpark
// matches R1's corrected ~2e7 four-way estimate) -- 7.75s wall-clock, 2 GiB
// peak memory (a flat count array, not a hashmap -- see that tool's own
// header for why). SC2 below re-verifies, on every normal run, that these
// four ids STILL collide under the CURRENT oracle -- if that ever fails,
// the oracle changed and the committed search tool is right there to
// regenerate these constants (see this file's own comment on that test).

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

#include "sympsica/core/pools.hpp"
#include "sympsica/core/state.hpp"
#include "sympsica/gates/beaver.hpp"
#include "sympsica/gates/minors.hpp"
#include "sympsica/protocols/update.hpp"
#include "sympsica/utils/field.hpp"
#include "sympsica/utils/net.hpp"
#include "sympsica/utils/params.hpp"

using namespace sympsica;

namespace {

constexpr u64 kId0 = 8330016ull;
constexpr u64 kId1 = 14151800ull;
constexpr u64 kId2 = 14581523ull;
constexpr u64 kId3 = 25248808ull;
constexpr u32 kBucket = 163929875u;
constexpr u64 kDrawsFirstPair = 23079ull;
constexpr u64 kDrawsFirstTriple = 1585015ull;
constexpr u64 kDrawsFirstQuad = 25248808ull;


// --- two-party in-process TCP harness (kat_minors.cpp / kat_golden_cheap.cpp
// precedent -- duplicated per file rather than shared) --------------------

std::atomic<int> g_port_counter{49100};

std::string next_address() { return "127.0.0.1:" + std::to_string(g_port_counter.fetch_add(1)); }

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

// --- dealer-style test triples (task-13-brief.md requirement 1: "explicitly
// permitted for correctness KATs") ----------------------------------------

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
        Fp a0(dist(rng)), b0(dist(rng)), c0(dist(rng));
        t0.push_back(Triple{i, Share{a0}, Share{b0}, Share{c0}});
        t1.push_back(Triple{i, Share{a.sub(a0)}, Share{b.sub(b0)}, Share{c.sub(c0)}});
    }
    out.party0.refill(std::move(t0));
    out.party1.refill(std::move(t1));
    return out;
}

// Test-only secret sharing (NOT a protocol primitive -- kat_minors.cpp's own
// split_shares comment): splits a plaintext Fp vector into two parties'
// Share vectors.
std::pair<std::vector<Share>, std::vector<Share>> split_fp(const std::vector<Fp>& plain,
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

// Bucket-independent expected-syndrome technique (TBL-1's own precedent,
// test/core/kat_table.cpp): brute-forces sigma(id)^1..^K directly from the
// Encoder, with NO reference to any bucket/table at all.
std::array<Fp, Params::K> brute_force_power_sums(const Encoder& enc, u64 id) {
    std::array<Fp, Params::K> v{};
    Fp acc = enc.sigma(id);
    for (u64 k = 0; k < Params::K; ++k) {
        v[k] = acc;
        acc = acc.mul(enc.sigma(id));
    }
    return v;
}

// R3 (plan-review revision): "Compute each expected syndrome independently
// as sum over R of sigma(x)^k - sum over S of sigma(x)^k." Independent of
// PowerSumTable/Update entirely -- this is the "at least one side must come
// from real production code" half of R6-NOTAUTO's own rule; the OTHER side
// (actual_d, built below) is what actually exercises production code.
std::array<Fp, Params::K> expected_syndrome(const Encoder& enc, const std::vector<u64>& recv_ids,
                                             const std::vector<u64>& send_ids) {
    std::array<Fp, Params::K> d{};
    for (u64 id : recv_ids) {
        auto v = brute_force_power_sums(enc, id);
        for (u64 k = 0; k < Params::K; ++k) d[k] = d[k].add(v[k]);
    }
    for (u64 id : send_ids) {
        auto v = brute_force_power_sums(enc, id);
        for (u64 k = 0; k < Params::K; ++k) d[k] = d[k].sub(v[k]);
    }
    return d;
}

struct MultiIdCase {
    std::string name;
    std::vector<u64> receiver_ids;
    std::vector<u64> sender_ids;
    u64 expected_t;
};

} // namespace

// ---------------------------------------------------------------------------
// SC1 [COLLIDE-FIND] is the committed search tool's own run, recorded above
// and in task-25-report.md's captured output -- not re-run here (that
// would defeat R6-COLLIDE-PIN's whole point: "so the test itself is
// instant"). SC2 below is the live, cheap re-verification.
// ---------------------------------------------------------------------------

// SC2 [COLLIDE-PIN]: the four committed ids still collide under the CURRENT
// real BucketOracle. Pure local computation -- no network, instant.
//
// FC2 [collision is real, not assumed] (task-25-report.md carries the real
// demonstration output): substituting kId0 for a non-colliding id (kId0+1)
// here and rebuilding makes this test FAIL loudly -- proving this
// assertion is not vacuously true regardless of what the ids actually are.
TEST(MultiIdBucketCollide, SC2_PinnedIdsStillCollideUnderRealOracle) {
    Params params = Params::instantiate();
    const BucketOracle& G = params.oracle;

    const u32 b0 = G.of(kId0);
    const u32 b1 = G.of(kId1);
    const u32 b2 = G.of(kId2);
    const u32 b3 = G.of(kId3);

    EXPECT_EQ(b0, kBucket) << "kId0=" << kId0 << " no longer maps to the pinned bucket";
    EXPECT_EQ(b1, kBucket) << "kId1=" << kId1 << " no longer maps to the pinned bucket";
    EXPECT_EQ(b2, kBucket) << "kId2=" << kId2 << " no longer maps to the pinned bucket";
    EXPECT_EQ(b3, kBucket) << "kId3=" << kId3 << " no longer maps to the pinned bucket";

    // Milestone sanity (informational, not a hash-uniformity claim -- plan
    // review Minor 1: a single waiting time has high variance): the
    // progression must at least be strictly increasing draw counts.
    EXPECT_LT(kDrawsFirstPair, kDrawsFirstTriple);
    EXPECT_LT(kDrawsFirstTriple, kDrawsFirstQuad);
}

// ---------------------------------------------------------------------------
// SC3 [COLLIDE-T2/T3/T4] + SC4 [COLLIDE-MIXED]: t_beta in {2,3,4} plus one
// mixed-sign (2 receiver + 1 sender) case, all in the SAME real bucket
// (kBucket), built through the PRODUCTION Update::apply -> PowerSumTable
// path, rank-recovered through the REAL MinorCircuit::eval_batch (dealer
// triples, one batched 2-round call for all four cases -- gates/minors.hpp's
// own doc comment: eval_batch stays at 2 rounds no matter how many buckets/
// cases are batched together).
//
// R3's exact signed construction: separate Receiver/Sender PartyStates,
// each storing POSITIVE power sums (production representation); syndrome =
// +table_R.row(beta) - table_S.row(beta). Cases with no sender ids get an
// all-zero table_S.row(beta) (PowerSumTable's own "absent-row semantics:
// zero row" -- table.hpp) so the same formula applies uniformly to every
// case below, not just the mixed one.
//
// FC1 [recovery non-vacuity] (task-25-report.md carries the real
// demonstration output): temporarily changing the T3 case's expected_t from
// 3 to 2 and rebuilding makes the t-assertion below FAIL loudly, naming the
// bucket and both values (actual vs want) in the failure message.
// FC3 [depth bound] (task-25-report.md carries the real demonstration
// output): temporarily changing the T4 case's expected_t from 4 to a wrong
// value (3 or 5) and rebuilding makes the same assertion fail on the T=4
// boundary case specifically -- proving recovered t is genuinely 4, not
// silently clamped/wrapped.
TEST(MultiIdBucketCollide, SC3_SC4_MultiplicityTwoThreeFourAndMixedSignRealTableRealMinorCircuit) {
    Params params_r = Params::instantiate();
    Params params_s = Params::instantiate();
    const Encoder& enc = params_r.encoder;
    const BucketOracle& G = params_r.oracle;

    // Precondition: every id used below really is in the pinned bucket
    // under the CURRENT oracle (this test does not rely on SC2 having run
    // first -- gtest does not guarantee cross-TEST() ordering).
    for (u64 id : {kId0, kId1, kId2, kId3}) {
        ASSERT_EQ(G.of(id), kBucket) << "id=" << id << " precondition: must be in the pinned bucket";
    }

    const std::vector<MultiIdCase> cases = {
        {"T2_TwoReceiverIds", {kId0, kId1}, {}, 2},
        {"T3_ThreeReceiverIds", {kId0, kId1, kId2}, {}, 3},
        {"T4_AllFourReceiverIds_TMaxBoundary", {kId0, kId1, kId2, kId3}, {}, 4},
        {"Mixed_TwoReceiverOneSender_R6COLLIDESIGNS", {kId0, kId1}, {kId2}, 3},
    };

    const std::size_t n_cases = cases.size();
    std::vector<std::array<Fp, Params::K>> d_actual(n_cases), d_expected(n_cases);

    for (std::size_t ci = 0; ci < n_cases; ++ci) {
        const MultiIdCase& c = cases[ci];
        SCOPED_TRACE(::testing::Message() << "case=" << c.name);

        // --- production path: separate Receiver/Sender PartyStates, real
        // Update::apply, real PowerSumTable. ---
        PartyState st_r, st_s;
        Update::apply(st_r, c.receiver_ids, {}, enc, G);
        Update::apply(st_s, c.sender_ids, {}, enc, G);

        auto row_r = st_r.table.row(kBucket);
        auto row_s = st_s.table.row(kBucket);
        for (u64 k = 0; k < Params::K; ++k) d_actual[ci][k] = row_r[k].sub(row_s[k]);

        // --- independent expected value (bucket-agnostic, TBL-1's own
        // technique). ---
        d_expected[ci] = expected_syndrome(enc, c.receiver_ids, c.sender_ids);

        for (u64 k = 0; k < Params::K; ++k) {
            EXPECT_EQ(d_actual[ci][k], d_expected[ci][k])
                << "bucket=" << kBucket << " case=" << c.name << " k=" << k
                << ": production table syndrome diverges from the independently "
                   "brute-forced expected value";
        }
    }

    // --- rank recovery via the REAL gates/minors.hpp MinorCircuit, dealer
    // triples, ONE batched eval_batch call for all four cases (2 rounds
    // total, regardless of batch size -- eval_batch's own doc comment). ---
    auto pools = make_dealer_pools(29 * static_cast<u64>(n_cases), 0x25000000ull);
    std::vector<std::array<Share, 7>> d0_shares(n_cases), d1_shares(n_cases);
    for (std::size_t ci = 0; ci < n_cases; ++ci) {
        std::vector<Fp> plain(d_actual[ci].begin(), d_actual[ci].end());
        auto [s0, s1] = split_fp(plain, 0x25100000ull + ci);
        for (int k = 0; k < 7; ++k) {
            d0_shares[ci][k] = s0[k];
            d1_shares[ci][k] = s1[k];
        }
    }

    std::vector<std::array<Share, 4>> D0, D1;
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            BeaverEngine eng(Role::Receiver);
            D0 = MinorCircuit::eval_batch(d0_shares, eng, pools.party0, ch);
        },
        [&](Channel& ch) {
            BeaverEngine eng(Role::Sender);
            D1 = MinorCircuit::eval_batch(d1_shares, eng, pools.party1, ch);
        });
    ASSERT_EQ(D0.size(), n_cases);
    ASSERT_EQ(D1.size(), n_cases);

    for (std::size_t ci = 0; ci < n_cases; ++ci) {
        const MultiIdCase& c = cases[ci];
        SCOPED_TRACE(::testing::Message() << "case=" << c.name);
        std::array<Fp, 4> D{};
        for (int i = 0; i < 4; ++i) D[i] = D0[ci][i].v.add(D1[ci][i].v);
        const u64 t_actual = MinorCircuit::t_of(D);
        EXPECT_EQ(t_actual, c.expected_t)
            << "bucket=" << kBucket << " case=" << c.name << ": recovered t=" << t_actual
            << " want t=" << c.expected_t;
    }
}

// test/protocols/kat_golden_cheap.cpp — Task 21's SC1/SC2 (task-21-brief.md
// W6.2, R6-GOLDCONTENT/R6-GOLDDEPTH): the CHEAP layer of the seed-fixed
// n=2^10 golden suite. All 10 seeds (0..9), default ctest set, target < 30s
// total.
//
// What "cheap" means here (R6-GOLDDEPTH): the real PowerSumTable is built
// through the PRODUCTION core/table + protocols/update path (Update::apply,
// real Params::instantiate() Encoder/BucketOracle) -- genuinely exercising
// that code, not a hand rebuild. What is AVOIDED is the full Query::run
// dispatch and Setup-produced (OT/silent-VOLE) pools (that is SC3's job,
// test/protocols_heavy/kat_golden_full.cpp, ONE seed). The per-bucket rank
// recovery below still goes through the REAL gates/minors.hpp MinorCircuit
// (eval_batch + t_of) -- the ONLY implementation of that schedule anywhere
// in this codebase -- but over DEALER-style test triples (locally sampled,
// split into shares) run through a real but throwaway in-process loopback
// Channel, exactly the "fast, explicitly permitted for correctness KATs"
// precedent test/gates/kat_minors.cpp's MIN-3/MIN-4 already established
// (task-13-brief.md requirement 1) -- NOT the OT-based Setup pools the full
// layer needs. This is what keeps ~2000 buckets/seed x 10 seeds inside the
// 30s budget: eval_batch runs the WHOLE per-seed bucket batch in exactly 2
// communication rounds, regardless of batch size (gates/minors.hpp's own
// doc comment).
//
// Golden fixture format (test/fixtures/golden_seed{0..9}.fixture, README.md
// has the full field-by-field spec): two nR=nS=1024 id sets (R/S, matching
// R-SYND's Receiver/Sender convention), plus, for every id in their
// symmetric difference, that id's per-id signed syndrome d_1..d_7 and
// recovered rank t_beta (computed in Python via Ref.syndromes/Ref.t_of --
// the SAME rank-recovery rule gates/minors.hpp's MinorCircuit::t_of
// implements, just independently, since reference.py never ports
// BucketOracle/BLAKE3 -- R-ORACLE-AGNOSTIC). The golden treats each
// symmetric-difference id as occupying its OWN "virtual bucket" (there is
// no real bucket LABEL in the file); this test is what TIES that back to
// the REAL Params oracle -- see the injectivity check below.
//
// Injectivity precondition (why this is sound, not an assumption baked
// silently into the golden): M = Params::M = 2^31, nR+nS-overlap is at
// most ~2000, so the birthday probability of ANY two DISTINCT ids in
// R union S landing in the same real BLAKE3 bucket is negligible
// (~5e-4 per seed) but not exactly zero -- so this test PROVES it for the
// actual committed ids, per seed, via the real BucketOracle, rather than
// assuming it. If this ever fails for some future re-generation of these
// fixtures with different id ranges, that is a genuine finding (a natural
// hash collision), not a bug in this test -- see this file's own top
// comment in that case.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
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

#include "../utils/fixture_support.hpp"

using namespace sympsica;

namespace {

// --- two-party in-process TCP harness (same pattern as
// test/protocols_heavy/kat_schedule100.cpp / test/gates/kat_minors.cpp) ---

std::atomic<int> g_port_counter{48100};

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

// --- dealer-style test triples (kat_minors.cpp precedent, task-13-brief.md
// requirement 1: "explicitly permitted for correctness KATs") ------------

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

std::pair<std::vector<Share>, std::vector<Share>> split_fp(const std::vector<Fp>& plain, u64 seed) {
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

// --- golden fixture parsing -------------------------------------------------

struct GoldenBucket {
    u64 id = 0;
    int sign = 0; // 0 = R-only (+), 1 = S-only (-)
    std::array<Fp, 7> d{};
    u64 t_beta = 0;
};

struct GoldenCase {
    u64 seed = 0;
    u64 overlap = 0;
    std::vector<u64> ids_r, ids_s;
    std::vector<GoldenBucket> buckets;
    u64 agg_t = 0;
    u64 expected_count = 0;
};

GoldenCase load_golden(const std::string& path) {
    sympsica_test::Fixture fx(path);
    GoldenCase c;
    c.seed = fx.u64_at("seed");
    c.overlap = fx.u64_at("golden_overlap");
    const u64 nR = fx.u64_at("golden_nR");
    const u64 nS = fx.u64_at("golden_nS");
    {
        // Fixture::one() returns the FULL row including the key at [0]
        // (unlike all(), which strips it) -- skip it explicitly.
        const auto& row = fx.one("golden_ids_r");
        EXPECT_EQ(row.size() - 1, nR) << "seed=" << c.seed;
        c.ids_r.reserve(row.size() - 1);
        for (std::size_t i = 1; i < row.size(); ++i) c.ids_r.push_back(std::stoull(row[i]));
    }
    {
        const auto& row = fx.one("golden_ids_s");
        EXPECT_EQ(row.size() - 1, nS) << "seed=" << c.seed;
        c.ids_s.reserve(row.size() - 1);
        for (std::size_t i = 1; i < row.size(); ++i) c.ids_s.push_back(std::stoull(row[i]));
    }
    const u64 bucket_count = fx.u64_at("golden_bucket_count");
    for (const auto& row : fx.all("golden_bucket")) {
        SYMPSICA_REQUIRE(row.size() == 10u, "golden_bucket row shape (id sign d1..d7 t_beta)");
        GoldenBucket b;
        b.id = std::stoull(row[0]);
        b.sign = std::stoi(row[1]);
        for (int k = 0; k < 7; ++k) b.d[k] = Fp(std::stoull(row[2 + k]));
        b.t_beta = std::stoull(row[9]);
        c.buckets.push_back(b);
    }
    EXPECT_EQ(c.buckets.size(), bucket_count) << "seed=" << c.seed;
    c.agg_t = fx.u64_at("golden_t");
    c.expected_count = fx.u64_at("golden_count");
    return c;
}

std::vector<GoldenCase> load_all_goldens() {
    std::vector<GoldenCase> out;
    for (int seed = 0; seed <= 9; ++seed) {
        out.push_back(load_golden(sympsica_test::fixture_path(
            "test/fixtures/golden_seed" + std::to_string(seed) + ".fixture")));
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// SC1/SC2 [GOLD-CHEAP]: all 10 seeds, cheap layer.
// ---------------------------------------------------------------------------
TEST(GoldenCheap, SC2_TenSeedsMatchReferencePyExactly) {
    const auto t_start = std::chrono::steady_clock::now();

    std::vector<GoldenCase> cases = load_all_goldens();
    ASSERT_EQ(cases.size(), 10u) << "SC1: all 10 seeds (0..9) must be present";

    int total_mismatches = 0;

    for (const GoldenCase& c : cases) {
        SCOPED_TRACE(::testing::Message() << "seed=" << c.seed);

        Params params_r = Params::instantiate();
        Params params_s = Params::instantiate();

        PartyState st_r, st_s;
        Update::apply(st_r, c.ids_r, {}, params_r.encoder, params_r.oracle);
        Update::apply(st_s, c.ids_s, {}, params_s.encoder, params_s.oracle);

        // --- injectivity precondition: every DISTINCT id in ids_r U ids_s
        // must map to its own real bucket under the production oracle. ---
        std::set<u64> id_union(c.ids_r.begin(), c.ids_r.end());
        id_union.insert(c.ids_s.begin(), c.ids_s.end());
        std::unordered_map<u32, u64> beta_to_id;
        for (u64 id : id_union) {
            u32 beta = params_r.oracle.of(id);
            auto [it, inserted] = beta_to_id.emplace(beta, id);
            // task-25-brief.md M-INJ (carried finding, Task 21 review):
            // EXPECT_TRUE, not ASSERT_TRUE -- this runs inside the bare
            // per-seed loop (not a SCOPED_TRACE'd sub-scope with its own
            // early-return boundary), so an ASSERT_TRUE here would abort
            // the WHOLE TEST at the first offending seed, silently skipping
            // every later seed's own diagnostics. EXPECT_TRUE still fails
            // the test (a non-fatal failure still marks the test FAILED at
            // teardown) but lets every seed report independently.
            EXPECT_TRUE(inserted) << "real BucketOracle collision: ids " << it->second << " and "
                                   << id << " both map to bucket " << beta
                                   << " -- golden's virtual-bucket-per-id assumption is violated "
                                      "for this seed; see this file's top comment";
        }

        // --- production table code: every touched real bucket must be
        // accounted for by exactly one of (a) a golden symmetric-diff
        // bucket, or (b) a net-zero shared-id bucket. ---
        std::set<u32> touched;
        for (const auto& [beta, row] : st_r.table.rows()) touched.insert(beta);
        for (const auto& [beta, row] : st_s.table.rows()) touched.insert(beta);
        ASSERT_EQ(touched.size(), id_union.size())
            << "seed=" << c.seed << ": real bucket-row count disagrees with the distinct id count "
                                     "(production table/update code touched more or fewer buckets "
                                     "than expected)";

        std::map<u32, const GoldenBucket*> beta_to_golden;
        for (const GoldenBucket& b : c.buckets) {
            u32 beta = params_r.oracle.of(b.id);
            beta_to_golden.emplace(beta, &b);
        }
        ASSERT_EQ(beta_to_golden.size(), c.buckets.size())
            << "seed=" << c.seed << ": two golden bucket ids collided under the real oracle "
                                     "(should have been caught by the injectivity check above)";

        // --- d-vector check over EVERY touched bucket, not just golden's
        // listed ones (FC2's own point: catches a bug that shifts weight
        // between buckets while preserving the aggregate). ---
        std::vector<std::array<Fp, 7>> d_batch;
        std::vector<const GoldenBucket*> d_batch_golden;
        d_batch.reserve(c.buckets.size());
        d_batch_golden.reserve(c.buckets.size());

        for (u32 beta : touched) {
            std::array<Fp, 7> row_r = st_r.table.row(beta);
            std::array<Fp, 7> row_s = st_s.table.row(beta);
            std::array<Fp, 7> d_actual{};
            for (int k = 0; k < 7; ++k) d_actual[k] = row_r[k].sub(row_s[k]);

            auto it = beta_to_golden.find(beta);
            if (it == beta_to_golden.end()) {
                // Not a symmetric-difference bucket -> must be a shared-id
                // bucket, net-zero exactly.
                bool all_zero = true;
                for (const Fp& v : d_actual) all_zero &= v.is_zero();
                EXPECT_TRUE(all_zero) << "seed=" << c.seed << " bucket=" << beta
                                       << ": expected a net-zero shared-id bucket, got a nonzero "
                                          "syndrome (production table code disagrees with the "
                                          "expected R-minus-S cancellation)";
                if (!all_zero) ++total_mismatches;
                continue;
            }
            const GoldenBucket& gb = *it->second;
            bool row_ok = (d_actual == gb.d);
            EXPECT_TRUE(row_ok) << "seed=" << c.seed << " bucket=" << beta << " id=" << gb.id
                                 << ": per-bucket syndrome mismatch";
            if (!row_ok) ++total_mismatches;
            d_batch.push_back(d_actual);
            d_batch_golden.push_back(&gb);
        }
        EXPECT_EQ(d_batch.size(), c.buckets.size()) << "seed=" << c.seed;

        // --- rank recovery via the REAL gates/minors.hpp MinorCircuit,
        // dealer triples, one batched eval_batch call (2 rounds) for the
        // WHOLE seed's bucket set. ---
        const u64 B = d_batch.size();
        auto pools = make_dealer_pools(29 * B, 0x60000000ull ^ c.seed);
        std::vector<std::array<Share, 7>> d0_shares(B), d1_shares(B);
        for (u64 b = 0; b < B; ++b) {
            std::vector<Fp> plain(d_batch[b].begin(), d_batch[b].end());
            auto [s0, s1] = split_fp(plain, 0x61000000ull ^ (c.seed * 1000003ull + b));
            for (int k = 0; k < 7; ++k) {
                d0_shares[b][k] = s0[k];
                d1_shares[b][k] = s1[k];
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
        ASSERT_EQ(D0.size(), B);
        ASSERT_EQ(D1.size(), B);

        u64 agg_t_actual = 0;
        for (u64 b = 0; b < B; ++b) {
            std::array<Fp, 4> D{};
            for (int i = 0; i < 4; ++i) D[i] = D0[b][i].v.add(D1[b][i].v);
            u64 t_actual = MinorCircuit::t_of(D);
            const GoldenBucket& gb = *d_batch_golden[b];
            bool t_ok = (t_actual == gb.t_beta);
            EXPECT_TRUE(t_ok) << "seed=" << c.seed << " bucket id=" << gb.id
                               << ": t_beta mismatch, got " << t_actual << " want " << gb.t_beta;
            if (!t_ok) ++total_mismatches;
            agg_t_actual += t_actual;
        }

        EXPECT_EQ(agg_t_actual, c.agg_t) << "seed=" << c.seed << ": aggregate t mismatch";
        ASSERT_EQ((c.ids_r.size() + c.ids_s.size() - agg_t_actual) % 2, 0u) << "seed=" << c.seed;
        const u64 count_actual = (c.ids_r.size() + c.ids_s.size() - agg_t_actual) / 2;
        EXPECT_EQ(count_actual, c.expected_count) << "seed=" << c.seed << ": derived count mismatch";
        if (agg_t_actual != c.agg_t || count_actual != c.expected_count) ++total_mismatches;
    }

    EXPECT_EQ(total_mismatches, 0) << "SC2: all 10 golden seeds must match reference.py exactly";

    const double elapsed_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    std::cerr << "[SC2-golden] 10-seed cheap layer measured runtime: " << elapsed_s << "s\n";
}

// test/protocols_heavy/kat_golden_full.cpp — Task 21's SC3 (task-21-brief.md
// W6.2, R6-GOLDDEPTH): the FULL layer of the seed-fixed n=2^10 golden suite.
// ONE seed (chosen below), replayed through the REAL two-party Query::run
// over a real TCP Channel, with Setup-PRODUCED (OT/silent-VOLE) pools --
// unlike kat_golden_cheap.cpp's dealer triples, this is the genuine
// production correlation-generation path. Registered under the `heavy`
// ctest label (this directory's whole target already carries LABELS
// "heavy" TIMEOUT 1500 -- see CMakeLists.txt's sympsica_add_test_target
// helper, protocols_heavy branch -- satisfying R6-GOLDDEPTH's "explicit
// TIMEOUT" requirement with no new CMake code needed).
//
// Seed choice: seed 9 (golden_seed9.fixture), the LARGEST configured
// overlap among the 10 committed goldens (406, from
// Ref.make_golden_pair's `overlap = 37 + 41*(seed % 10)` schedule) --
// which means the SMALLEST union-bucket count (nR + nS - overlap =
// 1024 + 1024 - 406 = 1642), minimizing this test's real-MPC cost while
// still exercising the full FullPublic path end-to-end (every bucket
// still goes through Setup-produced triples/ZtGates, real BeaverEngine,
// real SymDiffEvaluator -- the bucket COUNT only affects wall time, not
// which code paths run).
//
// firstQuery (query_no == 0 on a freshly-constructed PartyState) forces
// SwitchRule::decide's FullPublic branch unconditionally (query.hpp's own
// doc comment: "FullPublic if firstQuery OR ..."), so this test does not
// need force_full=true -- it asserts the path taken (via path_out) IS
// FullPublic, as a sanity check on that reasoning rather than a
// requirement being imposed.
//
// Pool sizing mirrors apps/party_main.cpp's own production formula
// (pool_evaluation_target: max(2*Params::U_MAX, my_size + their_size)) --
// NOT an ad hoc test-only number -- applied once via Setup::run (no
// refill_offline needed: this is a single query, firstQuery, no prior
// pool consumption).

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "sympsica/core/state.hpp"
#include "sympsica/protocols/query.hpp"
#include "sympsica/protocols/setup.hpp"
#include "sympsica/protocols/update.hpp"
#include "sympsica/utils/net.hpp"
#include "sympsica/utils/params.hpp"

#include "../utils/fixture_support.hpp"

using namespace sympsica;

namespace {

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

// Minimal golden loader: only what SC3 needs (id sets + the golden count).
// Deliberately NOT shared with test/protocols/kat_golden_cheap.cpp -- these
// are two separate gtest binaries (no shared TU), matching this codebase's
// own precedent of small per-binary duplication (kat_schedule100.cpp vs
// kat_minors.cpp each define their own run_two_party/connect helpers).
struct GoldenIdsCase {
    u64 seed = 0;
    std::vector<u64> ids_r, ids_s;
    u64 expected_count = 0;
};

GoldenIdsCase load_golden_ids(const std::string& path) {
    sympsica_test::Fixture fx(path);
    GoldenIdsCase c;
    c.seed = fx.u64_at("seed");
    const u64 nR = fx.u64_at("golden_nR");
    const u64 nS = fx.u64_at("golden_nS");
    {
        const auto& row = fx.one("golden_ids_r"); // row[0] is the key itself
        SYMPSICA_REQUIRE(row.size() - 1 == nR, "golden_ids_r count mismatch");
        c.ids_r.reserve(nR);
        for (std::size_t i = 1; i < row.size(); ++i) c.ids_r.push_back(std::stoull(row[i]));
    }
    {
        const auto& row = fx.one("golden_ids_s");
        SYMPSICA_REQUIRE(row.size() - 1 == nS, "golden_ids_s count mismatch");
        c.ids_s.reserve(nS);
        for (std::size_t i = 1; i < row.size(); ++i) c.ids_s.push_back(std::stoull(row[i]));
    }
    c.expected_count = fx.u64_at("golden_count");
    return c;
}

} // namespace

TEST(GoldenFull, SC3_OneSeedRealTwoPartyQueryMatchesGolden) {
    const auto t_start = std::chrono::steady_clock::now();

    GoldenIdsCase c =
        load_golden_ids(sympsica_test::fixture_path("test/fixtures/golden_seed9.fixture"));
    ASSERT_EQ(c.seed, 9u);
    ASSERT_EQ(c.ids_r.size(), 1024u);
    ASSERT_EQ(c.ids_s.size(), 1024u);

    Params params_r = Params::instantiate();
    Params params_s = Params::instantiate();

    PartyState st_r, st_s;
    Update::apply(st_r, c.ids_r, {}, params_r.encoder, params_r.oracle);
    Update::apply(st_s, c.ids_s, {}, params_s.encoder, params_s.oracle);
    ASSERT_EQ(st_r.my_size, c.ids_r.size());
    ASSERT_EQ(st_s.my_size, c.ids_s.size());
    ASSERT_EQ(st_r.query_no, 0u) << "firstQuery must hold for SwitchRule::decide's FullPublic branch";
    ASSERT_EQ(st_s.query_no, 0u);

    // apps/party_main.cpp's own production pool-sizing formula
    // (pool_evaluation_target), not an ad hoc test number.
    const u64 target = std::max<u64>(2 * Params::U_MAX, st_r.my_size + st_s.my_size);
    const PoolSizes sizes{44 * target, 4 * target};

    Pools pool_r, pool_s;
    run_two_party(
        "127.0.0.1:49950",
        [&](Channel& ch) { pool_r = Setup::run(Role::Receiver, ch, params_r, sizes); },
        [&](Channel& ch) { pool_s = Setup::run(Role::Sender, ch, params_s, sizes); });

    Share out_r{}, out_s{};
    SwitchRule::Path path_r = SwitchRule::Path::Incremental, path_s = SwitchRule::Path::Incremental;
    run_two_party(
        "127.0.0.1:49951",
        [&](Channel& ch) {
            out_r = Query::run(Role::Receiver, st_r, pool_r, ch, params_r,
                                "/tmp/sympsica_golden_full_r.bin", /*seed=*/0xC01D0009ull,
                                /*force_full=*/false, &path_r);
        },
        [&](Channel& ch) {
            out_s = Query::run(Role::Sender, st_s, pool_s, ch, params_s,
                                "/tmp/sympsica_golden_full_s.bin", /*seed=*/0xC01D0009ull,
                                /*force_full=*/false, &path_s);
        });

    EXPECT_EQ(path_r, SwitchRule::Path::FullPublic) << "firstQuery must force FullPublic";
    EXPECT_EQ(path_s, SwitchRule::Path::FullPublic);

    u64 count_r = 0, count_s = 0;
    run_two_party(
        "127.0.0.1:49952", [&](Channel& ch) { count_r = Query::open_count(ch, out_r); },
        [&](Channel& ch) { count_s = Query::open_count(ch, out_s); });

    EXPECT_EQ(count_r, c.expected_count) << "receiver-opened count vs golden";
    EXPECT_EQ(count_s, c.expected_count) << "sender-opened count vs golden";

    const double elapsed_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    std::cerr << "[SC3-golden-full] seed=" << c.seed << " measured runtime: " << elapsed_s << "s\n";
}

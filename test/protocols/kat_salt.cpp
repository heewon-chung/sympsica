// test/protocols/kat_salt.cpp — Task 18's formal SC/FC suite for
// protocols/salt.hpp (task-18-brief.md, W5.5/W5.6): SaltManager (due()/
// refresh()), ScheduleSync (digest()/sync()), and the OverflowChecker stub.
//
// Test IDs carried in the test names:
//   SALT1_And_SALTDET_RefreshRebuildsChangesSaltPreservesCount [SC1, SC2] —
//       two-thread, real Setup::run pools + a real TCP Channel: query
//       (count c1) -> SaltManager::refresh -> query again (count c2);
//       c1 == c2 == the true plaintext intersection size; the salt actually
//       changed; both parties' post-refresh oracles are byte-identical
//       (SC2); the table was correctly rebuilt under the fresh salt
//       (PartyState::check_against).
//   TRIGGER1_DueTable [SC3] — due()'s pure-function unit table: phi
//       divisibility (incl. the query_no==0 convention), maintenance day,
//       force flag, phi==0 guard, the none-due row, and a Params::PHI-scale
//       sanity pair.
//   STUB_OverflowCheckerDies [SC4] — OverflowChecker::check dies with the
//       documented "reference-only in PoC" message (EXPECT_DEATH).
//   FC2_StubHasNoReturnStatement — grep-style structural check (over the
//       actual salt.cpp source) that OverflowChecker::check's body contains
//       no `return` token on any code path.
//   FC1_ScheduleSyncPairDriftAborts — a schedule pair with a mismatched
//       query flag: both parties' ScheduleSync::sync() calls abort
//       (deterministically, via SYMPSICA_REQUIRE -> SIGABRT), never hang.
//   FC4_WrongOrderingDivergesOracles — constructing the salt as
//       H(r_S || r_R) instead of R-SALTX's fixed H(r_R || r_S) produces a
//       DIFFERENT oracle (different salt bytes, a probe id maps to a
//       different bucket) — the negative case that shows the ordering pin
//       is load-bearing.
//
// Harness: SC1/SC2 use REAL Setup::run pools over a real loopback TCP
// Channel pair (kat_query.cpp/kat_setup.cpp's own two-thread pattern), at
// deliberately tiny scale (|U| <= 4 buckets per query, two full-path
// queries total — the initial query plus refresh()'s own internal forced-
// full query). TRIGGER1/FC2/FC4 are pure, zero-communication unit checks.
// SC4/FC1 are EXPECT_DEATH-based; both set `death_test_style = "threadsafe"`
// (task-17-report.md's fork-after-threads lesson, restated by
// kat_ztest.cpp's TV-F3 / net_exchange.cpp's own death tests: this binary's
// OTHER tests open real coproto/asio Channels whose background threads can
// still be alive at the point a later TEST() runs, so gtest's default
// fork-based death-test style is unsafe here — "threadsafe" re-execs a
// fresh, single-threaded child instead of forking the live process).

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "sympsica/core/pools.hpp"
#include "sympsica/core/share.hpp"
#include "sympsica/core/state.hpp"
#include "sympsica/protocols/query.hpp"
#include "sympsica/protocols/salt.hpp"
#include "sympsica/protocols/setup.hpp"
#include "sympsica/utils/common.hpp"
#include "sympsica/utils/encoding.hpp"
#include "sympsica/utils/field.hpp"
#include "sympsica/utils/net.hpp"
#include "sympsica/utils/params.hpp"

#include "../utils/fixture_support.hpp"

using namespace sympsica;

namespace {

// ---------------------------------------------------------------------------
// Two-party TCP harness (kat_query.cpp / kat_setup.cpp pattern). Port base
// 49700 stays clear of every other test binary's own range (see
// kat_setup.cpp's comment for the full inventory: net_smoke 41231,
// w24_pool_gate 45231, gates' kat_minors/kat_ztest/kat_symdiff
// 47100-47499, net_exchange 48100+, kat_setup.cpp 49100+, kat_query.cpp
// 49500+) so this binary can run concurrently with them under `ctest -j`
// (the project's own SERIAL ctest convention aside, this costs nothing).
// ---------------------------------------------------------------------------

std::atomic<int> g_port_counter{49700};

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

std::string scratch_path(const std::string& name) {
    return (std::filesystem::temp_directory_path() / ("sympsica_kat_salt_" + name)).string();
}

void clear_path(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + ".tmp").c_str());
}

// Two full-path queries' worth of pool provisioning (the initial query plus
// refresh()'s own internal forced-full query, each over <= 4 buckets --
// R-SCALE: never bench-scale in a test) with comfortable margin.
constexpr PoolSizes kSaltSizes{/*triples=*/800, /*gates=*/80};

} // namespace

// ---------------------------------------------------------------------------
// SC1 + SC2: SaltManager::refresh — count preserved, salt changed, table
// rebuilt, both parties' post-refresh oracles byte-identical.
// ---------------------------------------------------------------------------

TEST(Salt, SALT1_And_SALTDET_RefreshRebuildsChangesSaltPreservesCount) {
    Params params_r = Params::instantiate();
    Params params_s = Params::instantiate();
    const std::vector<u64> A = {1, 2, 3};
    const std::vector<u64> B = {2, 3, 4};

    PartyState st_r, st_s;
    st_r.my_ids = A;
    st_r.table.init(A, params_r.encoder, params_r.oracle);
    st_r.my_size = A.size();
    st_s.my_ids = B;
    st_s.table.init(B, params_s.encoder, params_s.oracle);
    st_s.my_size = B.size();

    const std::array<u8, 32> salt_before = params_r.oracle.salt(); // all-zero, epoch 0 (Params::instantiate())

    Pools pool_r, pool_s;
    run_two_party(
        next_address(),
        [&](Channel& ch) { pool_r = Setup::run(Role::Receiver, ch, params_r, kSaltSizes); },
        [&](Channel& ch) { pool_s = Setup::run(Role::Sender, ch, params_s, kSaltSizes); });

    const std::string path_r = scratch_path("salt1_r.bin");
    const std::string path_s = scratch_path("salt1_s.bin");
    clear_path(path_r);
    clear_path(path_s);

    // Query 1 -- first query for both parties, unconditionally FullPublic.
    Share out_r1{}, out_s1{};
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            out_r1 = Query::run(Role::Receiver, st_r, pool_r, ch, params_r, path_r, /*seed=*/111);
        },
        [&](Channel& ch) {
            out_s1 = Query::run(Role::Sender, st_s, pool_s, ch, params_s, path_s, /*seed=*/222);
        });

    u64 c1_r = 0, c1_s = 0;
    run_two_party(
        next_address(), [&](Channel& ch) { c1_r = Query::open_count(ch, out_r1); },
        [&](Channel& ch) { c1_s = Query::open_count(ch, out_s1); });
    EXPECT_EQ(c1_r, 2u) << "true |A meet B| = |{2,3}| = 2";
    EXPECT_EQ(c1_s, 2u);
    ASSERT_EQ(st_r.query_no, 1u);
    ASSERT_EQ(st_s.query_no, 1u);

    // SaltManager::refresh -- the unified maintenance event: r_R/r_S
    // exchange, new oracle, table.rebuild, then a forced-full query.
    Share out_r2{}, out_s2{};
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            out_r2 = SaltManager::refresh(Role::Receiver, st_r, params_r, pool_r, ch, path_r, /*seed=*/333);
        },
        [&](Channel& ch) {
            out_s2 = SaltManager::refresh(Role::Sender, st_s, params_s, pool_s, ch, path_s, /*seed=*/444);
        });

    // --- SC2 [SALT-DET]: both parties' post-refresh oracles are
    // byte-identical (R-SALTX's fixed H(r_R || r_S) ordering). ---
    EXPECT_EQ(params_r.oracle.salt(), params_s.oracle.salt());
    const u64 probe_id = 987654321ull;
    EXPECT_EQ(params_r.oracle.of(probe_id), params_s.oracle.of(probe_id))
        << "SC2: a probe id must map to the same bucket under both parties' post-refresh oracles";

    // --- SC1: the salt actually changed. ---
    EXPECT_NE(params_r.oracle.salt(), salt_before);

    // --- SC1: the table was correctly rebuilt under the fresh salt
    // (PartyState::check_against recomputes a table from the plaintext id
    // set + fresh oracle and compares row-for-row). ---
    EXPECT_TRUE(st_r.check_against(A, params_r.encoder, params_r.oracle));
    EXPECT_TRUE(st_s.check_against(B, params_s.encoder, params_s.oracle));

    // --- SC1: count preserved across the refresh; cache/t_share coherent
    // (this can only reconstruct correctly if both are). ---
    u64 c2_r = 0, c2_s = 0;
    run_two_party(
        next_address(), [&](Channel& ch) { c2_r = Query::open_count(ch, out_r2); },
        [&](Channel& ch) { c2_s = Query::open_count(ch, out_s2); });
    EXPECT_EQ(c2_r, 2u);
    EXPECT_EQ(c2_s, 2u);

    // J cleared, query_no advanced by exactly one more commit (refresh's
    // own internal Query::run commit).
    EXPECT_TRUE(st_r.J.empty());
    EXPECT_TRUE(st_s.J.empty());
    EXPECT_EQ(st_r.query_no, 2u);
    EXPECT_EQ(st_s.query_no, 2u);

    clear_path(path_r);
    clear_path(path_s);
}

// ---------------------------------------------------------------------------
// SC3 [TRIGGER]: due()'s pure-function unit table.
// ---------------------------------------------------------------------------

TEST(Salt, TRIGGER1_DueTable) {
    // phi divisibility, INCLUDING the query_no==0 convention (documented in
    // salt.hpp: never due via the phi branch at query_no==0).
    EXPECT_FALSE(SaltManager::due(0, 8, false, false));
    EXPECT_FALSE(SaltManager::due(1, 8, false, false));
    EXPECT_FALSE(SaltManager::due(7, 8, false, false));
    EXPECT_TRUE(SaltManager::due(8, 8, false, false));
    EXPECT_TRUE(SaltManager::due(16, 8, false, false));
    EXPECT_FALSE(SaltManager::due(9, 8, false, false));

    // maintenance day -> due, unconditional OR (independent of phi/query_no).
    EXPECT_TRUE(SaltManager::due(0, 8, /*maintenance_day=*/true, false));
    EXPECT_TRUE(SaltManager::due(3, 1000, /*maintenance_day=*/true, false));

    // force -> due, unconditional OR.
    EXPECT_TRUE(SaltManager::due(0, 8, false, /*force=*/true));
    EXPECT_TRUE(SaltManager::due(5, 1000, false, /*force=*/true));

    // phi == 0 guarded (no division; never due via the phi branch).
    EXPECT_FALSE(SaltManager::due(8, 0, false, false));
    EXPECT_FALSE(SaltManager::due(1000, 0, false, false));

    // the none-due row.
    EXPECT_FALSE(SaltManager::due(5, 8, false, false));

    // Params::PHI-scale sanity pair (query-side integration uses this
    // constant directly, per R-TRIGGER).
    EXPECT_TRUE(SaltManager::due(Params::PHI, Params::PHI, false, false));
    EXPECT_FALSE(SaltManager::due(Params::PHI - 1, Params::PHI, false, false));
}

// ---------------------------------------------------------------------------
// SC4 [STUB] / FC2 [stub non-return]: OverflowChecker::check dies with the
// documented message; its body contains no return statement on any path.
// ---------------------------------------------------------------------------

TEST(Salt, STUB_OverflowCheckerDies) {
    ::testing::GTEST_FLAG(death_test_style) = "threadsafe";
    PartyState st;
    EXPECT_DEATH({ (void)OverflowChecker::check(st, 7); }, "reference-only in PoC");
}

TEST(Salt, FC2_StubHasNoReturnStatement) {
    const std::string path = sympsica_test::fixture_path("src/protocols/salt.cpp");
    std::ifstream f(path, std::ios::binary);
    ASSERT_TRUE(f.is_open()) << "could not open " << path;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    const std::string needle = "bool OverflowChecker::check(";
    const std::size_t start = content.find(needle);
    ASSERT_NE(start, std::string::npos) << "OverflowChecker::check's definition not found in salt.cpp";
    const std::size_t body_start = content.find('{', start);
    ASSERT_NE(body_start, std::string::npos);

    int depth = 0;
    std::size_t i = body_start;
    for (; i < content.size(); ++i) {
        if (content[i] == '{') {
            ++depth;
        } else if (content[i] == '}') {
            --depth;
            if (depth == 0) break;
        }
    }
    ASSERT_LT(i, content.size()) << "could not find OverflowChecker::check's closing brace";

    const std::string body = content.substr(body_start, i - body_start + 1);
    EXPECT_EQ(body.find("return"), std::string::npos)
        << "FC2: OverflowChecker::check's body must contain no `return` token on any code path -- "
           "the stub must DIE, never return a value";
}

// ---------------------------------------------------------------------------
// FC1 [pair drift]: a schedule pair with a mismatched query flag ->
// ScheduleSync::sync() aborts deterministically on both sides, never hangs.
// ---------------------------------------------------------------------------

TEST(Salt, FC1_ScheduleSyncPairDriftAborts) {
    ::testing::GTEST_FLAG(death_test_style) = "threadsafe";
    EXPECT_DEATH(
        {
            run_two_party(
                next_address(),
                [&](Channel& ch) {
                    ScheduleSync::sync(ch, "2023-01-05", /*query=*/true, /*maintenance=*/false);
                },
                [&](Channel& ch) {
                    ScheduleSync::sync(ch, "2023-01-05", /*query=*/false, /*maintenance=*/false);
                });
        },
        "PAIR DRIFT");
}

// Positive companion (not a named SC/FC, but pins that agreeing markers do
// NOT abort -- otherwise FC1 above would be vacuous, always dying for some
// unrelated reason).
TEST(Salt, ScheduleSyncAgreeingMarkersSucceed) {
    EXPECT_NO_FATAL_FAILURE(run_two_party(
        next_address(),
        [&](Channel& ch) { ScheduleSync::sync(ch, "2023-01-05", /*query=*/true, /*maintenance=*/false); },
        [&](Channel& ch) { ScheduleSync::sync(ch, "2023-01-05", /*query=*/true, /*maintenance=*/false); }));
}

// ---------------------------------------------------------------------------
// FC4 [salt asymmetry]: constructing the salt as H(r_S || r_R) (the wrong
// order) instead of R-SALTX's fixed H(r_R || r_S) produces a DIFFERENT
// oracle. Exercised directly at the BucketOracle::refreshed level (both
// orderings are equally valid CALLS -- the correctness requirement is a
// PROTOCOL convention, not something refreshed() itself enforces), which is
// exactly the point: nothing stops a wrong per-party ordering choice except
// both parties AGREEING on R-SALTX's fixed convention, and this test shows
// what breaks when they don't.
// ---------------------------------------------------------------------------

TEST(Salt, FC4_WrongOrderingDivergesOracles) {
    std::array<u8, 32> r_receiver{}, r_sender{};
    for (int i = 0; i < 32; ++i) {
        r_receiver[static_cast<std::size_t>(i)] = static_cast<u8>(i + 1);
        r_sender[static_cast<std::size_t>(i)] = static_cast<u8>(200 + i);
    }

    const BucketOracle correct = BucketOracle::refreshed(r_receiver, r_sender); // R-SALTX: H(r_R || r_S)
    const BucketOracle wrong = BucketOracle::refreshed(r_sender, r_receiver);   // wrong: H(r_S || r_R)

    EXPECT_NE(correct.salt(), wrong.salt());

    const u64 probe_id = 424242ull;
    EXPECT_NE(correct.of(probe_id), wrong.of(probe_id))
        << "FC4: a probe id must map to a DIFFERENT bucket under the two orderings";
}

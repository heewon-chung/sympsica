// test/protocols/kat_pool_audit.cpp — Task 23 (W6.4 + W6.6(ii)): the
// full-transcript id-based single-use pool audit (W3.4 semantics,
// R6-AUDIT-TRANSCRIPT), UNIT-SCALE leg (SC1). Three of R6-AUDIT-TRANSCRIPT's
// four properties are independently checked here; the fourth (global
// uniqueness across refills) is enforced as a PRECONDITION by
// CorrelationPool<T>::refill itself (src/core/pools.cpp) and this audit
// RELIES ON that enforcement rather than re-deriving it -- see
// test/utils/audit_pool_transcript.hpp's own header for exactly why a
// post-hoc audit built on generated()/consumed_ids()/remaining() cannot
// independently re-verify it. The E2E-scale leg (SC2) runs the SAME checks
// against REAL party_main subprocesses via
// test/e2e/run_e2e_gate.py (apps/party_main.cpp's --audit-out flag) -- this
// file is the in-process, small-scale companion, plus:
//   - FC1/FC2/FC4's non-vacuity demonstrations for the audit helper itself
//     (test/utils/audit_pool_transcript.hpp);
//   - FC3's non-vacuity demonstration for R6-CGB-PROMOTE's "PkOpCounter
//     snapshot == final value" claim-B gate assertion -- the SAME shape
//     test/e2e/run_e2e_gate.py now asserts at E2E scale. Reaching the
//     production entry point this needs (protocols/detail/setup_ot.hpp's
//     run_base_ots) from the E2E driver is impractical (no CLI hook), so
//     this in-process demonstration, on the SAME production entry point
//     Task 16's own kat_setup.cpp FC3 already established for this exact
//     purpose, stands in for it -- see task-23-report.md.
//
// The audit primitives themselves (CorrelationPool<T>::generated/
// consumed_ids/remaining, Pools::next_corr_id) already exist
// (core/pools.hpp, protocols/setup.hpp) -- this task adds NO pool
// machinery, only the audit assertions on top of them
// (test/utils/audit_pool_transcript.hpp).
//
// Test IDs:
//   SC1_FullTranscriptAuditPassesAfterQueryRefillQuery [SC1] -- Setup::run,
//     a query day, a local Update::apply, a refill_offline (between
//     queries), a second query day; audit all four pools (triples/gates x
//     receiver/sender) at the end, print the numbers.
//   FC1_DoubleConsumeOnTranscriptConsumedIdAborts [FC1] -- take_by_id() a
//     SECOND time on an id the SAME transcript above already consumed
//     aborts (TV-F10's pattern, now driven from inside a real multi-day
//     transcript's own pool object rather than a fresh isolated one).
//   FC2_ReconciliationCanFail [FC2] -- audit_pool_transcript() fed a
//     deliberately-miscounted fixture (generated != consumed+remaining)
//     dies naming the offending pool and the three numbers; the SAME
//     shape restored to correct numbers then passes.
//   FC3_CGBSnapshotEqualsFinalAssertionCanFail [FC3] -- the exact
//     snapshot==final shape run_e2e_gate.py asserts at E2E scale, shown
//     PASSING after an ordinary refill_offline and FAILING (not equal)
//     after a real second base-OT execution (Task 16's FC3 hook).
//   FC4_HighWaterUnconsumedItemsAreLegal [FC4] -- over-provisioning a pool
//     far beyond what gets consumed must NOT fail the audit.

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

// macoro/sync_wait.h uses std::source_location/basic_traceable but does not
// include macoro/trace.h itself -- every file in this project that drives
// macoro::sync_wait over a real socket hits the same transitive-include gap
// (test/integration/w24_pool_gate.cpp's header comment has the full
// rationale); supplied directly here for FC3's detail::run_base_ots call.
#include "macoro/trace.h"
#include "macoro/sync_wait.h"

#include "sympsica/core/state.hpp"
#include "sympsica/protocols/detail/setup_ot.hpp"
#include "sympsica/protocols/query.hpp"
#include "sympsica/protocols/setup.hpp"
#include "sympsica/protocols/update.hpp"
#include "sympsica/utils/net.hpp"
#include "sympsica/utils/params.hpp"

#include "../utils/audit_pool_transcript.hpp"

using namespace sympsica;

namespace {

// Port range 49970+ -- this file's own dedicated block, distinct from every
// other file sharing this binary's `protocols` target (kat_setup.cpp
// 49100+, kat_query.cpp 49500+, kat_salt.cpp 49700+, kat_crash_invariant.cpp
// 49960-49963 fixed).
std::atomic<int> g_port_counter{49970};

std::string next_address() {
    return "127.0.0.1:" + std::to_string(g_port_counter.fetch_add(1));
}

// mkdtemp-based scoped tmp dir (kat_clmb.cpp's M3 pattern): two concurrent
// runs of this binary -- or a stale file left behind by a killed run --
// must never collide on a fixed path.
std::filesystem::path make_unique_tmp_dir(const std::string& tag) {
    const std::string tmpl = (std::filesystem::temp_directory_path() / (tag + "_XXXXXX")).string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* dir = ::mkdtemp(buf.data());
    if (dir == nullptr) {
        throw std::runtime_error("mkdtemp failed for template '" + tmpl + "': " + std::strerror(errno));
    }
    return std::filesystem::path(dir);
}

struct TmpDirGuard {
    std::filesystem::path dir;
    explicit TmpDirGuard(const std::string& tag) : dir(make_unique_tmp_dir(tag)) {}
    ~TmpDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

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

// Pool sizes throughout (R-SCALE: bench-scale sizes must never run in these
// unit-scale tests -- SC2/the E2E driver is where the real n=2^10 scale
// audit lives). NOT the same small sizes kat_clmb.cpp's OWN Setup::run
// call uses (200/20): unlike kat_clmb.cpp, which performs 20
// refill_offline calls BEFORE its first query (ending far above 200/20),
// this file's Query::run calls run directly off the INITIAL Setup::run
// pool with no refill first -- so it must survive R-EXHAUST
// (src/protocols/query.cpp's eval_union guard: `triples.remaining() >=
// 44*|betas|`, `gates.remaining() >= 4*|betas|`) on its own. betas is the
// union of both parties' `min(my_size, M)`-sized supports (query.cpp's
// run_full), which for this file's <= 7-id sets is comfortably <= 14, so
// 2000/200 leaves wide margin (observed, not assumed -- see
// task-23-report.md for the FIRST attempt at 200/20, which reproduced
// R-EXHAUST for real and is why this is 2000/200 instead).
constexpr PoolSizes kSmallSizes{/*triples=*/2000, /*gates=*/200};
constexpr PoolSizes kRefillTarget{/*triples=*/4000, /*gates=*/400};

struct Transcript {
    Pools pool_r, pool_s;
};

// Builds a genuine multi-query in-process transcript on production code
// (Setup::run/Query::run/Update::apply/Setup::refill_offline, exactly the
// same call sequence apps/party_main.cpp's per-day loop uses): Setup::run,
// a query day, a LOCAL Update::apply (no communication), a refill_offline
// (R6-AUDIT-TRANSCRIPT's "at least one refill between queries"), then a
// second query day.
Transcript run_full_transcript(const TmpDirGuard& tmp_guard) {
    const std::string state_r = (tmp_guard.dir / "r.bin").string();
    const std::string state_s = (tmp_guard.dir / "s.bin").string();

    Params params_r = Params::instantiate();
    Params params_s = Params::instantiate();

    Transcript t;
    run_two_party(
        next_address(),
        [&](Channel& ch) { t.pool_r = Setup::run(Role::Receiver, ch, params_r, kSmallSizes); },
        [&](Channel& ch) { t.pool_s = Setup::run(Role::Sender, ch, params_s, kSmallSizes); });

    PartyState st_r, st_s;
    st_r.my_ids = {1, 2, 3, 4, 5};
    st_r.table.init(st_r.my_ids, params_r.encoder, params_r.oracle);
    st_r.my_size = 5;
    st_s.my_ids = {3, 4, 5, 6, 7};
    st_s.table.init(st_s.my_ids, params_s.encoder, params_s.oracle);
    st_s.my_size = 5;

    // Query day 1.
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            (void)Query::run(Role::Receiver, st_r, t.pool_r, ch, params_r, state_r, /*seed=*/8001);
        },
        [&](Channel& ch) {
            (void)Query::run(Role::Sender, st_s, t.pool_s, ch, params_s, state_s, /*seed=*/8002);
        });

    // Local update, zero communication.
    Update::apply(st_r, std::vector<u64>{8, 9}, std::vector<u64>{1}, params_r.encoder, params_r.oracle);
    Update::apply(st_s, std::vector<u64>{10}, std::vector<u64>{}, params_s.encoder, params_s.oracle);

    // Refill BETWEEN queries (R6-AUDIT-TRANSCRIPT).
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            Setup::refill_offline(t.pool_r, Role::Receiver, ch, params_r, kRefillTarget);
        },
        [&](Channel& ch) {
            Setup::refill_offline(t.pool_s, Role::Sender, ch, params_s, kRefillTarget);
        });

    // Query day 2.
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            (void)Query::run(Role::Receiver, st_r, t.pool_r, ch, params_r, state_r, /*seed=*/8003);
        },
        [&](Channel& ch) {
            (void)Query::run(Role::Sender, st_s, t.pool_s, ch, params_s, state_s, /*seed=*/8004);
        });

    return t;
}

} // namespace

// ---------------------------------------------------------------------------
// SC1 [AUDIT-UNIT]: R6-AUDIT-TRANSCRIPT's three independently-checked
// properties (reconciliation, consumed-side single-use, no-id-from-nowhere)
// for both pools, on both parties, after the multi-query transcript above
// -- plus, implicitly, property 4 (cross-refill global uniqueness), which
// this SAME transcript's refill_offline call has already relied on
// CorrelationPool::refill to enforce as a precondition (see
// audit_pool_transcript.hpp's header).
// ---------------------------------------------------------------------------

TEST(PoolAudit, SC1_FullTranscriptAuditPassesAfterQueryRefillQuery) {
    TmpDirGuard tmp_guard("sympsica_pool_audit_sc1");
    Transcript t = run_full_transcript(tmp_guard);

    sympsica::test::audit_pool_transcript("triples_r", t.pool_r.triples.generated(),
                                           t.pool_r.triples.consumed_ids(), t.pool_r.triples.remaining(),
                                           t.pool_r.next_corr_id);
    sympsica::test::audit_pool_transcript("gates_r", t.pool_r.gates.generated(),
                                           t.pool_r.gates.consumed_ids(), t.pool_r.gates.remaining(),
                                           t.pool_r.next_corr_id);
    sympsica::test::audit_pool_transcript("triples_s", t.pool_s.triples.generated(),
                                           t.pool_s.triples.consumed_ids(), t.pool_s.triples.remaining(),
                                           t.pool_s.next_corr_id);
    sympsica::test::audit_pool_transcript("gates_s", t.pool_s.gates.generated(),
                                           t.pool_s.gates.consumed_ids(), t.pool_s.gates.remaining(),
                                           t.pool_s.next_corr_id);

    // Machine-parseable summary for task-23-report.md / docs/kat_results.md
    // (kat_setup.cpp's SETUP6 json-printf convention).
    std::printf(
        "{\"sc1_pool_audit\":{"
        "\"triples_r\":{\"generated\":%llu,\"consumed\":%zu,\"remaining\":%llu,\"next_corr_id\":%llu},"
        "\"gates_r\":{\"generated\":%llu,\"consumed\":%zu,\"remaining\":%llu,\"next_corr_id\":%llu},"
        "\"triples_s\":{\"generated\":%llu,\"consumed\":%zu,\"remaining\":%llu,\"next_corr_id\":%llu},"
        "\"gates_s\":{\"generated\":%llu,\"consumed\":%zu,\"remaining\":%llu,\"next_corr_id\":%llu}"
        "}}\n",
        (unsigned long long)t.pool_r.triples.generated(), t.pool_r.triples.consumed_ids().size(),
        (unsigned long long)t.pool_r.triples.remaining(), (unsigned long long)t.pool_r.next_corr_id,
        (unsigned long long)t.pool_r.gates.generated(), t.pool_r.gates.consumed_ids().size(),
        (unsigned long long)t.pool_r.gates.remaining(), (unsigned long long)t.pool_r.next_corr_id,
        (unsigned long long)t.pool_s.triples.generated(), t.pool_s.triples.consumed_ids().size(),
        (unsigned long long)t.pool_s.triples.remaining(), (unsigned long long)t.pool_s.next_corr_id,
        (unsigned long long)t.pool_s.gates.generated(), t.pool_s.gates.consumed_ids().size(),
        (unsigned long long)t.pool_s.gates.remaining(), (unsigned long long)t.pool_s.next_corr_id);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// FC1 [double-consume, in a real transcript's own pool]: take_by_id() a
// SECOND time on an id the transcript above already consumed aborts.
// ---------------------------------------------------------------------------

TEST(PoolAuditDeathTest, FC1_DoubleConsumeOnTranscriptConsumedIdAborts) {
    TmpDirGuard tmp_guard("sympsica_pool_audit_fc1");
    Transcript t = run_full_transcript(tmp_guard);

    ASSERT_FALSE(t.pool_r.triples.consumed_ids().empty())
        << "the transcript must have consumed at least one triple for this negative to mean anything";
    const u64 already_consumed_id = t.pool_r.triples.consumed_ids().front();

    // EXPECT_DEATH forks a child process for the statement below; the
    // parent's t.pool_r is untouched by the child's abort, so this can
    // safely run AFTER the transcript (and could equally run before SC1's
    // own audit call, which is a separate TEST() invocation entirely).
    EXPECT_DEATH({ t.pool_r.triples.take_by_id(already_consumed_id); }, "already consumed");
}

// ---------------------------------------------------------------------------
// FC2 [reconciliation non-vacuity]: audit_pool_transcript() fed a
// deliberately-miscounted fixture dies naming the offending pool and the
// three numbers; the SAME shape with correct numbers then passes.
// ---------------------------------------------------------------------------

TEST(PoolAuditDeathTest, FC2_ReconciliationCanFail) {
    // Deliberately wrong: 3 consumed + 50 remaining = 53 != 100 generated.
    const std::vector<u64> consumed{10, 20, 30};
    EXPECT_DEATH(
        { sympsica::test::audit_pool_transcript("fixture_pool", /*generated=*/100, consumed,
                                                 /*remaining=*/50, /*next_corr_id=*/1000); },
        "reconciliation failed");

    // Restored: the SAME consumed set, generated corrected to 53. Not
    // wrapped in EXPECT_DEATH -- audit_pool_transcript's failure path is
    // SYMPSICA_REQUIRE (std::abort(), not a gtest fatal failure), so
    // "reaching the end of this test" IS the pass signal for the restored
    // case; this line itself is the "restore, record both" half.
    sympsica::test::audit_pool_transcript("fixture_pool", /*generated=*/53, consumed, /*remaining=*/50,
                                           /*next_corr_id=*/1000);
}

// ---------------------------------------------------------------------------
// FC3 [CG-B non-vacuity]: the exact snapshot==final shape
// test/e2e/run_e2e_gate.py asserts at E2E scale (R6-CGB-PROMOTE), shown
// PASSING after an ordinary refill_offline (R-PKOP: never touches
// PkOpCounter) and FAILING (not equal) after a real second base-OT
// execution via Task 16's own FC3 hook (protocols/detail/setup_ot.hpp's
// run_base_ots, exposed at namespace scope specifically so a SECOND call
// can be driven directly, on a throwaway SetupOtState, unrelated to any
// Pools).
// ---------------------------------------------------------------------------

TEST(PoolAudit, FC3_CGBSnapshotEqualsFinalAssertionCanFail) {
    Params params_r = Params::instantiate();
    Params params_s = Params::instantiate();

    Pools pool_r, pool_s;
    run_two_party(
        next_address(),
        [&](Channel& ch) { pool_r = Setup::run(Role::Receiver, ch, params_r, kSmallSizes); },
        [&](Channel& ch) { pool_s = Setup::run(Role::Sender, ch, params_s, kSmallSizes); });

    const u64 snapshot = PkOpCounter::value();
    ASSERT_GT(snapshot, 0u) << "SC4-style non-vacuity precondition: snapshot must be nonzero";

    // Positive control: this IS the exact assertion run_e2e_gate.py makes
    // at E2E scale (R6-CGB-PROMOTE) -- an ordinary refill_offline call must
    // leave PkOpCounter untouched.
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            Setup::refill_offline(pool_r, Role::Receiver, ch, params_r, kRefillTarget);
        },
        [&](Channel& ch) {
            Setup::refill_offline(pool_s, Role::Sender, ch, params_s, kRefillTarget);
        });
    const u64 final_after_refill = PkOpCounter::value();
    EXPECT_EQ(final_after_refill, snapshot)
        << "positive control (snapshot=" << snapshot << " final=" << final_after_refill << ")";

    // Negative demonstration: a real SECOND base-OT execution DOES move
    // PkOpCounter away from the post-Setup snapshot -- proving the
    // snapshot==final assertion is capable of firing, not vacuously true.
    detail::SetupOtState throwaway_r, throwaway_s;
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            macoro::sync_wait(detail::run_base_ots(Role::Receiver, throwaway_r, ch.socket()));
        },
        [&](Channel& ch) {
            macoro::sync_wait(detail::run_base_ots(Role::Sender, throwaway_s, ch.socket()));
        });
    const u64 final_after_second_base_ot = PkOpCounter::value();
    EXPECT_NE(final_after_second_base_ot, snapshot)
        << "FC3: a genuine second base-OT execution must move PkOpCounter away from the "
           "post-Setup snapshot (snapshot="
        << snapshot << " final=" << final_after_second_base_ot
        << ") -- proving R6-CGB-PROMOTE's snapshot==final assertion is not vacuous";
}

// ---------------------------------------------------------------------------
// FC4 [high-water is legal]: over-provisioning a pool far beyond what gets
// consumed must NOT fail the audit.
// ---------------------------------------------------------------------------

TEST(PoolAudit, FC4_HighWaterUnconsumedItemsAreLegal) {
    Params params_r = Params::instantiate();
    Params params_s = Params::instantiate();

    Pools pool_r, pool_s;
    run_two_party(
        next_address(),
        [&](Channel& ch) { pool_r = Setup::run(Role::Receiver, ch, params_r, kSmallSizes); },
        [&](Channel& ch) { pool_s = Setup::run(Role::Sender, ch, params_s, kSmallSizes); });

    // Over-provision far beyond anything consumed below (deliberately
    // wasteful, on purpose -- this is exactly the shape FC4 must accept).
    const PoolSizes over_target{/*triples=*/kSmallSizes.triples + 500, /*gates=*/kSmallSizes.gates + 100};
    run_two_party(
        next_address(),
        [&](Channel& ch) { Setup::refill_offline(pool_r, Role::Receiver, ch, params_r, over_target); },
        [&](Channel& ch) { Setup::refill_offline(pool_s, Role::Sender, ch, params_s, over_target); });

    // Consume just a couple of items -- everything else stays unconsumed
    // high-water stock.
    (void)pool_r.triples.take();
    (void)pool_r.gates.take();
    (void)pool_s.triples.take();
    (void)pool_s.gates.take();

    ASSERT_GT(pool_r.triples.remaining(), 400u) << "this test's own point requires substantial headroom";

    // Not wrapped in EXPECT_DEATH -- audit_pool_transcript's failure path is
    // SYMPSICA_REQUIRE (std::abort()), so "this test finishes at all" IS the
    // pass signal: FC4's whole point is that the large `remaining` count
    // above must NOT trip the audit.
    sympsica::test::audit_pool_transcript("triples_r", pool_r.triples.generated(),
                                           pool_r.triples.consumed_ids(), pool_r.triples.remaining(),
                                           pool_r.next_corr_id);
    sympsica::test::audit_pool_transcript("gates_r", pool_r.gates.generated(), pool_r.gates.consumed_ids(),
                                           pool_r.gates.remaining(), pool_r.next_corr_id);

    std::printf("{\"fc4_high_water\":{\"triples_remaining\":%zu,\"gates_remaining\":%zu}}\n",
                pool_r.triples.remaining(), pool_r.gates.remaining());
    std::fflush(stdout);
}

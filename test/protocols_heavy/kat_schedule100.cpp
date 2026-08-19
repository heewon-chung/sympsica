// test/protocols_heavy/kat_schedule100.cpp — Task 19's SC1 (task-19-brief.md
// W5.8, R-SCALE19 SMALL scale): randomized day-schedules replayed
// IN-PROCESS against a real two-party Channel, comparing every query day's
// reconstructed count against ref/reference.py's Ref.simulate_days()
// golden.
//
// R-SCALE19 (binding): "n in [32,128] ids per party, 4-8 days each, >= 2
// query days ... In-process two-thread execution is ACCEPTABLE and
// preferred here ... Whole 100-seed sweep must fit in ~180s Release ... If
// a leg cannot fit its budget, REDUCE THE COUNT (fewer seeds), never the
// assertion strength, and say exactly what you reduced and why."
//
// COVERAGE REDUCTION (documented per R-SCALE19's own requirement): the
// fixture below carries seeds 0..34 (35 schedules), NOT the plan's literal
// 100. Empirically measured (task-19-report.md has the full calibration
// log): after a one-time ~11s Setup::run cost, each schedule at this scale
// costs ~4.3s steady-state (dominated by per-query-day refill_offline/
// gate-generation round trips, not by the raw triple/gate count itself,
// which is tiny at n in [32,128]) -- 100 schedules would cost ~440s, well
// over the ~180s Release budget; 35 schedules costs ~155-160s, comfortably
// inside it. The assertion strength is UNCHANGED (35/35 must still match
// reference.py exactly at every query day); only the seed COUNT was
// reduced, per R-SCALE19's explicit permission.
//
// At this scale nA+nB never approaches 2*Params::U_MAX, so SwitchRule::
// decide always takes the FullPublic/FullAnnounced branch (never
// Incremental, which would unconditionally pad to a fixed 2*u_max
// regardless of scale) -- this is what keeps the sweep small-cost in the
// first place. ONE Setup::run (ONE base-OT setup, R-PKOP) is shared across
// the WHOLE sweep, inside a single long-lived two-thread session, with
// refill_offline topping the pools up before each query day.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "sympsica/core/pools.hpp"
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

// --- fixture parsing -------------------------------------------------------

struct DayRow {
    std::string day;
    bool query = false;
    std::vector<u64> insert_r, delete_r, insert_s, delete_s;
};

struct SchedCase {
    u64 seed = 0;
    u64 n_days = 0;
    u64 n_queries = 0;
    std::vector<DayRow> days;
    std::vector<u64> expected; // one per query day
};

std::vector<u64> parse_ids(const std::vector<std::string>& row, std::size_t& i) {
    u64 count = std::stoull(row.at(i++));
    std::vector<u64> out;
    out.reserve(count);
    for (u64 k = 0; k < count; ++k) out.push_back(std::stoull(row.at(i++)));
    return out;
}

std::vector<SchedCase> load_sched100(const std::string& path) {
    sympsica_test::Fixture fx(path);
    std::map<u64, SchedCase> by_seed;
    for (const auto& row : fx.all("sched")) {
        SchedCase c;
        c.seed = std::stoull(row.at(0));
        c.n_days = std::stoull(row.at(1));
        c.n_queries = std::stoull(row.at(2));
        by_seed[c.seed] = std::move(c);
    }
    for (const auto& row : fx.all("day")) {
        std::size_t i = 0;
        u64 seed = std::stoull(row.at(i++));
        DayRow d;
        d.day = row.at(i++);
        d.query = row.at(i++) == "1";
        d.insert_r = parse_ids(row, i);
        d.delete_r = parse_ids(row, i);
        d.insert_s = parse_ids(row, i);
        d.delete_s = parse_ids(row, i);
        by_seed.at(seed).days.push_back(std::move(d));
    }
    for (const auto& row : fx.all("expected")) {
        u64 seed = std::stoull(row.at(0));
        auto& c = by_seed.at(seed);
        for (std::size_t k = 1; k < row.size(); ++k) c.expected.push_back(std::stoull(row.at(k)));
    }
    std::vector<SchedCase> out;
    out.reserve(by_seed.size());
    for (auto& [seed, c] : by_seed) {
        EXPECT_EQ(c.days.size(), c.n_days) << "seed=" << seed;
        EXPECT_EQ(c.expected.size(), c.n_queries) << "seed=" << seed;
        out.push_back(std::move(c));
    }
    return out;
}

// A generous-but-modest per-query top-up target: n in [32,128] per party
// across up to 8 days means a query-day union is comfortably under a few
// hundred buckets; 400 buckets' worth of margin covers every SC1 case with
// room to spare while staying far below Incremental's fixed 2*u_max cost.
constexpr u64 kQueryTopUpBuckets = 400;

} // namespace

TEST(Schedule100, SC1_ThirtyFiveRandomSchedulesMatchReferencePy) {
    const auto t_start = std::chrono::steady_clock::now();

    std::vector<SchedCase> cases =
        load_sched100(sympsica_test::fixture_path("test/fixtures/sched100.fixture"));
    ASSERT_EQ(cases.size(), 35u) << "SC1 fixture must carry the reduced 35-seed sweep (0..34) -- "
                                     "see this file's top comment for the R-SCALE19 justification";

    Params params_r = Params::instantiate();
    Params params_s = Params::instantiate();

    // ONE shared Setup::run for the whole 100-schedule sweep (R-SCALE19's
    // amortization rationale, this file's top comment).
    Pools pool_r, pool_s;
    run_two_party(
        "127.0.0.1:49900",
        [&](Channel& ch) { pool_r = Setup::run(Role::Receiver, ch, params_r, PoolSizes{20000, 2000}); },
        [&](Channel& ch) { pool_s = Setup::run(Role::Sender, ch, params_s, PoolSizes{20000, 2000}); });

    std::vector<std::vector<u64>> observed_r(cases.size()), observed_s(cases.size());

    run_two_party(
        "127.0.0.1:49901",
        [&](Channel& ch) {
            for (std::size_t i = 0; i < cases.size(); ++i) {
                const SchedCase& c = cases[i];
                PartyState st;
                u64 seed_arg = 0x53430000ull + c.seed;
                for (const DayRow& d : c.days) {
                    Update::apply(st, d.insert_r, d.delete_r, params_r.encoder, params_r.oracle);
                    if (d.query) {
                        Setup::refill_offline(pool_r, Role::Receiver, ch, params_r,
                                               PoolSizes{44 * kQueryTopUpBuckets, 4 * kQueryTopUpBuckets});
                        Share out = Query::run(Role::Receiver, st, pool_r, ch, params_r,
                                                "/tmp/sympsica_sched100_r.bin", seed_arg);
                        observed_r[i].push_back(Query::open_count(ch, out));
                    }
                }
                if (std::getenv("SYMPSICA_CALIBRATE") != nullptr) {
                    double el = std::chrono::duration<double>(
                                    std::chrono::steady_clock::now() - t_start)
                                    .count();
                    std::cerr << "[calib] schedule#" << i << " (seed=" << c.seed
                              << ") done at t=" << el << "s, pool_r.triples.remaining()="
                              << pool_r.triples.remaining()
                              << " pool_r.gates.remaining()=" << pool_r.gates.remaining() << "\n";
                }
            }
        },
        [&](Channel& ch) {
            for (std::size_t i = 0; i < cases.size(); ++i) {
                const SchedCase& c = cases[i];
                PartyState st;
                u64 seed_arg = 0x53530000ull + c.seed;
                for (const DayRow& d : c.days) {
                    Update::apply(st, d.insert_s, d.delete_s, params_s.encoder, params_s.oracle);
                    if (d.query) {
                        Setup::refill_offline(pool_s, Role::Sender, ch, params_s,
                                               PoolSizes{44 * kQueryTopUpBuckets, 4 * kQueryTopUpBuckets});
                        Share out = Query::run(Role::Sender, st, pool_s, ch, params_s,
                                                "/tmp/sympsica_sched100_s.bin", seed_arg);
                        observed_s[i].push_back(Query::open_count(ch, out));
                    }
                }
            }
        });

    int mismatches = 0;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const SchedCase& c = cases[i];
        SCOPED_TRACE(::testing::Message() << "seed=" << c.seed);
        ASSERT_EQ(observed_r[i].size(), c.expected.size());
        ASSERT_EQ(observed_s[i].size(), c.expected.size());
        for (std::size_t q = 0; q < c.expected.size(); ++q) {
            EXPECT_EQ(observed_r[i][q], c.expected[q])
                << "seed=" << c.seed << " query#" << q << ": receiver count mismatch";
            EXPECT_EQ(observed_s[i][q], c.expected[q])
                << "seed=" << c.seed << " query#" << q << ": sender count mismatch";
            if (observed_r[i][q] != c.expected[q] || observed_s[i][q] != c.expected[q]) ++mismatches;
        }
    }
    EXPECT_EQ(mismatches, 0) << "SC1: all " << cases.size() << "/" << cases.size()
                              << " schedules must match reference.py exactly at every query day";

    const double elapsed_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    std::cerr << "[SC1] " << cases.size() << "-schedule sweep measured runtime: " << elapsed_s
              << "s\n";
}

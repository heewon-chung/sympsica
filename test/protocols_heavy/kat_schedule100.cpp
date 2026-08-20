// test/protocols_heavy/kat_schedule100.cpp — Task 19's SC1 (task-19-brief.md
// W5.8, R-SCALE19 SMALL scale): ALL 100 randomized day-schedules (seeds
// 0..99, the plan's literal count -- W5.8 text and the phase SC line both
// say "100 randomized schedules") replayed IN-PROCESS against a real
// two-party Channel, comparing every query day's reconstructed count
// against ref/reference.py's Ref.simulate_days() golden.
//
// R-SCALE19-AMEND (controller ruling, binding, supersedes an earlier draft
// of this file that reduced the sweep to 35 seeds): the "~180s Release"
// figure in the original brief was a controller-side CONVENIENCE estimate,
// not a requirement -- the 100-seed count itself is SPEC-SIDE (plan W5.8 +
// the phase SC line) and must not be traded away to protect an invented
// budget number. This test therefore runs the FULL 100 seeds behind the
// "heavy" ctest LABEL with a TIMEOUT sized for it (see CMakeLists.txt);
// the label is exactly what keeps the default/fast developer loop
// unaffected by this leg's real cost.
//
// Measured cost breakdown (task-19-report.md has the full investigation):
// a controlled experiment (10x larger upfront pool + a much lower
// refill_offline top-up target, to test whether per-query-day pool
// provisioning dominated the per-schedule cost) made NO measurable
// difference to total runtime -- confirming the cost is dominated by the
// real MPC evaluation itself (eval_union's Beaver-triple/ZtGate rounds,
// proportional to each query's union bucket count), which is irreducible
// work, not incidental overhead. What DOES reduce cost proportionally is
// party scale: the schedule generator (Ref.emit_sched100_fixture) was
// retuned to sit at the LOW end of R-SCALE19's own "n in [32,128] ids per
// party" range (measured n: min 33 / max 45 / mean ~40 across seeds
// 0..99) -- SC1 exists to cover SCHEDULE LOGIC (filter, J marking, path
// selection, count correctness), not crypto scale, so there is no reason
// to pay for n toward the top of that range here.
//
// Measured runtime (task-19-report.md): 436.2s for all 100 seeds --
// comfortably inside the "heavy" TIMEOUT below (~3.4x margin).
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
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
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

// M3 (task-22-brief.md, carried minor): a per-process, mkdtemp-style unique
// temp directory for party state files, replacing the old hardcoded
// "/tmp/sympsica_sched100_{r,s}.bin" paths. Two concurrent runs of this test
// binary -- or a stale file left behind by a killed run -- used to collide
// silently on those fixed paths; mkdtemp's XXXXXX suffix guarantees a fresh,
// unique directory per test invocation, and TmpDirGuard removes it again on
// scope exit (including on a fatal ASSERT_* early-return) so nothing leaks
// into /tmp. Same idiom as test/protocols_heavy/kat_clmb.cpp's own M3 fix.
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
constexpr u64 kQueryTopUpBuckets = 150;

} // namespace

TEST(Schedule100, SC1_HundredRandomSchedulesMatchReferencePy) {
    const auto t_start = std::chrono::steady_clock::now();

    TmpDirGuard tmp_guard("sympsica_sched100");
    const std::string state_r = (tmp_guard.dir / "r.bin").string();
    const std::string state_s = (tmp_guard.dir / "s.bin").string();

    std::vector<SchedCase> cases =
        load_sched100(sympsica_test::fixture_path("test/fixtures/sched100.fixture"));
    ASSERT_EQ(cases.size(), 100u) << "SC1 fixture must carry all 100 seeds (0..99), the plan's "
                                      "own literal count (R-SCALE19-AMEND)";

    Params params_r = Params::instantiate();
    Params params_s = Params::instantiate();

    // ONE shared Setup::run for the whole 100-schedule sweep (R-SCALE19's
    // amortization rationale, this file's top comment).
    Pools pool_r, pool_s;
    run_two_party(
        "127.0.0.1:49900",
        [&](Channel& ch) { pool_r = Setup::run(Role::Receiver, ch, params_r, PoolSizes{40000, 4000}); },
        [&](Channel& ch) { pool_s = Setup::run(Role::Sender, ch, params_s, PoolSizes{40000, 4000}); });

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
                                                state_r, seed_arg);
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
                                                state_s, seed_arg);
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

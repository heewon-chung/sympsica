// test/protocols_heavy/kat_clmb.cpp — Task 19's SC6 (task-19-brief.md W5.8,
// controller ruling R-CLMB): CLM-B ("O(lambda) lifetime public-key = base
// OTs once") -- PkOpCounter is NONZERO after Setup::run (non-vacuity,
// Task-16's FC3 precedent) and IDENTICAL across >= 20 refill_offline calls
// PLUS a full schedule run (Update::apply/Query::run day sequence).
//
// Scale note (documented deviation, R-SCALE19's spirit applied by
// analogy): CLM-B is not one of R-SCALE19's explicitly-scaled legs
// (schedule_tests / E2E-1..4); this test uses a SMALL schedule (same scale
// discipline as SC1's kat_schedule100.cpp), not E2E's n=2^10 scale, since
// CLM-B's property (PkOpCounter constancy) is orthogonal to schedule size
// -- refill_offline never touches PkOpCounter regardless of how much it
// refills (setup.hpp's own doc comment). task-19-report.md also cross-
// checks PkOpCounter stays constant across a REAL n=2^10 E2E-1..4 run
// (apps/party_main.cpp prints it to stderr) as a bonus, non-primary,
// confirmation at the scale CLM-B's own plan-text wording ("a full E2E
// schedule") literally names.

#include <gtest/gtest.h>

#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "sympsica/core/state.hpp"
#include "sympsica/protocols/query.hpp"
#include "sympsica/protocols/setup.hpp"
#include "sympsica/protocols/update.hpp"
#include "sympsica/utils/net.hpp"
#include "sympsica/utils/params.hpp"

using namespace sympsica;

namespace {

// M3 (task-22-brief.md, carried minor): a per-process, mkdtemp-style unique
// temp directory for party state files, replacing the old hardcoded
// "/tmp/sympsica_clmb_{r,s}.bin" paths. Two concurrent runs of this test
// binary -- or a stale file left behind by a killed run -- used to collide
// silently on those fixed paths; mkdtemp's XXXXXX suffix guarantees a fresh,
// unique directory per test invocation, and TmpDirGuard removes it again on
// scope exit (including on a fatal ASSERT_* early-return) so nothing leaks
// into /tmp.
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

} // namespace

TEST(ClmB, SC6_PkOpCounterConstantAcrossSetupTwentyRefillsAndASchedule) {
    TmpDirGuard tmp_guard("sympsica_clmb");
    const std::string state_r = (tmp_guard.dir / "r.bin").string();
    const std::string state_s = (tmp_guard.dir / "s.bin").string();

    Params params_r = Params::instantiate();
    Params params_s = Params::instantiate();

    // --- Setup::run: exactly ONE base-OT setup (R-PKOP) ---
    Pools pool_r, pool_s;
    run_two_party(
        "127.0.0.1:49950",
        [&](Channel& ch) { pool_r = Setup::run(Role::Receiver, ch, params_r, PoolSizes{200, 20}); },
        [&](Channel& ch) { pool_s = Setup::run(Role::Sender, ch, params_s, PoolSizes{200, 20}); });

    const u64 after_setup = PkOpCounter::value();
    ASSERT_GT(after_setup, 0u) << "SC6 non-vacuity (R-CLMB): PkOpCounter must be NONZERO after "
                                   "Setup::run, or 'constant' below would hold vacuously";

    // --- >= 20 refill_offline calls, small increments, no base OTs ---
    run_two_party("127.0.0.1:49951",
                   [&](Channel& ch) {
                       for (int i = 0; i < 20; ++i) {
                           Setup::refill_offline(pool_r, Role::Receiver, ch, params_r,
                                                  PoolSizes{200 + static_cast<std::size_t>(i) * 40,
                                                            20 + static_cast<std::size_t>(i) * 4});
                       }
                   },
                   [&](Channel& ch) {
                       for (int i = 0; i < 20; ++i) {
                           Setup::refill_offline(pool_s, Role::Sender, ch, params_s,
                                                  PoolSizes{200 + static_cast<std::size_t>(i) * 40,
                                                            20 + static_cast<std::size_t>(i) * 4});
                       }
                   });

    EXPECT_EQ(PkOpCounter::value(), after_setup)
        << "SC6: PkOpCounter must stay EXACTLY constant across 20 refill_offline calls";

    // --- a small, full schedule run (Update::apply/Query::run day sequence) ---
    PartyState st_r, st_s;
    st_r.my_ids = {1, 2, 3};
    st_r.table.init(st_r.my_ids, params_r.encoder, params_r.oracle);
    st_r.my_size = 3;
    st_s.my_ids = {2, 3, 4};
    st_s.table.init(st_s.my_ids, params_s.encoder, params_s.oracle);
    st_s.my_size = 3;

    run_two_party(
        "127.0.0.1:49952",
        [&](Channel& ch) {
            (void)Query::run(Role::Receiver, st_r, pool_r, ch, params_r, state_r,
                              /*seed=*/9001);
        },
        [&](Channel& ch) {
            (void)Query::run(Role::Sender, st_s, pool_s, ch, params_s, state_s,
                              /*seed=*/9002);
        });

    Update::apply(st_r, std::vector<u64>{5, 6}, std::vector<u64>{1}, params_r.encoder, params_r.oracle);
    Update::apply(st_s, std::vector<u64>{7}, std::vector<u64>{}, params_s.encoder, params_s.oracle);

    run_two_party(
        "127.0.0.1:49953",
        [&](Channel& ch) {
            Setup::refill_offline(pool_r, Role::Receiver, ch, params_r, PoolSizes{2000, 200});
            (void)Query::run(Role::Receiver, st_r, pool_r, ch, params_r, state_r,
                              /*seed=*/9003);
        },
        [&](Channel& ch) {
            Setup::refill_offline(pool_s, Role::Sender, ch, params_s, PoolSizes{2000, 200});
            (void)Query::run(Role::Sender, st_s, pool_s, ch, params_s, state_s,
                              /*seed=*/9004);
        });

    EXPECT_EQ(PkOpCounter::value(), after_setup)
        << "SC6: PkOpCounter must stay EXACTLY constant across a full local schedule run too";
}

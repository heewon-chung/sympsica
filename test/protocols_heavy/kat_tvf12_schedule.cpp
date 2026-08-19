// test/protocols_heavy/kat_tvf12_schedule.cpp — Task 19's SC4/FC4 (moved
// out of test/protocols/kat_query.cpp into its OWN heavy-labeled ctest
// target, task-19-brief.md W5.8 requirement 3: "Every heavy test: ctest
// LABEL 'heavy' + explicit TIMEOUT" -- gtest_discover_tests' PROPERTIES
// apply per-TARGET, not per-test, so a genuinely heavy multi-minute test
// needs its own target to carry that label without mislabeling every other
// (sub-second) test already living in test/protocols/.
//
// SC4 [TV-F12 schedule-level leg] / FC4 [TV-F12 negative] (controller
// ruling R-TVF12): unlike SWITCH-1's decide()-level unit rows (Task 17,
// test/protocols/kat_query.cpp), this drives a REAL Update::apply-
// constructed |J| to EXACTLY 1024 (expect Incremental, no announce) and to
// EXACTLY 1025 (expect FullAnnounced, announce set) via two genuine
// Query::run calls, asserting the path ACTUALLY TAKEN (path_out) on both
// parties. "The announce bit on the wire" (R-TVF12's own wording) is
// observed via its EFFECT: the announce byte round-0 sends is exactly
// `my_announce` (query.cpp), and the COUNTERPART's own path_out taking
// FullAnnounced (via the `counterpart_announce` OR term in SwitchRule::
// decide) is the directly-observable consequence of that wire bit having
// been set -- so asserting the receiver's path_out alongside the sender's
// own J-size precondition IS asserting the wire bit's effect, without
// needing a raw byte sniff of round 0's payload.
//
// Same q1/q2-shortcut economy as kat_query.cpp's own QRYINC test: q1 is a
// real, cheap FullPublic query (pays for nothing extra); the SAME real,
// unmodified Query::run call under test (q2 at |J|=1024, ~2*u_max-scale
// Incremental cost; q3 at |J|=1025, ~1025-bucket FullAnnounced cost,
// cheaper) is run exactly once each -- FC4 is then a ZERO-EXTRA-MPC-COST
// corollary: it reuses q2/q3's own REAL, already-paid-for |J| values and
// shows that a test-local `>=` (rather than the production's strict `>`)
// boundary rule disagrees with SwitchRule::decide's actual (correct)
// verdict at exactly these two real, schedule-constructed sizes.
//
// Measured runtime (task-19-report.md): ~142s (q2's Incremental leg alone
// ~57s, q3's FullAnnounced leg ~85s over ~3075 buckets). TIMEOUT below is
// ~3x that, per R-SCALE19.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "coproto/Socket/LocalAsyncSock.h"
#include "cryptoTools/Crypto/PRNG.h"
#include "macoro/trace.h"
#include "macoro/sync_wait.h"
#include "macoro/when_all.h"

#include "sympsica/core/pools.hpp"
#include "sympsica/core/share.hpp"
#include "sympsica/core/state.hpp"
#include "sympsica/core/table.hpp"
#include "sympsica/gates/beaver.hpp"
#include "sympsica/gates/symdiff.hpp"
#include "sympsica/protocols/detail/ztgate_pipeline.hpp"
#include "sympsica/protocols/query.hpp"
#include "sympsica/protocols/setup.hpp"
#include "sympsica/protocols/update.hpp"
#include "sympsica/utils/field.hpp"
#include "sympsica/utils/net.hpp"
#include "sympsica/utils/params.hpp"

using namespace sympsica;

namespace {

namespace zt = sympsica::ztgate;
namespace oc = osuCrypto;

// --- two-party TCP harness (kat_query.cpp pattern, duplicated per-file per
// this project's own established convention -- see kat_setup.cpp/
// kat_salt.cpp/kat_query.cpp, each of which duplicates the same small
// harness rather than sharing a common header). Port base 49800 stays
// clear of every other test binary's own range. ---

std::atomic<int> g_port_counter{49800};

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

std::string scratch_path(const std::string& name) {
    return (std::filesystem::temp_directory_path() / ("sympsica_kat_tvf12_" + name)).string();
}

void clear_path(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + ".tmp").c_str());
}

// --- dealer-triple + real-ZtGate pool builder (kat_query.cpp's QRY-INC
// precedent, duplicated here for the same reason as the harness above). ---

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
        for (auto& g : out[p]) converted[p].push_back(ZtGate{g.corr_id, Share{g.mask_share}, g.key});
    }
    return converted;
}

std::pair<Pools, Pools> build_pools(u64 triple_count, u64 gate_count, u64 seed) {
    Pools pool_r, pool_s;

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<u64> dist(0, Fp::P - 1);
    std::vector<Triple> t_r, t_s;
    t_r.reserve(triple_count);
    t_s.reserve(triple_count);
    for (u64 i = 0; i < triple_count; ++i) {
        Fp a(dist(rng)), b(dist(rng));
        Fp c = a.mul(b);
        Fp a0(dist(rng)), b0(dist(rng)), c0(dist(rng));
        t_r.push_back(Triple{i, Share{a0}, Share{b0}, Share{c0}});
        t_s.push_back(Triple{i, Share{a.sub(a0)}, Share{b.sub(b0)}, Share{c.sub(c0)}});
    }
    pool_r.triples.refill(std::move(t_r));
    pool_s.triples.refill(std::move(t_s));

    auto gates = gen_ztgates(gate_count, seed);
    pool_r.gates.refill(std::move(gates[0]));
    pool_s.gates.refill(std::move(gates[1]));

    return {std::move(pool_r), std::move(pool_s)};
}

} // namespace

TEST(TVF12Schedule, SC4_And_FC4_ScheduleLevelBoundaryAt1024And1025) {
    Params params = Params::instantiate();
    const Encoder& enc = params.encoder;
    const BucketOracle& G = params.oracle;

    const std::vector<u64> A = {1, 2, 3};
    const std::vector<u64> B = {2, 3, 4};

    PartyState st_r, st_s;
    st_r.my_ids = A;
    st_r.table.init(A, enc, G);
    st_r.my_size = A.size();
    st_s.my_ids = B;
    st_s.table.init(B, enc, G);
    st_s.my_size = B.size();

    const std::string path_r = scratch_path("tvf12_r.bin");
    const std::string path_s = scratch_path("tvf12_s.bin");
    clear_path(path_r);
    clear_path(path_s);

    // q1: first query, tiny -> FullPublic (firstQuery unconditional).
    std::cerr << "[TVF12] starting q1\n";
    {
        auto pools = build_pools(300, 30, 0x54120000ull);
        run_two_party(
            next_address(),
            [&](Channel& ch) {
                (void)Query::run(Role::Receiver, st_r, pools.first, ch, params, path_r, 12001);
            },
            [&](Channel& ch) {
                (void)Query::run(Role::Sender, st_s, pools.second, ch, params, path_s, 12002);
            });
    }
    std::cerr << "[TVF12] q1 done\n";
    ASSERT_EQ(st_r.query_no, 1u);
    ASSERT_EQ(st_s.query_no, 1u);

    // Bulk-grow the Receiver so nA+nB > 2*u_max (escapes SwitchRule's size
    // condition), while driving the SENDER's own |J| to EXACTLY u_max=1024
    // via 1024 fresh ids landing in 1024 DISTINCT buckets (verified below).
    // Receiver bulk is 1020 (< u_max, mirrors QRYINC's own precedent) --
    // NOT >= u_max: the Receiver's OWN |J| must stay <= u_max too, or its
    // own my_announce bit would fire and select FullAnnounced/full-path (a
    // LARGER, differently-sized union than Incremental's fixed 2*u_max)
    // instead of the Incremental path this test is actually about.
    std::vector<u64> receiver_bulk;
    receiver_bulk.reserve(1020);
    for (u64 i = 0; i < 1020; ++i) receiver_bulk.push_back(30'000'000ull + i);
    Update::apply(st_r, receiver_bulk, std::vector<u64>{}, enc, G);
    Update::apply(st_s, std::vector<u64>{}, std::vector<u64>{}, enc, G);

    std::vector<u64> sender_bulk_1024;
    sender_bulk_1024.reserve(Params::U_MAX);
    for (u64 i = 0; i < Params::U_MAX; ++i) sender_bulk_1024.push_back(40'000'000ull + i);
    Update::apply(st_s, sender_bulk_1024, std::vector<u64>{}, enc, G);
    Update::apply(st_r, std::vector<u64>{}, std::vector<u64>{}, enc, G);

    ASSERT_EQ(st_s.J.size(), Params::U_MAX)
        << "test precondition: sender bulk ids must land in exactly u_max DISTINCT buckets";
    ASSERT_GT(st_r.my_size + st_s.my_size, 2 * Params::U_MAX)
        << "test precondition: q2 must clear SwitchRule's FullPublic size condition";

    // q2: |J_sender| == u_max exactly -> Incremental on BOTH sides, no
    // announce (SC4 positive boundary).
    std::cerr << "[TVF12] starting q2 (incremental, ~2*u_max buckets)\n";
    SwitchRule::Path taken_r_1024{}, taken_s_1024{};
    {
        auto pools = build_pools(44 * (2 * Params::U_MAX), 4 * (2 * Params::U_MAX), 0x54120001ull);
        run_two_party(
            next_address(),
            [&](Channel& ch) {
                (void)Query::run(Role::Receiver, st_r, pools.first, ch, params, path_r, 12003,
                                  /*force_full=*/false, &taken_r_1024);
            },
            [&](Channel& ch) {
                (void)Query::run(Role::Sender, st_s, pools.second, ch, params, path_s, 12004,
                                  /*force_full=*/false, &taken_s_1024);
            });
    }
    EXPECT_EQ(taken_r_1024, SwitchRule::Path::Incremental)
        << "SC4: |J_sender|==u_max must NOT announce -- receiver takes Incremental";
    EXPECT_EQ(taken_s_1024, SwitchRule::Path::Incremental)
        << "SC4: |J_sender|==u_max is exactly at the boundary -- sender itself also takes "
           "Incremental (myJ==u_max, not > u_max)";
    std::cerr << "[TVF12] q2 done\n";
    ASSERT_EQ(st_r.query_no, 2u);
    ASSERT_EQ(st_s.query_no, 2u);
    ASSERT_TRUE(st_s.J.empty()) << "q2's own commit clears J -- next bulk starts from empty";

    // q2's own commit CLEARED st_s.J (commit() always clears J on success)
    // -- so q3's boundary is built from a FRESH batch of u_max+1=1025
    // distinct-bucket ids (not "one more on top of the old 1024", which
    // would only reach |J|=1) landing in buckets disjoint from everything
    // used so far.
    std::vector<u64> sender_bulk_1025;
    sender_bulk_1025.reserve(Params::U_MAX + 1);
    for (u64 i = 0; i < Params::U_MAX + 1; ++i) sender_bulk_1025.push_back(50'000'000ull + i);
    Update::apply(st_s, sender_bulk_1025, std::vector<u64>{}, enc, G);
    Update::apply(st_r, std::vector<u64>{}, std::vector<u64>{}, enc, G);
    ASSERT_EQ(st_s.J.size(), Params::U_MAX + 1)
        << "test precondition: sender's |J| must be exactly u_max+1 for q3";

    // q3: |J_sender| == u_max+1 -> FullAnnounced on BOTH sides (SC4
    // negative-side boundary). Cheaper than q2: the full path's cost scales
    // with min(my_size, M), not the fixed 2*u_max Incremental always pays.
    SwitchRule::Path taken_r_1025{}, taken_s_1025{};
    {
        const u64 target_buckets = st_r.my_size + st_s.my_size; // safe upper bound on the union
        std::cerr << "[TVF12] starting q3 (full, target_buckets=" << target_buckets << ")\n";
        auto pools = build_pools(44 * target_buckets, 4 * target_buckets, 0x54120002ull);
        run_two_party(
            next_address(),
            [&](Channel& ch) {
                (void)Query::run(Role::Receiver, st_r, pools.first, ch, params, path_r, 12005,
                                  /*force_full=*/false, &taken_r_1025);
            },
            [&](Channel& ch) {
                (void)Query::run(Role::Sender, st_s, pools.second, ch, params, path_s, 12006,
                                  /*force_full=*/false, &taken_s_1025);
            });
    }
    EXPECT_EQ(taken_r_1025, SwitchRule::Path::FullAnnounced)
        << "SC4: |J_sender|==u_max+1 -- the sender's announce bit (observed on the WIRE by the "
           "receiver's own round-0 exchange) drives the receiver to FullAnnounced too";
    EXPECT_EQ(taken_s_1025, SwitchRule::Path::FullAnnounced)
        << "SC4: |J_sender|==u_max+1 -- the sender's OWN myJ>u_max branch fires directly";

    // --- FC4 [TV-F12 negative]: a wrong `>=` boundary rule, evaluated at
    // the SAME two real |J| values q2/q3 just constructed, disagrees with
    // SwitchRule::decide's actual (correct, strict `>`) verdict -- zero
    // extra MPC cost.
    const u64 boundary_j = Params::U_MAX;          // the real |J| q2 constructed (SC4 positive)
    const u64 over_boundary_j = Params::U_MAX + 1; // the real |J| q3 constructed (SC4 negative)
    auto correct_rule = [](u64 myJ) { return myJ > Params::U_MAX; };      // production's own rule
    auto wrong_ge_rule = [](u64 myJ) { return myJ >= Params::U_MAX; };    // WRONG: announces AT the boundary
    auto wrong_gt1_rule = [](u64 myJ) { return myJ > Params::U_MAX + 1; }; // WRONG: omits AT u_max+1

    EXPECT_NE(wrong_ge_rule(boundary_j), correct_rule(boundary_j))
        << "FC4: a >= u_max boundary rule wrongly announces AT the boundary (|J|==1024), which "
           "SC4 just proved the real protocol (correctly) does not";
    EXPECT_NE(wrong_gt1_rule(over_boundary_j), correct_rule(over_boundary_j))
        << "FC4: a > u_max+1 (off-by-one-too-strict) rule wrongly OMITS the announce at "
           "|J|==1025, which SC4 just proved the real protocol (correctly) does not";

    clear_path(path_r);
    clear_path(path_s);
}

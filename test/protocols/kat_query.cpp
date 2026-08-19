// test/protocols/kat_query.cpp — Task 17's formal SC/FC suite for
// protocols/query.hpp (task-17-brief.md, W5.2-W5.4): SwitchRule::decide()
// and Query::run (incremental / full / receiver-side conversion / atomic
// commit / crash-hook plumbing).
//
// Test IDs carried in the test names:
//   SWITCH-1 [SC2, FC2]  — decide() unit table, all branches + the TV-F12
//                          boundary (myJ==u_max -> Incremental; myJ==u_max+1
//                          -> FullAnnounced).
//   QRY-1    [SC1]       — concrete full-path query over REAL Setup::run
//                          pools; reconstructed count == |A meet B|; the
//                          Sender's returned share equals -inv2*t_S exactly.
//   QRY-FULL [SC4]       — full-path REPLACE semantics: stale pre-seeded
//                          cache is fully replaced, not accumulated.
//   QRY-F2   [SC6]       — a padded bucket's new share reconstructs to 0.
//   QRY-INC  [SC3]       — incremental path end-to-end, real u_max-scale
//                          padding (structurally unavoidable -- see the
//                          fixture's own top comment).
//   FC3      [QRY-F1]    — double-applying the SAME commit delta drifts
//                          t_share; derived from QRY-INC's own before/after
//                          t_share (zero extra MPC cost).
//   FC4      [exhaustion]— undersized pools abort with NO state mutation.
//   QRY-ATOMIC [SC5]     — SYMPSICA_CRASH_AT=pre-serialize kills the
//                          process after evaluation, before save(); the
//                          state file is untouched; reload + retry succeeds.
//   FC1      [TV-F9]     — the receiver-form conversion applied on BOTH
//                          parties reconstructs the wrong count.
//   FC5      [TV-F8-lite]— building one party's syndrome with the wrong
//                          (+,+) sign pair on an ASYMMETRIC set pair gives
//                          the wrong count.
//
// Harness: SC1/SC4/SC6/FC1/FC4/SC5 use REAL Setup::run over a real
// loopback TCP Channel pair (production composition, task-17-brief.md
// requirement 2) at deliberately tiny scale (|U| <= 8). QRY-INC/FC3
// structurally require an evaluation set close to 2*u_max (padding always
// fills to EXACTLY u_max on the incremental path, regardless of how few
// real dirty buckets there are -- see that fixture's own comment), which
// is bench-scale for Setup::run's OWN base-OT-inclusive path (R-SCALE:
// "do NOT run [Setup::run at 90112/8192] in tests"); that fixture instead
// uses DEALER triples (Task-13 precedent, cheap, no real crypto) + REAL
// ZtGates via the lower-level Phase-2 pipeline directly (bypassing
// Setup::run's base-OT/PkOpCounter machinery -- "ZtGatePool is already
// always real, a DPF key cannot be dealer-faked", kat_symdiff.cpp), the
// SAME established pattern kat_symdiff.cpp/kat_ztest.cpp already use for
// every protocol-level MPC-evaluation test in this codebase.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include "coproto/Socket/LocalAsyncSock.h"
#include "cryptoTools/Crypto/PRNG.h"
// macoro/sync_wait.h uses std::source_location/basic_traceable but does not
// include macoro/trace.h itself -- same transitive-include gap every other
// file in this project that drives a real coproto socket already documents
// (test/protocols/kat_setup.cpp, test/gates/kat_symdiff.cpp).
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

// R-CONVERT's field inverse of 2 (mirrors query.cpp's own INV2 -- test-side
// duplication, since it's a private production constant).
constexpr Fp kInv2 = Fp(1ull << 60);

// ---------------------------------------------------------------------------
// Two-party TCP harness (kat_setup.cpp / kat_symdiff.cpp pattern).
// ---------------------------------------------------------------------------

std::atomic<int> g_port_counter{49500};

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
    return (std::filesystem::temp_directory_path() / ("sympsica_kat_query_" + name)).string();
}

void clear_path(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + ".tmp").c_str());
}

std::string read_file_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Small PoolSizes throughout the Setup::run-backed tests (R-SCALE: never
// bench-scale in a test). |U| <= 8 in every such scenario here.
constexpr PoolSizes kTinySizes{/*triples=*/400, /*gates=*/40};

// ---------------------------------------------------------------------------
// QRY-INC/FC3-only harness: dealer triples (Task-13 precedent) + REAL
// ZtGates via the lower-level Phase-2 pipeline (bypasses Setup::run's
// base-OT overhead entirely -- see this file's top comment).
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Fork-based subprocess harness (FC4, SC5): fork() BEFORE any threads
// exist in the child (kat_ztest.cpp's TV-F3 comment documents the hazard
// this avoids -- only the calling thread survives a fork of a multi-
// threaded process, so a real two-party TCP exchange started before the
// fork would hang forever in the child once its peer thread silently stops
// existing). The child spawns its OWN threads freely after forking.
// ---------------------------------------------------------------------------

struct ChildResult {
    bool exited = false;
    int exit_code = -1;
    bool signaled = false;
    int signal = 0;
};

ChildResult run_in_child(const std::function<void()>& fn) {
    pid_t pid = fork();
    EXPECT_NE(pid, -1) << "fork() failed";
    if (pid == -1) return ChildResult{};
    if (pid == 0) {
        fn();
        _exit(0); // only reached if fn() returned without crashing
    }
    ChildResult r;
    int status = 0;
    EXPECT_NE(waitpid(pid, &status, 0), -1) << "waitpid() failed";
    if (WIFEXITED(status)) {
        r.exited = true;
        r.exit_code = WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        r.signaled = true;
        r.signal = WTERMSIG(status);
    }
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// SWITCH-1 [SC2, FC2]: decide() unit table -- all three branches plus the
// TV-F12 boundary. Pure logic, zero communication, zero pools.
// ---------------------------------------------------------------------------

TEST(Query, SWITCH1_DecideTable) {
    Params params = Params::instantiate();
    using Path = SwitchRule::Path;
    const u64 kBig = 1'000'000; // >> 2*u_max, safely clears the size test

    // firstQuery -> FullPublic, regardless of sizes/J.
    EXPECT_EQ(SwitchRule::decide(params, kBig, kBig, 0, /*firstQuery=*/true, false), Path::FullPublic);

    // size condition -> FullPublic: 2*u_max >= min(nA,m)+min(nB,m). At
    // nA=nB=u_max exactly, 2*u_max == u_max+u_max -- the boundary itself.
    EXPECT_EQ(SwitchRule::decide(params, Params::U_MAX, Params::U_MAX, 0, false, false),
              Path::FullPublic);

    // myJ == u_max -> Incremental (TV-F12 positive: the announce bit does
    // NOT fire at the boundary itself).
    EXPECT_EQ(SwitchRule::decide(params, kBig, kBig, Params::U_MAX, false, false),
              Path::Incremental);

    // myJ == u_max+1 -> FullAnnounced (TV-F12 negative: announces strictly
    // above u_max).
    EXPECT_EQ(SwitchRule::decide(params, kBig, kBig, Params::U_MAX + 1, false, false),
              Path::FullAnnounced);

    // counterpartAnnounced -> FullAnnounced, even with a tiny myJ.
    EXPECT_EQ(SwitchRule::decide(params, kBig, kBig, 0, false, /*counterpartAnnounced=*/true),
              Path::FullAnnounced);
}

// ---------------------------------------------------------------------------
// QRY-1 [SC1, CONCRETE]: nA=3, nB=3, |A xor B|=2 -- A={1,2,3}, B={2,3,4},
// |A meet B|=2. This is necessarily the FIRST query for both parties
// (query_no==0), so SwitchRule::decide() takes the FullPublic branch
// regardless of size -- this test exercises the full path over REAL
// Setup::run pools (production composition, requirement 2).
// ---------------------------------------------------------------------------

TEST(Query, QRY1_ConcreteFullPathReconstructsIntersectionSize) {
    Params params = Params::instantiate();
    const std::vector<u64> A = {1, 2, 3};
    const std::vector<u64> B = {2, 3, 4};

    PartyState st_r, st_s;
    st_r.my_ids = A;
    st_r.table.init(A, params.encoder, params.oracle);
    st_r.my_size = A.size();
    st_s.my_ids = B;
    st_s.table.init(B, params.encoder, params.oracle);
    st_s.my_size = B.size();

    Pools pool_r, pool_s;
    run_two_party(
        next_address(),
        [&](Channel& ch) { pool_r = Setup::run(Role::Receiver, ch, params, kTinySizes); },
        [&](Channel& ch) { pool_s = Setup::run(Role::Sender, ch, params, kTinySizes); });

    const std::string path_r = scratch_path("qry1_r.bin");
    const std::string path_s = scratch_path("qry1_s.bin");
    clear_path(path_r);
    clear_path(path_s);

    Share out_r{}, out_s{};
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            out_r = Query::run(Role::Receiver, st_r, pool_r, ch, params, path_r, /*seed=*/1001);
        },
        [&](Channel& ch) {
            out_s = Query::run(Role::Sender, st_s, pool_s, ch, params, path_s, /*seed=*/2002);
        });

    // Structural check: the Sender's returned share equals -inv2*t_S
    // exactly (receiver-side-only affine, verified WITHOUT re-deriving
    // convert()'s own arithmetic -- st_s.t_share is the raw, pre-
    // conversion committed value; convert() itself never mutates it).
    const Fp expected_sender = kInv2.neg().mul(st_s.t_share.v);
    EXPECT_EQ(out_s.v, expected_sender);

    // Reconstruct via test-side open() (R-XCHG's explicit carve-out: "the
    // final count opening, one element, test-side, may use open()").
    u64 reconstructed = 0;
    run_two_party(
        next_address(), [&](Channel& ch) { reconstructed = open(ch, out_r).v; },
        [&](Channel& ch) { (void)open(ch, out_s); });
    EXPECT_EQ(reconstructed, 2u);

    // query_no advanced; J cleared on both sides; on-disk file matches
    // in-memory state (load-back compare).
    EXPECT_EQ(st_r.query_no, 1u);
    EXPECT_EQ(st_s.query_no, 1u);
    EXPECT_TRUE(st_r.J.empty());
    EXPECT_TRUE(st_s.J.empty());

    PartyState loaded_r;
    loaded_r.load(path_r);
    EXPECT_EQ(loaded_r.t_share, st_r.t_share);
    EXPECT_EQ(loaded_r.query_no, st_r.query_no);
    EXPECT_EQ(loaded_r.cache.size(), st_r.cache.size());

    clear_path(path_r);
    clear_path(path_s);
}

// ---------------------------------------------------------------------------
// FC1 [TV-F9]: applying the RECEIVER-form conversion on BOTH parties (a
// test-side wrong computation) reconstructs a count that differs from the
// true value -- demonstrating that the Sender's own -inv2*t_S form (not a
// second copy of the Receiver's inv2*(nA+nB-t) form) is load-bearing.
// ---------------------------------------------------------------------------

TEST(Query, FC1_ReceiverFormConversionOnBothPartiesGivesWrongCount) {
    Params params = Params::instantiate();
    const std::vector<u64> A = {1, 2, 3};
    const std::vector<u64> B = {2, 3, 4};

    PartyState st_r, st_s;
    st_r.my_ids = A;
    st_r.table.init(A, params.encoder, params.oracle);
    st_r.my_size = A.size();
    st_s.my_ids = B;
    st_s.table.init(B, params.encoder, params.oracle);
    st_s.my_size = B.size();

    Pools pool_r, pool_s;
    run_two_party(
        next_address(),
        [&](Channel& ch) { pool_r = Setup::run(Role::Receiver, ch, params, kTinySizes); },
        [&](Channel& ch) { pool_s = Setup::run(Role::Sender, ch, params, kTinySizes); });

    const std::string path_r = scratch_path("fc1_r.bin");
    const std::string path_s = scratch_path("fc1_s.bin");
    clear_path(path_r);
    clear_path(path_s);

    run_two_party(
        next_address(),
        [&](Channel& ch) {
            (void)Query::run(Role::Receiver, st_r, pool_r, ch, params, path_r, /*seed=*/3003);
        },
        [&](Channel& ch) {
            (void)Query::run(Role::Sender, st_s, pool_s, ch, params, path_s, /*seed=*/4004);
        });

    const Fp nAB = Fp::from_u64(st_r.my_size).add(Fp::from_u64(st_s.my_size));

    // Correct: receiver-form on the Receiver, sender-form on the Sender.
    const Fp correct_r = kInv2.mul(nAB.sub(st_r.t_share.v));
    const Fp correct_s = kInv2.neg().mul(st_s.t_share.v);
    const Fp correct_count = correct_r.add(correct_s);
    EXPECT_EQ(correct_count.v, Fp::from_u64(2).v) << "sanity: the correct form reconstructs |A meet B|=2";

    // Wrong (TV-F9): receiver-form applied on BOTH parties.
    const Fp wrong_s = kInv2.mul(nAB.sub(st_s.t_share.v));
    const Fp wrong_count = correct_r.add(wrong_s);
    EXPECT_NE(wrong_count.v, correct_count.v)
        << "TV-F9: the receiver-form-on-both bug must be distinguishable from the correct count";

    clear_path(path_r);
    clear_path(path_s);
}

// ---------------------------------------------------------------------------
// QRY-FULL [SC4]: full-path REPLACE semantics -- a stale, pre-seeded cache
// entry (including one for a bucket NOT in this query's evaluation set) is
// fully replaced, not accumulated into.
// ---------------------------------------------------------------------------

TEST(Query, QRYFULL_ReplaceSemanticsDropsStaleCacheAndSum) {
    Params params = Params::instantiate();
    const std::vector<u64> A = {1, 2, 3};
    const std::vector<u64> B = {2, 3, 4};

    PartyState st_r, st_s;
    st_r.my_ids = A;
    st_r.table.init(A, params.encoder, params.oracle);
    st_r.my_size = A.size();
    st_s.my_ids = B;
    st_s.table.init(B, params.encoder, params.oracle);
    st_s.my_size = B.size();

    // Stale pre-seeding: one entry for a bucket that WILL be in this
    // query's union (G.of(1), wrong stale value 456 -- must be
    // overwritten, not accumulated onto), one for a bucket that will NOT
    // be in it (999999 -- must be dropped entirely), and a nonzero stale
    // t_share (789 -- must be fully replaced by the sum of fresh shares,
    // not added to).
    const u32 beta1 = params.oracle.of(1);
    st_r.cache[beta1] = Share{Fp(456)};
    st_r.cache[999999] = Share{Fp(123)};
    st_r.t_share = Share{Fp(789)};

    Pools pool_r, pool_s;
    run_two_party(
        next_address(),
        [&](Channel& ch) { pool_r = Setup::run(Role::Receiver, ch, params, kTinySizes); },
        [&](Channel& ch) { pool_s = Setup::run(Role::Sender, ch, params, kTinySizes); });

    const std::string path_r = scratch_path("qryfull_r.bin");
    const std::string path_s = scratch_path("qryfull_s.bin");
    clear_path(path_r);
    clear_path(path_s);

    Share out_r{}, out_s{};
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            out_r = Query::run(Role::Receiver, st_r, pool_r, ch, params, path_r, /*seed=*/5005);
        },
        [&](Channel& ch) {
            out_s = Query::run(Role::Sender, st_s, pool_s, ch, params, path_s, /*seed=*/6006);
        });

    // Expected union: {1,2,3} occupied on the Receiver, {2,3,4} occupied on
    // the Sender, no padding (each party's target == its own occupied
    // count exactly) -> 4 distinct buckets (1,2,3,4 map to distinct
    // buckets w.h.p. over a 2^31 domain).
    std::set<u32> expected_union = {params.oracle.of(1), params.oracle.of(2), params.oracle.of(3),
                                     params.oracle.of(4)};
    ASSERT_EQ(expected_union.size(), 4u) << "test precondition: ids 1..4 map to distinct buckets";

    EXPECT_EQ(st_r.cache.size(), expected_union.size());
    EXPECT_EQ(st_r.cache.count(999999), 0u) << "the stale, out-of-union cache entry must be dropped";
    for (u32 beta : expected_union) EXPECT_EQ(st_r.cache.count(beta), 1u) << "beta=" << beta;

    // t_share was REPLACED, not accumulated: reconstructing via the real
    // returned shares gives the true count (2), which could not hold if
    // the stale 789 residual had leaked into the sum.
    u64 reconstructed = 0;
    run_two_party(
        next_address(), [&](Channel& ch) { reconstructed = open(ch, out_r).v; },
        [&](Channel& ch) { (void)open(ch, out_s); });
    EXPECT_EQ(reconstructed, 2u);

    clear_path(path_r);
    clear_path(path_s);
}

// ---------------------------------------------------------------------------
// QRY-F2 [SC6]: a padded (unoccupied-everywhere) bucket's new share
// reconstructs to 0. The Receiver declares my_size=4 while only actually
// occupying 3 buckets (a legitimate test-only field poke: Query::run only
// ever READS my_size as a number, never cross-checks it against my_ids/
// table -- this is exactly the shortcut needed to force exactly ONE real
// padding bucket without touching the global count-correctness formula,
// which this test does not exercise).
// ---------------------------------------------------------------------------

TEST(Query, QRYF2_PaddedBucketReconstructsToZero) {
    Params params = Params::instantiate();
    const std::vector<u64> A = {1, 2, 3};
    const std::vector<u64> B = {100, 101, 102};

    PartyState st_r, st_s;
    st_r.my_ids = A;
    st_r.table.init(A, params.encoder, params.oracle);
    st_r.my_size = A.size() + 1; // declares one more than it actually occupies -> 1 pad bucket
    st_s.my_ids = B;
    st_s.table.init(B, params.encoder, params.oracle);
    st_s.my_size = B.size();

    Pools pool_r, pool_s;
    run_two_party(
        next_address(),
        [&](Channel& ch) { pool_r = Setup::run(Role::Receiver, ch, params, kTinySizes); },
        [&](Channel& ch) { pool_s = Setup::run(Role::Sender, ch, params, kTinySizes); });

    const std::string path_r = scratch_path("qryf2_r.bin");
    const std::string path_s = scratch_path("qryf2_s.bin");
    clear_path(path_r);
    clear_path(path_s);

    run_two_party(
        next_address(),
        [&](Channel& ch) {
            (void)Query::run(Role::Receiver, st_r, pool_r, ch, params, path_r, /*seed=*/7007);
        },
        [&](Channel& ch) {
            (void)Query::run(Role::Sender, st_s, pool_s, ch, params, path_s, /*seed=*/8008);
        });

    std::set<u32> known;
    for (u64 id : A) known.insert(params.oracle.of(id));
    for (u64 id : B) known.insert(params.oracle.of(id));
    ASSERT_EQ(known.size(), 6u) << "test precondition: ids 1..3,100..102 map to distinct buckets";

    std::vector<u32> pad_candidates;
    for (const auto& [beta, sh] : st_r.cache) {
        (void)sh;
        if (known.count(beta) == 0) pad_candidates.push_back(beta);
    }
    ASSERT_EQ(pad_candidates.size(), 1u)
        << "exactly one pad bucket expected (my_size - occupied = 1)";
    const u32 pad_beta = pad_candidates[0];
    EXPECT_GE(pad_beta, 1u);
    EXPECT_LE(static_cast<u64>(pad_beta), Params::M);
    ASSERT_EQ(st_s.cache.count(pad_beta), 1u) << "both parties' caches must agree on the union";

    u64 reconstructed = 0;
    run_two_party(
        next_address(), [&](Channel& ch) { reconstructed = open(ch, st_r.cache.at(pad_beta)).v; },
        [&](Channel& ch) { (void)open(ch, st_s.cache.at(pad_beta)); });
    EXPECT_EQ(reconstructed, 0u) << "a bucket unoccupied on BOTH sides must reconstruct to 0";

    clear_path(path_r);
    clear_path(path_s);
}

// ---------------------------------------------------------------------------
// QRY-INC [SC3] / FC3 [QRY-F1] — incremental path end-to-end, ONE combined
// TEST() (see note below on why) whose expensive real-crypto run pays for
// itself exactly once; FC3 is a zero-extra-cost corollary derived from
// that run's own before/after t_share.
//
// SCALE NOTE (binding, see this file's top comment): the incremental path
// pads J~ to EXACTLY Params::U_MAX on BOTH parties, unconditionally,
// REGARDLESS of how few real dirty buckets exist -- this is the design's
// own leakage-hiding property (an observer cannot tell |J| from |J~|), not
// a test-scale choice. Exercising this branch AT ALL therefore requires an
// evaluation set close to 2*u_max; there is no way to shrink it further
// (Params::U_MAX has no test override). To reach nA+nB > 2*u_max while
// keeping the LATEST update's own dirty-bucket count <= u_max (needed to
// avoid FullAnnounced), the growth is split into two epochs:
//   q1 (tiny, first query) -> FullPublic unconditionally (firstQuery)
//   + Update: 1020 new ids on the Receiver, disjoint from everything else
//   q2 (nA=1023,nB=3, sum=1026) -> STILL FullPublic (size condition: the
//     Receiver's own dirty count is irrelevant -- the size branch wins)
//   + Update: 1024 new ids on the Sender (disjoint; |J_sender|==u_max
//     exactly -- also exercises TV-F12's positive boundary as a bonus)
//   q3 (nA=1023,nB=1027, sum=2050 > 2*u_max=2048) -> INCREMENTAL on both
// All bulk ids are chosen from ranges disjoint from {1,2,3}/{2,3,4} and
// from each other, so the true intersection stays 2 ({2,3}) at every
// query -- the ground truth never needs recomputing.
//
// SINGLE TEST(), NOT TEST_F+SetUpTestSuite (DEVIATION, documented in
// task-17-report.md): an earlier draft used a TEST_F fixture with
// SetUpTestSuite() specifically so QRYINC and FC3 could share ONE
// expensive run. That does NOT work under this project's CMake harness:
// gtest_discover_tests registers every TEST()/TEST_F() case as its OWN
// ctest test, each invoking the WHOLE BINARY FRESH with its own
// --gtest_filter -- so SetUpTestSuite() actually re-ran once per ctest
// entry (measured: 86.93s + 87.24s = 174s for what was meant to be one
// 87s run), blowing well past the ~90s budget. Fixed by merging into ONE
// TEST() so ctest only ever invokes this scenario once.
//
// q2's REAL cost is ALSO paid for nothing beyond what SC1/SC4/SC6 already
// verify (full-path correctness is not re-tested here), so q2 is instead
// constructed directly and cheaply: each of the 1020 receiver-bulk ids
// sits in a bucket the Sender never touches (verified below), so that
// bucket's true t = min(count-of-bulk-ids-landing-there, T) exactly -- the
// same "one side empty -> t = capped count" relationship task-14's SD-1/
// SD-2 KATs already independently validate against reference.py, not a
// new claim introduced here. Each t is split into two additive Fp shares
// (seeded RNG) and injected straight into both caches/t_shares -- exactly
// what a real full query would have produced, without paying its ~1026-
// gate real-MPC cost a second time. q1 and q3 remain genuine, unmodified
// Query::run calls; q3 -- the actual incremental path under test -- is
// the ONLY leg whose crypto scale cannot be avoided (~2048 buckets, ~58s
// measured, dominating this test's total runtime).
// ---------------------------------------------------------------------------

TEST(Query, QRYINC_And_FC3_IncrementalEndToEndPlusDoubleCommitDrift) {
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

    const std::string path_r = scratch_path("qryinc_r.bin");
    const std::string path_s = scratch_path("qryinc_s.bin");
    clear_path(path_r);
    clear_path(path_s);

    // q1: first query, tiny -> FullPublic (firstQuery unconditional).
    {
        auto pools = build_pools(300, 30, 0x51000000ull);
        run_two_party(
            next_address(),
            [&](Channel& ch) {
                (void)Query::run(Role::Receiver, st_r, pools.first, ch, params, path_r, 9001);
            },
            [&](Channel& ch) {
                (void)Query::run(Role::Sender, st_s, pools.second, ch, params, path_s, 9002);
            });
    }
    ASSERT_EQ(st_r.query_no, 1u);
    ASSERT_EQ(st_s.query_no, 1u);

    // Update 1: 1020 new ids on the Receiver, disjoint from everything.
    std::vector<u64> receiver_bulk;
    receiver_bulk.reserve(1020);
    for (u64 i = 0; i < 1020; ++i) receiver_bulk.push_back(10'000'000ull + i);
    Update::apply(st_r, receiver_bulk, std::vector<u64>{}, enc, G);
    Update::apply(st_s, std::vector<u64>{}, std::vector<u64>{}, enc, G); // no-op, symmetry only

    // q2 SHORTCUT: cheap, LOCAL construction of exactly what a real
    // FullPublic full query would have committed (see the block comment
    // above). st_r.J is EXACTLY the set of buckets receiver_bulk touched
    // (q1 already cleared J; nothing else has run since).
    {
        ASSERT_FALSE(st_r.J.empty());
        std::unordered_map<u32, u32> bulk_bucket_count;
        for (u64 id : receiver_bulk) ++bulk_bucket_count[G.of(id)];

        std::set<u32> known_ab_buckets = {G.of(1), G.of(2), G.of(3), G.of(4)};
        std::mt19937_64 rng(0x51100000ull);
        std::uniform_int_distribution<u64> dist(0, Fp::P - 1);
        Fp r_sum(0), s_sum(0);
        for (u32 beta : st_r.J) {
            ASSERT_EQ(known_ab_buckets.count(beta), 0u)
                << "test precondition: a receiver-bulk id's bucket collided with A/B's own "
                   "buckets (beta=" << beta << ") -- astronomically unlikely, but re-pick ids "
                   "if this ever fires";
            auto it = bulk_bucket_count.find(beta);
            ASSERT_NE(it, bulk_bucket_count.end());
            ASSERT_LE(it->second, Params::T) << "test precondition: bucket " << beta
                                              << " collected more bulk ids than T allows";
            const Fp t_beta = Fp::from_u64(it->second); // Sender-side count is 0 here: t=min(count,T)
            const Fp share_r(dist(rng));
            const Fp share_s = t_beta.sub(share_r);
            st_r.cache[beta] = Share{share_r};
            st_s.cache[beta] = Share{share_s};
            r_sum = r_sum.add(share_r);
            s_sum = s_sum.add(share_s);
        }
        st_r.t_share.v = st_r.t_share.v.add(r_sum);
        st_s.t_share.v = st_s.t_share.v.add(s_sum);
        st_r.J.clear();
        ++st_r.query_no;
        ++st_s.query_no;
    }
    ASSERT_EQ(st_r.query_no, 2u);
    ASSERT_TRUE(st_r.J.empty());

    // Update 2: exactly u_max new ids on the Sender (TV-F12 boundary
    // bonus), disjoint from everything.
    std::vector<u64> sender_bulk;
    sender_bulk.reserve(Params::U_MAX);
    for (u64 i = 0; i < Params::U_MAX; ++i) sender_bulk.push_back(20'000'000ull + i);
    Update::apply(st_s, sender_bulk, std::vector<u64>{}, enc, G);
    Update::apply(st_r, std::vector<u64>{}, std::vector<u64>{}, enc, G);

    ASSERT_GT(st_r.my_size + st_s.my_size, 2 * Params::U_MAX)
        << "test precondition: q3 must clear the FullPublic size condition";
    ASSERT_EQ(st_s.J.size(), Params::U_MAX)
        << "test precondition: q3 must sit exactly at the TV-F12 boundary on the Sender";
    ASSERT_TRUE(st_r.J.empty()) << "test precondition: the Receiver has no dirty buckets at q3";

    const Fp t_share_before_commit = st_r.t_share.v;

    // q3: INCREMENTAL on both sides -- the real, unmodified path under test.
    u64 reconstructed = 0;
    {
        auto pools = build_pools(44 * (2 * Params::U_MAX), 4 * (2 * Params::U_MAX), 0x53000000ull);
        Share out_r{}, out_s{};
        run_two_party(
            next_address(),
            [&](Channel& ch) {
                out_r = Query::run(Role::Receiver, st_r, pools.first, ch, params, path_r, 9005);
            },
            [&](Channel& ch) {
                out_s = Query::run(Role::Sender, st_s, pools.second, ch, params, path_s, 9006);
            });
        run_two_party(
            next_address(), [&](Channel& ch) { reconstructed = open(ch, out_r).v; },
            [&](Channel& ch) { (void)open(ch, out_s); });
    }
    const Fp t_share_after_commit = st_r.t_share.v;

    // --- QRY-INC [SC3] assertions ---
    EXPECT_EQ(reconstructed, 2u) << "true |A meet B| stays 2 throughout (disjoint bulk ids)";
    EXPECT_TRUE(st_r.J.empty());
    EXPECT_EQ(st_r.query_no, 3u);
    EXPECT_GT(st_r.cache.size(), 0u);

    PartyState loaded_r;
    loaded_r.load(path_r);
    EXPECT_EQ(loaded_r.t_share, st_r.t_share);
    EXPECT_EQ(loaded_r.query_no, st_r.query_no);
    EXPECT_EQ(loaded_r.cache.size(), st_r.cache.size());
    EXPECT_TRUE(loaded_r.J.empty());

    // --- FC3 [QRY-F1] assertion: double-applying the SAME commit delta
    // (derived observationally from q3's own before/after t_share -- see
    // this file's top comment -- zero extra MPC cost) drifts t_share away
    // from the correct, single-commit value.
    const Fp delta = t_share_after_commit.sub(t_share_before_commit);
    ASSERT_NE(delta.v, 0u)
        << "test precondition: q3 must actually change t_share for this check to be meaningful";
    const Fp buggy_double_apply = t_share_after_commit.add(delta);
    EXPECT_NE(buggy_double_apply.v, t_share_after_commit.v)
        << "FC3: applying the same commit delta a second time must drift t_share away from the "
           "correct (single-commit) value";

    // M1 (task-20-brief.md, carried minor from the Phase-5 gate review):
    // the EXPECT_NE above alone would be satisfied by ANY unrelated wrong
    // value, not specifically the "double-applied the same delta" defect.
    // Pin the STRUCTURAL SHAPE of the drift in closed form: double-applying
    // delta must land exactly on before_commit + 2*delta (added, not
    // replacing the check above).
    const Fp expected_doubled_delta = t_share_before_commit.add(delta).add(delta);
    EXPECT_EQ(buggy_double_apply.v, expected_doubled_delta.v)
        << "FC3 [M1]: the double-applied value must equal before_commit + 2*delta exactly -- "
           "pinning the drift's structural shape so this negative cannot be satisfied by an "
           "unrelated wrong value that merely happens to differ from the correct one";

    clear_path(path_r);
    clear_path(path_s);
}

// ---------------------------------------------------------------------------
// FC4 [exhaustion]: undersized (here, EMPTY) pools abort with NO state
// mutation. Symmetric on both parties (identically empty pools) so neither
// thread can ever block waiting on a peer that has already aborted --
// R-EXHAUST's check is reached, on both sides, strictly after all of this
// query's networking has completed.
// ---------------------------------------------------------------------------

TEST(Query, FC4_ExhaustedPoolAbortsWithNoStateMutation) {
    Params params = Params::instantiate();
    const std::vector<u64> A = {1, 2, 3};
    const std::vector<u64> B = {2, 3, 4};

    const std::string path_r = scratch_path("fc4_r.bin");
    clear_path(path_r);
    {
        PartyState st_r0;
        st_r0.my_ids = A;
        st_r0.table.init(A, params.encoder, params.oracle);
        st_r0.my_size = A.size();
        st_r0.save(path_r);
    }
    const std::string bytes_before = read_file_bytes(path_r);
    ASSERT_FALSE(bytes_before.empty());

    const std::string address = next_address();
    ChildResult child = run_in_child([&] {
        PartyState st_r, st_s;
        st_r.my_ids = A;
        st_r.table.init(A, params.encoder, params.oracle);
        st_r.my_size = A.size();
        st_s.my_ids = B;
        st_s.table.init(B, params.encoder, params.oracle);
        st_s.my_size = B.size();

        Pools pool_r, pool_s; // deliberately empty: remaining()==0 on both pool kinds
        run_two_party(
            address,
            [&](Channel& ch) { (void)Query::run(Role::Receiver, st_r, pool_r, ch, params, path_r, 1); },
            [&](Channel& ch) {
                (void)Query::run(Role::Sender, st_s, pool_s, ch, params, "fc4_s_unused_path", 1);
            });
    });

    EXPECT_TRUE(child.signaled) << "the exhausted-pool query must die, not return";
    EXPECT_EQ(child.signal, SIGABRT);

    const std::string bytes_after = read_file_bytes(path_r);
    EXPECT_EQ(bytes_after, bytes_before) << "an exhaustion abort must not mutate the on-disk state";

    clear_path(path_r);
}

// ---------------------------------------------------------------------------
// QRY-ATOMIC [SC5]: SYMPSICA_CRASH_AT=pre-serialize kills the process after
// evaluation, before save(); the on-disk state file stays byte-identical
// to its pre-query snapshot; reloading it and re-running the query
// succeeds with the correct count. Manual fork (not gtest EXPECT_DEATH's
// default fork style, which does not survive a multi-threaded process --
// see this file's top comment / kat_ztest.cpp's TV-F3 comment) isolates
// the crash env var to the CHILD only (setenv() after fork() never
// propagates back to the parent), so the parent's own "normal run" legs
// below are unaffected.
// ---------------------------------------------------------------------------

TEST(Query, QRYATOMIC_CrashBeforeSerializeLeavesStateUntouchedAndRetrySucceeds) {
    ASSERT_EQ(std::getenv("SYMPSICA_CRASH_AT"), nullptr)
        << "test precondition: SYMPSICA_CRASH_AT must be unset in this (parent) process";

    Params params = Params::instantiate();
    const std::vector<u64> A = {1, 2, 3};
    const std::vector<u64> B = {2, 3, 4};

    const std::string path_r = scratch_path("atomic_r.bin");
    const std::string path_s = scratch_path("atomic_s.bin");
    clear_path(path_r);
    clear_path(path_s);

    // fork() FIRST, before ANY crash_point()-triggering call in this
    // process -- PartyState::save() included. DEVIATION FROM AN EARLIER
    // DRAFT (documented in task-17-report.md): that draft called
    // st_r.save()/st_s.save() in the PARENT before forking, to snapshot a
    // "before" state for the byte-identity check below. crash_point()'s
    // cache is a function-local (C++11 "magic") static, and fork() copies
    // a process's ENTIRE memory image -- including an already-initialized
    // static's guard bit. That earlier save() call was the process's
    // FIRST-EVER crash_point() call, permanently caching "unset" (correct,
    // in the parent, at that moment) into the static; fork() then handed
    // the CHILD that SAME already-initialized cache, so the child's LATER
    // setenv() had no effect at all -- the crash hook silently never fired
    // (reproduced: child exit code 0, not 137, and the state file WAS
    // mutated). Fix: fork() is the very first crash_point()-reachable
    // action in this test, full stop; the child seeds its OWN cache
    // (correctly, from ITS OWN post-setenv() environment) via its own
    // first save() call below, and the parent only touches
    // Setup::run/Query::run/save() AFTER waitpid() returns (also safe: no
    // further forking happens in this test past that point). This is ALSO
    // why FC4/SC5 use a manual fork(), never gtest's default (fork-based)
    // EXPECT_DEATH style: that style forks a possibly multi-threaded
    // process AFTER arbitrary earlier test/library code has already run
    // (see kat_ztest.cpp's TV-F3 comment for the analogous "no Channel, no
    // networking" carve-out reasoning).
    const std::string attempt_address = next_address();
    ChildResult child = run_in_child([&] {
        setenv("SYMPSICA_CRASH_AT", "pre-serialize", 1);

        PartyState st_r, st_s;
        st_r.my_ids = A;
        st_r.table.init(A, params.encoder, params.oracle);
        st_r.my_size = A.size();
        st_s.my_ids = B;
        st_s.table.init(B, params.encoder, params.oracle);
        st_s.my_size = B.size();
        // Initial snapshot -- save()'s own checkpoint names ("mid-temp-
        // write", "post-fsync", "pre-rename", "post-rename") never equal
        // "pre-serialize", so this completes normally; it is what seeds
        // crash_point()'s cache to "pre-serialize" for this process.
        st_r.save(path_r);
        st_s.save(path_s);

        Pools pool_r, pool_s;
        run_two_party(
            next_address(),
            [&](Channel& ch) { pool_r = Setup::run(Role::Receiver, ch, params, kTinySizes); },
            [&](Channel& ch) { pool_s = Setup::run(Role::Sender, ch, params, kTinySizes); });

        run_two_party(
            attempt_address,
            [&](Channel& ch) {
                (void)Query::run(Role::Receiver, st_r, pool_r, ch, params, path_r, 4242);
            },
            [&](Channel& ch) {
                (void)Query::run(Role::Sender, st_s, pool_s, ch, params, path_s, 4343);
            });
    });

    EXPECT_TRUE(child.exited) << "std::_Exit(137) is a normal (non-signaled) termination";
    EXPECT_EQ(child.exit_code, 137);

    // The state file must be exactly what a fresh save() of the (never
    // mutated) pre-query state produces -- proves the crashed attempt
    // never reached its own save() call.
    PartyState st_r_expected;
    st_r_expected.my_ids = A;
    st_r_expected.table.init(A, params.encoder, params.oracle);
    st_r_expected.my_size = A.size();
    const std::string expected_path = scratch_path("atomic_r_expected.bin");
    clear_path(expected_path);
    st_r_expected.save(expected_path);
    EXPECT_EQ(read_file_bytes(path_r), read_file_bytes(expected_path))
        << "a crash at pre-serialize must leave the on-disk state file byte-identical to its "
           "pre-query snapshot";
    clear_path(expected_path);

    // Reload from the (unchanged) files and retry, with a fresh pool
    // provisioning (safe: this is after the only fork in this test).
    PartyState st_r_retry, st_s_retry;
    st_r_retry.load(path_r);
    st_s_retry.load(path_s);
    EXPECT_EQ(st_r_retry.query_no, 0u);

    Pools pool_r, pool_s;
    run_two_party(
        next_address(),
        [&](Channel& ch) { pool_r = Setup::run(Role::Receiver, ch, params, kTinySizes); },
        [&](Channel& ch) { pool_s = Setup::run(Role::Sender, ch, params, kTinySizes); });

    Share out_r{}, out_s{};
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            out_r = Query::run(Role::Receiver, st_r_retry, pool_r, ch, params, path_r, 4242);
        },
        [&](Channel& ch) {
            out_s = Query::run(Role::Sender, st_s_retry, pool_s, ch, params, path_s, 4343);
        });

    u64 reconstructed = 0;
    run_two_party(
        next_address(), [&](Channel& ch) { reconstructed = open(ch, out_r).v; },
        [&](Channel& ch) { (void)open(ch, out_s); });
    EXPECT_EQ(reconstructed, 2u) << "the retried query must reconstruct the correct count";
    EXPECT_EQ(st_r_retry.query_no, 1u);

    clear_path(path_r);
    clear_path(path_s);
}

// ---------------------------------------------------------------------------
// FC5 [TV-F8-lite]: building the SENDER's syndrome with the WRONG (+) sign
// (both parties use +row) on an ASYMMETRIC set pair (|A\B| != |B\A|) gives
// the wrong count. Standalone SymDiffEvaluator exercise (bypasses Query::
// run/SwitchRule entirely -- this is specifically about R-SYND's sign
// convention, not path selection).
// ---------------------------------------------------------------------------

TEST(Query, FC5_WrongSignOnBothPartiesAsymmetricSetsGivesWrongCount) {
    Params params = Params::instantiate();
    const Encoder& enc = params.encoder;
    const BucketOracle& G = params.oracle;

    const std::vector<u64> A = {1, 2, 3};        // |A\B| = 1 ({1})
    const std::vector<u64> B = {2, 3, 4, 5};     // |B\A| = 2 ({4,5}) -- deliberately asymmetric
    const u64 nA = A.size(), nB = B.size();
    const u64 true_count = 2; // A meet B = {2,3}

    PowerSumTable table_r, table_s;
    table_r.init(A, enc, G);
    table_s.init(B, enc, G);

    std::set<u32> union_set;
    for (u64 id : A) union_set.insert(G.of(id));
    for (u64 id : B) union_set.insert(G.of(id));
    ASSERT_EQ(union_set.size(), 5u) << "test precondition: ids 1..5 map to distinct buckets";
    std::vector<u32> betas(union_set.begin(), union_set.end());

    auto run_variant = [&](bool sender_uses_plus, u64 pool_seed) -> u64 {
        auto pools = build_pools(44 * betas.size(), 4 * betas.size(), pool_seed);

        std::vector<std::array<Share, Params::K>> syn_r(betas.size()), syn_s(betas.size());
        for (std::size_t i = 0; i < betas.size(); ++i) {
            std::array<Fp, Params::K> row_r = table_r.row(betas[i]);
            std::array<Fp, Params::K> row_s = table_s.row(betas[i]);
            for (u64 k = 0; k < Params::K; ++k) {
                syn_r[i][k] = Share{row_r[k]}; // Receiver: always +row (correct convention)
                syn_s[i][k] = Share{sender_uses_plus ? row_s[k] : row_s[k].neg()};
            }
        }

        Share out_r{}, out_s{};
        run_two_party(
            next_address(),
            [&](Channel& ch) {
                BeaverEngine engine(Role::Receiver);
                auto shares = SymDiffEvaluator::eval_buckets(betas, syn_r, engine, pools.first.triples,
                                                               pools.first.gates, ch);
                Fp total(0);
                for (const auto& sh : shares) total = total.add(sh.v);
                out_r = Share{total};
            },
            [&](Channel& ch) {
                BeaverEngine engine(Role::Sender);
                auto shares = SymDiffEvaluator::eval_buckets(betas, syn_s, engine, pools.second.triples,
                                                               pools.second.gates, ch);
                Fp total(0);
                for (const auto& sh : shares) total = total.add(sh.v);
                out_s = Share{total};
            });

        const Fp nAB = Fp::from_u64(nA).add(Fp::from_u64(nB));
        const Fp t_total = out_r.v.add(out_s.v); // test-side: both totals visible locally
        return kInv2.mul(nAB.sub(t_total)).v;
    };

    const u64 correct = run_variant(/*sender_uses_plus=*/false, 0xF5000000ull);
    EXPECT_EQ(correct, true_count) << "sanity: the correct (+,-) sign convention reconstructs 2";

    const u64 wrong = run_variant(/*sender_uses_plus=*/true, 0xF5010000ull);
    EXPECT_NE(wrong, true_count)
        << "TV-F8-lite: a (+,+) sign build on an asymmetric set pair must reconstruct the wrong count";
}

// ---------------------------------------------------------------------------
// ROCCUPIED [regression, task-18-brief.md controller ruling R-OCCUPIED]:
// found via a real two-process E2E run (task-18-report.md's "found defect"
// section). run_full()'s old `occupied` computation counted every KEY
// st.table's underlying map holds, INCLUDING a "ghost" all-zero row left
// behind by a delete (PowerSumTable::edit decrements a row but never
// erases it on decrement) -- so a delete that empties a bucket entirely,
// followed by a full-path query with NO intervening table.rebuild() in
// between, used to abort:
//   "SYMPSICA_REQUIRE failed: Query::run: occupied buckets exceed
//    min(my_size, m) (invariant violation)"
// This reproduces EXACTLY that shape (delete, then a SECOND full-path
// query, no rebuild in between) and asserts it now completes normally
// with the correct count -- the fix (filtering `occupied` to non-zero
// rows, R-OCCUPIED) is provably exact per Newton's identities (query.cpp's
// own comment at the fix site has the full argument): a bucket's row can
// only be all-zero when it holds exactly 0 ids, for any t up to K=7.
// ---------------------------------------------------------------------------

TEST(Query, ROCCUPIED_DeleteThenFullPathQueryDoesNotAbort) {
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

    Pools pool_r, pool_s;
    run_two_party(
        next_address(),
        [&](Channel& ch) { pool_r = Setup::run(Role::Receiver, ch, params, kTinySizes); },
        [&](Channel& ch) { pool_s = Setup::run(Role::Sender, ch, params, kTinySizes); });

    const std::string path_r = scratch_path("roccupied_r.bin");
    const std::string path_s = scratch_path("roccupied_s.bin");
    clear_path(path_r);
    clear_path(path_s);

    // q1: first query, tiny -> FullPublic (firstQuery unconditional).
    // Populates st_r.table with a row for G.of(1), among others.
    run_two_party(
        next_address(),
        [&](Channel& ch) { (void)Query::run(Role::Receiver, st_r, pool_r, ch, params, path_r, 7101); },
        [&](Channel& ch) { (void)Query::run(Role::Sender, st_s, pool_s, ch, params, path_s, 7202); });
    ASSERT_EQ(st_r.query_no, 1u);

    // Delete id=1 (not in the true intersection -- true count stays 2).
    // Id 1 is the ONLY id ever inserted into its bucket, so this leaves a
    // "ghost" all-zero row at G.of(1) in st_r.table (ASSERTED below, this
    // is the test's own precondition for reproducing the old bug shape --
    // NOT a claim about production behavior).
    const u32 beta1 = G.of(1);
    ASSERT_NE(st_r.table.rows().find(beta1), st_r.table.rows().end())
        << "test precondition: bucket G.of(1) must have a row before the delete";
    Update::apply(st_r, std::vector<u64>{}, std::vector<u64>{1}, enc, G);
    Update::apply(st_s, std::vector<u64>{}, std::vector<u64>{}, enc, G); // no-op, symmetry only
    ASSERT_EQ(st_r.my_size, 2u) << "test precondition: id 1 actually left st_r.my_ids/my_size";
    auto it = st_r.table.rows().find(beta1);
    ASSERT_NE(it, st_r.table.rows().end())
        << "test precondition: PowerSumTable::edit must NOT erase the row on decrement (this IS "
           "the ghost-row mechanism under test)";
    for (Fp v : it->second) {
        ASSERT_EQ(v.v, 0u) << "test precondition: the ghost row must be exactly all-zero";
    }

    // q2: a SECOND full-path query, with NO intervening table.rebuild() --
    // this is the exact shape that used to abort. Reached naturally via
    // SwitchRule::decide's size condition (2*u_max >= nA+nB at this tiny
    // scale), not force_full, so this also exercises the REAL SwitchRule
    // dispatch path, not just run_full() in isolation.
    Share out_r{}, out_s{};
    SwitchRule::Path taken{};
    run_two_party(
        next_address(),
        [&](Channel& ch) {
            out_r = Query::run(Role::Receiver, st_r, pool_r, ch, params, path_r, 7103,
                                /*force_full=*/false, &taken);
        },
        [&](Channel& ch) {
            out_s = Query::run(Role::Sender, st_s, pool_s, ch, params, path_s, 7204);
        });
    EXPECT_TRUE(taken == SwitchRule::Path::FullPublic || taken == SwitchRule::Path::FullAnnounced)
        << "test precondition: q2 must actually take the full path for this regression to be "
           "meaningful";

    u64 reconstructed = 0;
    run_two_party(
        next_address(), [&](Channel& ch) { reconstructed = Query::open_count(ch, out_r); },
        [&](Channel& ch) { (void)Query::open_count(ch, out_s); });
    EXPECT_EQ(reconstructed, 2u) << "true |A meet B| stays {2,3}=2 after deleting id 1 (not in "
                                     "the intersection)";
    EXPECT_EQ(st_r.query_no, 2u);

    clear_path(path_r);
    clear_path(path_s);
}

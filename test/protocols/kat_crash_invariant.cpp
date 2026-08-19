// test/protocols/kat_crash_invariant.cpp — Task 19's FC3 [TV-F11]
// (task-19-brief.md W5.8, controller ruling R-TVF11): "commit accumulate
// before all |J| evaluations done" is a WRONG-ORDER construction that does
// not exist in the shipped code (Task 17 puts commit() strictly after
// eval_union returns in full). R-TVF11 authorizes a TEST-LOCAL
// reimplementation of the wrong order; this file evaluates each bucket in
// the union ONE AT A TIME (instead of query.cpp's real batched eval_union)
// and immediately merges + saves each bucket's result into PartyState as it
// completes -- i.e. exactly the ordering FT6/commit()'s doc comment forbids
// -- stopping partway through the union. The resulting on-disk bytes are
// asserted to be NEITHER the correct pre-query state NOR the correct
// post-query state (both captured from real, unmodified Query::run calls):
// this is the R-INVCHK "never a mixture" check DETECTING a torn state that
// the real, atomic commit() order can never produce (SC3's own five crash
// points -- test/e2e/run_crash_matrix.py -- independently confirm the
// CORRECT order never produces such a mismatch, at every one of its five
// kill points).

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "sympsica/core/pools.hpp"
#include "sympsica/core/share.hpp"
#include "sympsica/core/state.hpp"
#include "sympsica/core/table.hpp"
#include "sympsica/gates/beaver.hpp"
#include "sympsica/gates/symdiff.hpp"
#include "sympsica/protocols/query.hpp"
#include "sympsica/protocols/setup.hpp"
#include "sympsica/utils/field.hpp"
#include "sympsica/utils/net.hpp"
#include "sympsica/utils/params.hpp"

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

std::string scratch_path(const std::string& name) {
    return (std::filesystem::temp_directory_path() / ("sympsica_kat_crashinv_" + name)).string();
}

void clear_path(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + ".tmp").c_str());
}

std::string read_file_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// WRONG-ORDER reimplementation (R-TVF11): evaluates `union_set` ONE BUCKET
// AT A TIME and immediately accumulates + SAVES after each one (instead of
// query.cpp's real order: evaluate the WHOLE union first, THEN commit+save
// exactly once). Stops after `stop_after` buckets have been merged+saved
// (simulating a crash right there) -- deliberately never reaches the last
// bucket(s), producing a state that reflects SOME but not all of the
// query's true delta.
void wrong_order_partial_commit(Role role, PartyState& st, Pools& pools, Channel& ch,
                                  const std::set<u32>& union_set, std::size_t stop_after,
                                  const std::string& state_path) {
    std::vector<u32> betas(union_set.begin(), union_set.end());
    std::size_t done = 0;
    for (u32 beta : betas) {
        if (done >= stop_after) break;

        std::array<Fp, Params::K> row = st.table.row(beta);
        std::array<Share, Params::K> syndrome{};
        for (u64 k = 0; k < Params::K; ++k) {
            syndrome[k] = Share{role == Role::Sender ? row[k].neg() : row[k]};
        }

        BeaverEngine engine(role);
        std::vector<u32> one_beta{beta};
        std::vector<std::array<Share, Params::K>> one_syn{syndrome};
        std::vector<Share> new_share =
            SymDiffEvaluator::eval_buckets(one_beta, one_syn, engine, pools.triples, pools.gates, ch);
        ASSERT_EQ(new_share.size(), 1u);

        // R-TVF11: accumulate + SAVE THIS ONE BUCKET IMMEDIATELY -- the
        // wrong order under test. A real crash between two such per-bucket
        // commits would leave exactly this shape on disk.
        Fp old_val(0);
        auto it = st.cache.find(beta);
        if (it != st.cache.end()) old_val = it->second.v;
        Fp delta = new_share[0].v.sub(old_val);
        st.cache[beta] = new_share[0];
        st.t_share.v = st.t_share.v.add(delta);
        st.save(state_path); // NOT preceded by J.clear()/query_no++ -- another torn-ness axis
        ++done;
    }
}

} // namespace

TEST(CrashInvariant, FC3_TVF11_WrongOrderMidQueryCommitProducesDetectablyTornState) {
    Params params = Params::instantiate();
    const Encoder& enc = params.encoder;
    const BucketOracle& G = params.oracle;

    const std::vector<u64> A = {1, 2, 3, 4, 5};
    const std::vector<u64> B = {3, 4, 5, 6, 7};

    PartyState st_r0, st_s0;
    st_r0.my_ids = A;
    st_r0.table.init(A, enc, G);
    st_r0.my_size = A.size();
    st_s0.my_ids = B;
    st_s0.table.init(B, enc, G);
    st_s0.my_size = B.size();

    const std::string path_r = scratch_path("r.bin");
    const std::string path_s = scratch_path("s.bin");
    clear_path(path_r);
    clear_path(path_s);

    Pools pool_r, pool_s;
    run_two_party(
        "127.0.0.1:49960",
        [&](Channel& ch) { pool_r = Setup::run(Role::Receiver, ch, params, PoolSizes{2000, 200}); },
        [&](Channel& ch) { pool_s = Setup::run(Role::Sender, ch, params, PoolSizes{2000, 200}); });

    // q1: real, correct, first query (FullPublic) -- establishes the "pre"
    // golden for q2 (both st_r0/st_s0 mutated in place and saved).
    run_two_party(
        "127.0.0.1:49961",
        [&](Channel& ch) { (void)Query::run(Role::Receiver, st_r0, pool_r, ch, params, path_r, 20001); },
        [&](Channel& ch) { (void)Query::run(Role::Sender, st_s0, pool_s, ch, params, path_s, 20002); });
    ASSERT_EQ(st_r0.query_no, 1u);
    const std::string pre_bytes_r = read_file_bytes(path_r);
    ASSERT_FALSE(pre_bytes_r.empty());

    // Delete one id (id=1, not in the true intersection) so q2's union
    // isn't trivially identical to q1's -- exercises the SAME R-OCCUPIED
    // ghost-row shape as kat_query.cpp's own regression, incidentally.
    st_r0.my_ids.erase(st_r0.my_ids.begin());
    st_r0.table.edit(1, -1, enc, G);
    st_r0.my_size = 4;
    st_r0.J.insert(G.of(1));

    // Compute q2's true union analytically (both parties' occupied buckets
    // -- known since ids are hardcoded): mirrors run_full()'s R-OCCUPIED
    // filter (row is non-zero), matching what the real full-path query
    // would evaluate.
    std::set<u32> union_set;
    for (const auto& [beta, row] : st_r0.table.rows()) {
        bool nz = false;
        for (Fp v : row)
            if (v.v != 0) nz = true;
        if (nz) union_set.insert(beta);
    }
    for (const auto& [beta, row] : st_s0.table.rows()) {
        bool nz = false;
        for (Fp v : row)
            if (v.v != 0) nz = true;
        if (nz) union_set.insert(beta);
    }
    ASSERT_GE(union_set.size(), 4u) << "test precondition: q2's union must have enough buckets to "
                                        "meaningfully stop partway through it";

    // --- CORRECT order (golden "post"): a fresh copy of the pre-q2 state,
    // run through the REAL, unmodified Query::run (atomic commit). ---
    PartyState st_r_correct = st_r0, st_s_correct = st_s0;
    const std::string path_r_correct = scratch_path("r_correct.bin");
    clear_path(path_r_correct);
    st_r_correct.save(path_r_correct); // seed the correct-order copy's own on-disk file
    run_two_party(
        "127.0.0.1:49962",
        [&](Channel& ch) {
            (void)Query::run(Role::Receiver, st_r_correct, pool_r, ch, params, path_r_correct, 20003);
        },
        [&](Channel& ch) {
            std::string dummy_s = scratch_path("s_correct_unused.bin");
            (void)Query::run(Role::Sender, st_s_correct, pool_s, ch, params, dummy_s, 20004);
        });
    const std::string post_bytes_r = read_file_bytes(path_r_correct);
    ASSERT_NE(post_bytes_r, pre_bytes_r) << "test precondition: q2 must actually change the "
                                             "receiver's on-disk state for this check to be "
                                             "meaningful";

    // --- WRONG order (torn state under test): another fresh copy, stopped
    // partway through the SAME union via wrong_order_partial_commit(). ---
    PartyState st_r_wrong = st_r0, st_s_wrong = st_s0;
    const std::string path_r_wrong = scratch_path("r_wrong.bin");
    clear_path(path_r_wrong);
    const std::size_t stop_after = union_set.size() > 2 ? union_set.size() - 2 : 1;
    run_two_party(
        "127.0.0.1:49963",
        [&](Channel& ch) {
            wrong_order_partial_commit(Role::Receiver, st_r_wrong, pool_r, ch, union_set, stop_after,
                                        path_r_wrong);
        },
        [&](Channel& ch) {
            wrong_order_partial_commit(Role::Sender, st_s_wrong, pool_s, ch, union_set, stop_after,
                                        "unused_sender_path_wrong.bin");
        });
    const std::string torn_bytes_r = read_file_bytes(path_r_wrong);
    ASSERT_FALSE(torn_bytes_r.empty());

    // --- R-INVCHK, applied to the torn state: it must match NEITHER golden
    // -- exactly the "never a mixture" detector SC3's crash matrix relies
    // on, here proving it actually FIRES on a wrong-order construction. ---
    EXPECT_NE(torn_bytes_r, pre_bytes_r)
        << "FC3 [TV-F11]: the wrong-order partial commit must NOT be byte-identical to the "
           "pre-query state (it DID mutate some buckets before 'crashing')";
    EXPECT_NE(torn_bytes_r, post_bytes_r)
        << "FC3 [TV-F11]: the wrong-order partial commit must NOT be byte-identical to the "
           "post-query state either (it did NOT finish all evaluations) -- this is the torn "
           "state that R-INVCHK's pre-XOR-post check detects as a FAILURE, which the real "
           "atomic commit() order (Query::run, exercised by SC3's five real crash points) can "
           "never produce";

    clear_path(path_r);
    clear_path(path_s);
    clear_path(path_r_correct);
    clear_path(path_r_wrong);
}

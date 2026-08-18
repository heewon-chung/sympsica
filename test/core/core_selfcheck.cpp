// test/core/core_selfcheck.cpp — Task 10's own quick self-checks for
// core/{share,table,state,pools} + protocols/update (task-10-brief.md).
// Task 11 owns the formal KAT suite in this same directory; this file is
// deliberately scoped to the specific checks task-10-brief.md's
// "Verification expected from you" section names, not a full test suite.

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "sympsica/core/pools.hpp"
#include "sympsica/core/state.hpp"
#include "sympsica/core/table.hpp"
#include "sympsica/protocols/update.hpp"
#include "sympsica/utils/encoding.hpp"
#include "sympsica/utils/field.hpp"
#include "sympsica/utils/params.hpp"

using namespace sympsica;

namespace {

// System temp dir (not the source tree): save()/load() are exercised
// against a scratch file, unlike test/utils/fixture_support.hpp's fixtures,
// which are read-only inputs resolved against SYMPSICA_SOURCE_DIR.
std::string scratch_path(const std::string& name) {
    return (std::filesystem::temp_directory_path() / ("sympsica_core_selfcheck_" + name)).string();
}

} // namespace

// --- TBL-3: edit +1 then -1 returns exact prior state, incl. absent-row ---
TEST(CoreSelfCheck, TBL3_EditPlusMinusIdentity) {
    Params params = Params::instantiate();
    const Encoder& enc = params.encoder;
    const BucketOracle& G = params.oracle;

    PowerSumTable table;
    u32 beta = G.of(42);

    // Absent-row case: row is zero before and after a +1/-1 cancel.
    auto before = table.row(beta);
    for (auto f : before) EXPECT_EQ(f, Fp(0));

    table.edit(42, +1, enc, G);
    table.edit(42, -1, enc, G);

    auto after = table.row(beta);
    ASSERT_EQ(before.size(), after.size());
    for (std::size_t k = 0; k < before.size(); ++k) {
        EXPECT_EQ(after[k], before[k]) << "k=" << k;
    }

    // Non-trivial prior-state case: edit(+1) for a second id first, then
    // verify a third id's +1/-1 pair returns exactly to that prior row.
    PowerSumTable table2;
    table2.edit(7, +1, enc, G); // unrelated id -- likely a different bucket
    auto prior = table2.row(G.of(99));
    table2.edit(99, +1, enc, G);
    table2.edit(99, -1, enc, G);
    auto restored = table2.row(G.of(99));
    for (std::size_t k = 0; k < prior.size(); ++k) {
        EXPECT_EQ(restored[k], prior[k]) << "k=" << k;
    }
}

// --- TBL-2 sanity: row(beta)[0] == sigma(id) for a single-id init ---------
TEST(CoreSelfCheck, TBL2_RowZeroIsSigmaForSingleId) {
    Params params = Params::instantiate();
    const Encoder& enc = params.encoder;
    const BucketOracle& G = params.oracle;

    u64 id = 12345;
    PowerSumTable table;
    table.init(std::vector<u64>{id}, enc, G);

    auto row = table.row(G.of(id));
    EXPECT_EQ(row[0], enc.sigma(id));
}

// --- save() -> load() round trip equality ----------------------------------
TEST(CoreSelfCheck, SaveLoadRoundTrip) {
    Params params = Params::instantiate();
    const Encoder& enc = params.encoder;
    const BucketOracle& G = params.oracle;

    PartyState st;
    st.my_ids = {1, 2, 3, 1000000};
    st.table.init(st.my_ids, enc, G);
    st.J = {G.of(1), G.of(2), G.of(1000000)};
    st.cache[G.of(1)] = Share{Fp(111)};
    st.cache[G.of(2)] = Share{Fp(222)};
    st.t_share = Share{Fp(999)};
    st.my_size = 4;

    std::string path = scratch_path("state.bin");
    std::remove(path.c_str());
    std::remove((path + ".tmp").c_str());
    st.save(path);

    PartyState loaded;
    loaded.load(path);

    EXPECT_EQ(loaded.my_ids, st.my_ids);
    EXPECT_EQ(loaded.J, st.J);
    ASSERT_EQ(loaded.cache.size(), st.cache.size());
    for (const auto& [beta, sh] : st.cache) {
        auto it = loaded.cache.find(beta);
        ASSERT_NE(it, loaded.cache.end());
        EXPECT_EQ(it->second, sh);
    }
    EXPECT_EQ(loaded.t_share, st.t_share);
    EXPECT_EQ(loaded.my_size, st.my_size);
    EXPECT_TRUE(loaded.check_against(st.my_ids, enc, G));

    std::remove(path.c_str());
}

// --- TriplePool: take() / take_by_id() double-consume aborts ---------------
TEST(CoreSelfCheckDeathTest, TriplePool_TakeByIdDoubleConsumeAborts) {
    TriplePool pool;
    std::vector<Triple> items;
    items.push_back(Triple{1, Share{Fp(1)}, Share{Fp(2)}, Share{Fp(3)}});
    items.push_back(Triple{2, Share{Fp(4)}, Share{Fp(5)}, Share{Fp(6)}});
    pool.refill(std::move(items));

    EXPECT_EQ(pool.remaining(), 2u);
    Triple t1 = pool.take_by_id(1);
    EXPECT_EQ(t1.corr_id, 1u);
    EXPECT_EQ(pool.remaining(), 1u);
    EXPECT_EQ(pool.consumed_ids().size(), 1u);

    // TV-F10: a second transition of the SAME id must abort.
    EXPECT_DEATH({ pool.take_by_id(1); }, "already consumed");

    // generated == consumed + remaining (unused high-water items legal).
    EXPECT_EQ(pool.generated(), pool.consumed_ids().size() + pool.remaining());
}

TEST(CoreSelfCheckDeathTest, TriplePool_TakeOnEmptyPoolAborts) {
    TriplePool pool;
    EXPECT_DEATH({ pool.take(); }, "pool exhausted");
}

TEST(CoreSelfCheck, TriplePool_TakeFifoOrderAndTranscript) {
    TriplePool pool;
    std::vector<Triple> items;
    items.push_back(Triple{10, Share{Fp(1)}, Share{Fp(1)}, Share{Fp(1)}});
    items.push_back(Triple{20, Share{Fp(2)}, Share{Fp(2)}, Share{Fp(2)}});
    items.push_back(Triple{30, Share{Fp(3)}, Share{Fp(3)}, Share{Fp(3)}});
    pool.refill(std::move(items));

    EXPECT_EQ(pool.generated(), 3u);
    Triple a = pool.take();
    Triple b = pool.take();
    EXPECT_EQ(a.corr_id, 10u);
    EXPECT_EQ(b.corr_id, 20u);
    EXPECT_EQ(pool.remaining(), 1u);
    EXPECT_EQ(pool.generated(), pool.consumed_ids().size() + pool.remaining());
}

// --- Update filter skip cases: present insert / absent delete / same-epoch pair
TEST(CoreSelfCheck, Update_SkipPresentInsert) {
    Params params = Params::instantiate();
    const Encoder& enc = params.encoder;
    const BucketOracle& G = params.oracle;

    PartyState st;
    st.my_ids = {5};
    st.table.init(st.my_ids, enc, G);
    st.my_size = 1;
    auto table_before = st.table.rows();
    auto j_before = st.J;

    std::vector<u64> I = {5}; // already present -- must be skipped
    std::vector<u64> D = {};
    Update::apply(st, I, D, enc, G);

    EXPECT_EQ(st.my_ids, std::vector<u64>{5});
    EXPECT_EQ(st.my_size, 1u);
    EXPECT_EQ(st.J, j_before); // UPD-4: not even J is touched
    EXPECT_EQ(st.table.rows(), table_before);
}

TEST(CoreSelfCheck, Update_SkipAbsentDelete) {
    Params params = Params::instantiate();
    const Encoder& enc = params.encoder;
    const BucketOracle& G = params.oracle;

    PartyState st;
    st.my_ids = {};
    st.my_size = 0;
    auto j_before = st.J;

    std::vector<u64> I = {};
    std::vector<u64> D = {77}; // not present -- must be skipped
    Update::apply(st, I, D, enc, G);

    EXPECT_TRUE(st.my_ids.empty());
    EXPECT_EQ(st.my_size, 0u);
    EXPECT_EQ(st.J, j_before);
    EXPECT_TRUE(st.table.rows().empty());
}

TEST(CoreSelfCheck, Update_SkipSameEpochPair) {
    Params params = Params::instantiate();
    const Encoder& enc = params.encoder;
    const BucketOracle& G = params.oracle;

    PartyState st;
    st.my_ids = {};
    st.my_size = 0;
    auto j_before = st.J;

    std::vector<u64> I = {88};
    std::vector<u64> D = {88}; // same-epoch pair -- must touch nothing
    Update::apply(st, I, D, enc, G);

    EXPECT_TRUE(st.my_ids.empty());
    EXPECT_EQ(st.my_size, 0u);
    EXPECT_EQ(st.J, j_before);
    EXPECT_TRUE(st.table.rows().empty());
}

TEST(CoreSelfCheck, Update_NormalInsertAndDeleteApply) {
    Params params = Params::instantiate();
    const Encoder& enc = params.encoder;
    const BucketOracle& G = params.oracle;

    PartyState st;
    st.my_ids = {1};
    st.table.init(st.my_ids, enc, G);
    st.my_size = 1;

    std::vector<u64> I = {2}; // not present -- applies
    std::vector<u64> D = {1}; // present -- applies
    Update::apply(st, I, D, enc, G);

    EXPECT_EQ(st.my_ids, std::vector<u64>{2});
    EXPECT_EQ(st.my_size, 1u);
    EXPECT_TRUE(st.J.count(G.of(1)));
    EXPECT_TRUE(st.J.count(G.of(2)));
    EXPECT_TRUE(st.check_against(st.my_ids, enc, G));
}

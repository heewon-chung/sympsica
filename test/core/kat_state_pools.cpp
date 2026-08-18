// test/core/kat_state_pools.cpp — Task 11's formal Phase-3b KAT suite for
// core/{state,pools}.hpp (task-11-brief.md, W3.7): PartyState save/load
// KATs beyond Task 10's own self-checks, TV-F10 (both flavors: TriplePool
// AND ZtGatePool, plus the take()-then-take_by_id() identity-vs-counter
// distinction, FT5), and R-REFILL's cross-refill corr_id uniqueness
// regression test (Task-10 review minor -- code existed, was untested).

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "sympsica/core/pools.hpp"
#include "sympsica/core/state.hpp"
#include "sympsica/core/table.hpp"
#include "sympsica/utils/encoding.hpp"
#include "sympsica/utils/field.hpp"
#include "sympsica/utils/params.hpp"
#include "../utils/fixture_support.hpp"

using namespace sympsica;

namespace {

std::string scratch_path(const std::string& name) {
    return (std::filesystem::temp_directory_path() / ("sympsica_kat_state_pools_" + name)).string();
}

} // namespace

// --- PartyState save/load: empty state round trip -------------------------
TEST(StatePools, SaveLoadRoundTrip_EmptyState) {
    Params params = Params::instantiate();

    PartyState st;
    st.t_share = Share{Fp(0)}; // avoid comparing an uninitialized default Fp

    std::string path = scratch_path("empty_state.bin");
    std::remove(path.c_str());
    std::remove((path + ".tmp").c_str());
    st.save(path);

    PartyState loaded;
    loaded.load(path);

    EXPECT_TRUE(loaded.my_ids.empty());
    EXPECT_TRUE(loaded.J.empty());
    EXPECT_TRUE(loaded.cache.empty());
    EXPECT_EQ(loaded.t_share, st.t_share);
    EXPECT_EQ(loaded.my_size, 0u);
    EXPECT_TRUE(loaded.check_against(std::vector<u64>{}, params.encoder, params.oracle));

    std::remove(path.c_str());
}

// --- PartyState save/load: larger, seeded state round trip ----------------
TEST(StatePools, SaveLoadRoundTrip_ManyIdsSeeded) {
    Params params = Params::instantiate();
    const Encoder& enc = params.encoder;
    const BucketOracle& G = params.oracle;

    for (u64 seed = 0; seed <= 2; ++seed) { // keep the whole-suite runtime budget in mind
        SCOPED_TRACE(::testing::Message() << "seed=" << seed);
        u64 state = seed ^ 0x1111111111111111ull;

        PartyState st;
        for (int i = 0; i < 50; ++i) {
            st.my_ids.push_back(sympsica_test::splitmix64_next(state) % (1ull << 60));
        }
        st.table.init(st.my_ids, enc, G);
        for (u64 id : st.my_ids) st.J.insert(G.of(id));
        for (int i = 0; i < 10; ++i) {
            u32 beta = static_cast<u32>(sympsica_test::splitmix64_next(state) % 1000000 + 1);
            st.cache[beta] = Share{Fp::from_u64(sympsica_test::splitmix64_next(state))};
        }
        st.t_share = Share{Fp::from_u64(sympsica_test::splitmix64_next(state))};
        st.my_size = st.my_ids.size();

        std::string path = scratch_path("many_ids_seed" + std::to_string(seed) + ".bin");
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
}

// --- TV-F10, flavor 1 (ZtGatePool): the double-take_by_id case Task 10's
// self-check already covers for TriplePool -- this repeats it for
// ZtGatePool ("one named triple, one named gate", per the doc row).
TEST(StatePoolsDeathTest, TVF10_ZtGate_DoubleTakeByIdAborts) {
    ZtGatePool pool;
    std::vector<ZtGate> items;
    items.push_back(ZtGate{1, Share{Fp(1)}, oc::RegularDpfKey{}});
    items.push_back(ZtGate{2, Share{Fp(2)}, oc::RegularDpfKey{}});
    pool.refill(std::move(items));

    EXPECT_EQ(pool.remaining(), 2u);
    ZtGate g1 = pool.take_by_id(1);
    EXPECT_EQ(g1.corr_id, 1u);
    EXPECT_EQ(pool.remaining(), 1u);

    EXPECT_DEATH({ pool.take_by_id(1); }, "already consumed");
}

// --- TV-F10, flavor 2: identity-based enforcement, not counter equality
// (FT5). Consume corr_id 7 via the ORDINARY FIFO take() (not take_by_id),
// then attempt take_by_id(7) on that SAME id -- it must still abort, even
// though "the pool has one fewer item" (a counter-based check) would not
// by itself catch a same-id re-consumption via a different code path.
TEST(StatePoolsDeathTest, TVF10_TakeThenTakeByIdSameIdAborts) {
    TriplePool pool;
    std::vector<Triple> items;
    items.push_back(Triple{7, Share{Fp(1)}, Share{Fp(2)}, Share{Fp(3)}});
    pool.refill(std::move(items));

    Triple t = pool.take(); // FIFO consume, NOT take_by_id
    EXPECT_EQ(t.corr_id, 7u);

    EXPECT_DEATH({ pool.take_by_id(7); }, "already consumed");
}

// --- R-REFILL (Task-10 review ruling, binding): refill() with a corr_id
// already seen in a PREVIOUS refill -- including an already-consumed one --
// must abort (cross-refill uniqueness; code existed in pools.cpp, was
// untested until now).
TEST(StatePoolsDeathTest, RREFILL_DuplicateCorrIdAfterConsumeAborts) {
    TriplePool pool;
    std::vector<Triple> first;
    first.push_back(Triple{5, Share{Fp(1)}, Share{Fp(1)}, Share{Fp(1)}});
    pool.refill(std::move(first));
    Triple t = pool.take_by_id(5); // consumed
    (void)t;

    std::vector<Triple> second;
    second.push_back(Triple{5, Share{Fp(9)}, Share{Fp(9)}, Share{Fp(9)}}); // SAME corr_id, later refill
    EXPECT_DEATH({ pool.refill(std::move(second)); }, "duplicate corr_id");
}

TEST(StatePoolsDeathTest, RREFILL_DuplicateCorrIdWhileStillAvailableAborts) {
    TriplePool pool;
    std::vector<Triple> first;
    first.push_back(Triple{6, Share{Fp(1)}, Share{Fp(1)}, Share{Fp(1)}});
    pool.refill(std::move(first)); // id 6 available, never taken

    std::vector<Triple> second;
    second.push_back(Triple{6, Share{Fp(2)}, Share{Fp(2)}, Share{Fp(2)}});
    EXPECT_DEATH({ pool.refill(std::move(second)); }, "duplicate corr_id");
}

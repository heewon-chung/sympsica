// test/protocols/kat_update.cpp — Task 11's formal Phase-3b KAT suite for
// protocols/update.hpp (task-11-brief.md, W3.7): UPD-1..5 + TV-F6
// (negative-as-positive-assert) + a schedule-replay consumption of R-SIM's
// epoch-schedule fixtures (update-path state check, per R-SIM's forward-
// looking-scope note).
//
// CMake naming note (requirement 1): this directory is registered via
// `sympsica_add_test_target(protocols update)` in CMakeLists.txt, which
// gives every test discovered here the TEST_PREFIX "update." (not
// "protocols.") specifically so the binding SC selector
// `ctest -R 'core|update'` matches (case-sensitive; "protocols.Update..."
// would not contain lowercase "update").
//
// Requirement 3: UPD-1 (and the schedule replay) compare C++ state against
// reference.py-emitted fixture values; UPD-2/3/4/5 are pure C++
// (equivalence/no-op/identity assertions), no fixture needed.

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "sympsica/core/state.hpp"
#include "sympsica/core/table.hpp"
#include "sympsica/protocols/update.hpp"
#include "sympsica/utils/encoding.hpp"
#include "sympsica/utils/field.hpp"
#include "sympsica/utils/params.hpp"
#include "../utils/fixture_support.hpp"

using namespace sympsica;

namespace {

std::vector<u64> read_id_list(const sympsica_test::Fixture& fx, const std::string& key) {
    auto rows = fx.all(key);
    std::vector<u64> out;
    if (rows.empty()) return out;
    for (const auto& s : rows.at(0)) out.push_back(std::stoull(s));
    return out;
}

} // namespace

// --- UPD-1 (R-UPD, provisional row): fresh inserts + valid deletes, cross-
// checked against the upd1_* fixture rows reference.py emits (my_ids,
// table rows via check_against, J, my_size all checked).
TEST(Update, UPD1_FreshInsertsValidDeletesMatchReferencePy) {
    Params params = Params::instantiate();
    const Encoder& enc = params.encoder;
    const BucketOracle& G = params.oracle;

    for (int seed = 0; seed <= 9; ++seed) {
        SCOPED_TRACE(::testing::Message() << "seed=" << seed);
        sympsica_test::Fixture fx(sympsica_test::fixture_path(
            "test/fixtures/seed" + std::to_string(seed) + ".fixture"));

        u64 n_init = fx.u64_at("upd1_n_init");
        std::vector<u64> init_ids = read_id_list(fx, "upd1_init_ids");
        ASSERT_EQ(init_ids.size(), n_init);

        std::vector<u64> I = read_id_list(fx, "upd1_I");
        std::vector<u64> D = read_id_list(fx, "upd1_D");
        std::vector<u64> Iprime = read_id_list(fx, "upd1_Iprime");
        std::vector<u64> Dprime = read_id_list(fx, "upd1_Dprime");
        std::vector<u64> expected_final = read_id_list(fx, "upd1_expected_final_ids");
        u64 expected_my_size = fx.u64_at("upd1_expected_my_size");

        // Fixture invariant (documented in ref/reference.py: UPD-1 is
        // "basic apply", not a filter-corner-case KAT): I'==I, D'==D.
        EXPECT_EQ(Iprime, I);
        EXPECT_EQ(Dprime, D);

        PartyState st;
        st.my_ids = init_ids;
        st.table.init(st.my_ids, enc, G);
        st.my_size = init_ids.size();

        Update::apply(st, I, D, enc, G);

        std::vector<u64> actual_final = st.my_ids;
        std::sort(actual_final.begin(), actual_final.end());
        EXPECT_EQ(actual_final, expected_final);
        EXPECT_EQ(st.my_size, expected_my_size);
        EXPECT_TRUE(st.check_against(expected_final, enc, G));

        std::set<u32> expected_J;
        for (u64 x : I) expected_J.insert(G.of(x));
        for (u64 x : D) expected_J.insert(G.of(x));
        EXPECT_EQ(st.J, expected_J);
    }
}

// --- UPD-2 (R-UPD): modification == delete+insert. Applying {delete x,
// insert x'} in ONE epoch must equal sequential delete-then-insert across
// two separate Update::apply calls.
TEST(Update, UPD2_ModificationEqualsSequentialDeleteThenInsert) {
    Params params = Params::instantiate();
    const Encoder& enc = params.encoder;
    const BucketOracle& G = params.oracle;

    const std::vector<u64> initial = {100, 200, 300};

    PartyState st_oneshot;
    st_oneshot.my_ids = initial;
    st_oneshot.table.init(st_oneshot.my_ids, enc, G);
    st_oneshot.my_size = initial.size();
    Update::apply(st_oneshot, std::vector<u64>{250}, std::vector<u64>{200}, enc, G);

    PartyState st_seq;
    st_seq.my_ids = initial;
    st_seq.table.init(st_seq.my_ids, enc, G);
    st_seq.my_size = initial.size();
    Update::apply(st_seq, std::vector<u64>{}, std::vector<u64>{200}, enc, G);
    Update::apply(st_seq, std::vector<u64>{250}, std::vector<u64>{}, enc, G);

    std::vector<u64> a = st_oneshot.my_ids;
    std::vector<u64> b = st_seq.my_ids;
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());

    EXPECT_EQ(a, b);
    EXPECT_EQ(st_oneshot.my_size, st_seq.my_size);
    EXPECT_EQ(st_oneshot.J, st_seq.J);
    EXPECT_TRUE(st_oneshot.check_against(a, enc, G));
    EXPECT_TRUE(st_seq.check_against(b, enc, G));
}

// --- UPD-3 (FT1, R-UPD): the filter triple -- insert-of-present skipped,
// delete-of-absent skipped, same-epoch insert+delete pair dropped from
// BOTH (checked against the raw, deduped I/D lists per R-DUP, not the
// structurally-always-disjoint I'/D').
TEST(Update, UPD3_FilterTripleSkipsAll) {
    Params params = Params::instantiate();
    const Encoder& enc = params.encoder;
    const BucketOracle& G = params.oracle;

    { // (a) insert-of-present: skipped.
        PartyState st;
        st.my_ids = {11};
        st.table.init(st.my_ids, enc, G);
        st.my_size = 1;
        auto table_before = st.table.rows();
        auto j_before = st.J;

        Update::apply(st, std::vector<u64>{11}, std::vector<u64>{}, enc, G);

        EXPECT_EQ(st.my_ids, std::vector<u64>{11});
        EXPECT_EQ(st.my_size, 1u);
        EXPECT_EQ(st.J, j_before);
        EXPECT_EQ(st.table.rows(), table_before);
    }

    { // (b) delete-of-absent: skipped.
        PartyState st;
        auto j_before = st.J;

        Update::apply(st, std::vector<u64>{}, std::vector<u64>{22}, enc, G);

        EXPECT_TRUE(st.my_ids.empty());
        EXPECT_EQ(st.my_size, 0u);
        EXPECT_EQ(st.J, j_before);
        EXPECT_TRUE(st.table.rows().empty());
    }

    { // (c) same-epoch pair (33 in BOTH raw I and raw D, not currently
      // present): dropped from both.
        PartyState st;
        auto j_before = st.J;

        Update::apply(st, std::vector<u64>{33}, std::vector<u64>{33}, enc, G);

        EXPECT_TRUE(st.my_ids.empty());
        EXPECT_EQ(st.my_size, 0u);
        EXPECT_EQ(st.J, j_before);
        EXPECT_TRUE(st.table.rows().empty());
    }
}

// --- UPD-4 (R-UPD): skipped/dropped elements touch NOTHING -- not even J.
// UPD-3's three sub-cases already each assert st.J unchanged individually;
// this KAT states the invariant explicitly, in one MIXED epoch combining
// all three skip reasons alongside real activity, so "nothing else leaks
// in" is checked in the presence of genuine inserts/deletes too.
TEST(Update, UPD4_SkippedElementsTouchNothingIncludingJ) {
    Params params = Params::instantiate();
    const Encoder& enc = params.encoder;
    const BucketOracle& G = params.oracle;

    PartyState st;
    st.my_ids = {1, 2}; // 1: insert-of-present target; 2: real delete target
    st.table.init(st.my_ids, enc, G);
    st.my_size = 2;

    std::vector<u64> I = {1, 3, 44}; // 1 skip(present); 3 same-epoch pair; 44 real insert
    std::vector<u64> D = {2, 3, 55}; // 2 real delete; 3 same-epoch pair; 55 skip(absent)
    Update::apply(st, I, D, enc, G);

    // id 1's insert is skipped (already present) -- it simply STAYS present
    // (nothing deletes it in this epoch); id 2 is really deleted; id 44 is
    // really inserted; id 3 (same-epoch pair) and id 55 (absent delete) do
    // nothing. Cross-checked against Ref.update_filter([1,2],[1,3,44],
    // [2,3,55]) == ([44], [2]) -- see task-11-report.md.
    std::vector<u64> expected_final = {1, 44};
    std::vector<u64> actual = st.my_ids;
    std::sort(actual.begin(), actual.end());
    EXPECT_EQ(actual, expected_final);
    EXPECT_EQ(st.my_size, 2u);

    std::set<u32> expected_J = {G.of(2), G.of(44)}; // ONLY the two real edits touch J
    EXPECT_EQ(st.J, expected_J);
    EXPECT_TRUE(st.check_against(expected_final, enc, G));
}

// --- TV-F6 (negative-as-positive-assert, requirement 1): "mark J for a
// skipped edit" is UPD-4's stated negative. This does NOT call the real
// (correct) Update::apply for the buggy leg -- it reproduces the described
// bug by hand (deliberately inserting G.of(x) into J for a skipped edit)
// and asserts the result differs from the real implementation's J,
// demonstrating that UPD-4's invariant actually has discriminating power.
TEST(Update, TVF6_MarkingJOnASkippedEditWouldFailUPD4) {
    Params params = Params::instantiate();
    const Encoder& enc = params.encoder;
    const BucketOracle& G = params.oracle;

    PartyState st;
    st.my_ids = {5};
    st.table.init(st.my_ids, enc, G);
    st.my_size = 1;
    auto j_before = st.J;

    Update::apply(st, std::vector<u64>{5}, std::vector<u64>{}, enc, G); // insert-of-present, skipped

    std::set<u32> buggy_J = j_before;
    buggy_J.insert(G.of(5)); // the bug TV-F6 names: mark J anyway

    EXPECT_EQ(st.J, j_before);   // UPD-4 holds for the real implementation
    EXPECT_NE(buggy_J, st.J);    // TV-F6: the described bug IS distinguishable
}

// --- UPD-5 (R-UPD/R-DUP): duplicate insert (same id twice in I, and
// insert-of-present) has NO EFFECT with the filter active. Positive leg:
// passes under the DEFAULT (filtered) build. Compiled into a SEPARATE
// -DSYMPSICA_NO_FILTER=ON build dir, these SAME assertions fail (TV-F5's
// negative leg -- see task-11-report.md's NO_FILTER evidence). No #ifdef
// needed in this test file: only src/protocols/update.cpp's filter block
// is conditionally compiled.
TEST(Update, UPD5_DuplicateInsertNoEffectWithFilter) {
    Params params = Params::instantiate();
    const Encoder& enc = params.encoder;
    const BucketOracle& G = params.oracle;

    // Duplicate insert of an ABSENT id: applies exactly once.
    PowerSumTable single_edit_reference;
    single_edit_reference.edit(66, +1, enc, G);

    PartyState st;
    Update::apply(st, std::vector<u64>{66, 66}, std::vector<u64>{}, enc, G);

    EXPECT_EQ(st.my_ids, std::vector<u64>{66});
    EXPECT_EQ(st.my_size, 1u);
    EXPECT_EQ(st.table.row(G.of(66)), single_edit_reference.row(G.of(66)));

    // Duplicate insert of an ALREADY-PRESENT id: membership filter still
    // applies on top of dedup -- full no-op.
    PartyState st2;
    st2.my_ids = {77};
    st2.table.init(st2.my_ids, enc, G);
    st2.my_size = 1;
    auto table_before = st2.table.rows();
    auto j_before = st2.J;

    Update::apply(st2, std::vector<u64>{77, 77}, std::vector<u64>{}, enc, G);

    EXPECT_EQ(st2.my_ids, std::vector<u64>{77});
    EXPECT_EQ(st2.my_size, 1u);
    EXPECT_EQ(st2.J, j_before);
    EXPECT_EQ(st2.table.rows(), table_before);
}

// --- R-SIM's forward-looking consumption: replay a Phase-3b epoch-
// schedule fixture through the real Update::apply and assert my_size at
// every query epoch matches reference.py's Ref.simulate() golden replay.
// Update-path state check only (R-SIM: count()/t_of() full consumption is
// Phase 4/6).
TEST(Update, ScheduleReplay_QueryEpochSizesMatchReferencePy) {
    Params params = Params::instantiate();
    const Encoder& enc = params.encoder;
    const BucketOracle& G = params.oracle;

    for (int seed = 0; seed <= 2; ++seed) {
        SCOPED_TRACE(::testing::Message() << "seed=" << seed);
        sympsica_test::Fixture fx(sympsica_test::fixture_path(
            "test/fixtures/schedule" + std::to_string(seed) + ".fixture"));

        u64 n_init = fx.u64_at("n_init");
        std::vector<u64> init_ids = read_id_list(fx, "init_ids");
        ASSERT_EQ(init_ids.size(), n_init);

        u64 epoch_count = fx.u64_at("epoch_count");
        auto epoch_rows = fx.all("epoch");
        ASSERT_EQ(epoch_rows.size(), epoch_count);

        u64 query_count = fx.u64_at("query_count");
        auto query_rows = fx.all("query");
        ASSERT_EQ(query_rows.size(), query_count);

        PartyState st;
        st.my_ids = init_ids;
        st.table.init(st.my_ids, enc, G);
        st.my_size = init_ids.size();

        std::size_t qi = 0;
        for (u64 e = 0; e < epoch_count; ++e) {
            const auto& row = epoch_rows.at(e); // [idx, query, I_count, I..., D_count, D...]
            ASSERT_EQ(std::stoull(row.at(0)), e);
            bool is_query = std::stoull(row.at(1)) != 0;
            u64 i_count = std::stoull(row.at(2));
            std::vector<u64> I;
            for (u64 k = 0; k < i_count; ++k) I.push_back(std::stoull(row.at(3 + k)));
            u64 d_count = std::stoull(row.at(3 + i_count));
            std::vector<u64> D;
            for (u64 k = 0; k < d_count; ++k) D.push_back(std::stoull(row.at(4 + i_count + k)));

            Update::apply(st, I, D, enc, G);

            if (is_query) {
                ASSERT_LT(qi, query_rows.size());
                const auto& qrow = query_rows.at(qi); // [query_idx, epoch_idx, expected_count]
                EXPECT_EQ(std::stoull(qrow.at(1)), e);
                u64 expected = std::stoull(qrow.at(2));
                EXPECT_EQ(st.my_size, expected) << "epoch=" << e;
                ++qi;
            }
        }
        EXPECT_EQ(qi, query_count);
    }
}

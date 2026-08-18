// test/core/kat_table.cpp — Task 11's formal Phase-3b KAT suite for
// core/table.hpp (task-11-brief.md, W3.7): TBL-1..4.
//
// TBL-1 is the ONLY one of these four that needs a reference.py-emitted
// fixture (requirement 3): it consumes the tbl1 section Task 3 emitted
// unconsumed in Phase 1 (test/fixtures/seed{0..9}.fixture), per R-TBL1.
// TBL-2/3/4 are pure C++ (identity/property checks against PowerSumTable
// itself, no Python golden needed).

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

#include "sympsica/core/table.hpp"
#include "sympsica/utils/encoding.hpp"
#include "sympsica/utils/field.hpp"
#include "sympsica/utils/params.hpp"
#include "../utils/fixture_support.hpp"

using namespace sympsica;

// --- TBL-1: init on A={ids a,b}, brute-force power sums k=1..7 vs reference.py
//
// The tbl1 fixture row stores id_a, id_b, and Ref.power_sums([id_a,id_b],
// g, 7) -- the COMBINED per-depth sum sigma(a)^k + sigma(b)^k -- NOT two
// separate per-bucket rows (BucketOracle has no Python port, so
// reference.py cannot know G.of(id_a)/G.of(id_b)). This test therefore:
//   (1) brute-forces the two per-id power-sum vectors in C++ (production
//       Fp/g) and checks their elementwise sum equals the fixture's
//       combined row -- the actual cross-language check against
//       reference.py's output.
//   (2) inits a REAL PowerSumTable on {id_a, id_b} and checks
//       table.row(G.of(id_a))/table.row(G.of(id_b)) each equal the
//       corresponding per-id vector from (1) -- id_a and id_b land in
//       different buckets with overwhelming probability (2^31 buckets),
//       asserted explicitly below; this is what actually exercises
//       PowerSumTable::init/edit's accumulation logic.
TEST(Table, TBL1_TwoIdRowsMatchReferencePy) {
    Params params = Params::instantiate();
    const Encoder& enc = params.encoder;
    const BucketOracle& G = params.oracle;

    for (int seed = 0; seed <= 9; ++seed) {
        SCOPED_TRACE(::testing::Message() << "seed=" << seed);
        sympsica_test::Fixture fx(sympsica_test::fixture_path(
            "test/fixtures/seed" + std::to_string(seed) + ".fixture"));

        u64 id_a = fx.u64_at("tbl1", 0);
        u64 id_b = fx.u64_at("tbl1", 1);
        std::array<Fp, Params::K> combined{};
        for (u64 k = 0; k < Params::K; ++k) {
            combined[k] = Fp(fx.u64_at("tbl1", 2 + k));
        }

        auto brute_force = [](Fp sigma) {
            std::array<Fp, Params::K> v{};
            Fp acc = sigma;
            for (u64 k = 0; k < Params::K; ++k) {
                v[k] = acc;
                acc = acc.mul(sigma);
            }
            return v;
        };
        std::array<Fp, Params::K> vec_a = brute_force(enc.sigma(id_a));
        std::array<Fp, Params::K> vec_b = brute_force(enc.sigma(id_b));

        for (u64 k = 0; k < Params::K; ++k) {
            EXPECT_EQ(vec_a[k].add(vec_b[k]), combined[k])
                << "combined power-sum row mismatch vs reference.py at k=" << k;
        }

        u32 beta_a = G.of(id_a);
        u32 beta_b = G.of(id_b);
        ASSERT_NE(beta_a, beta_b)
            << "bucket collision between id_a/id_b (probability ~2^-31; if this "
               "ever actually fires, TBL-1's per-bucket assumption needs revisiting "
               "for this seed)";

        PowerSumTable table;
        table.init(std::vector<u64>{id_a, id_b}, enc, G);
        EXPECT_EQ(table.row(beta_a), vec_a);
        EXPECT_EQ(table.row(beta_b), vec_b);
    }
}

// --- TBL-2: k index base -- row(beta)[0] is depth 1 (sigma(id)^1), never
// depth 0 (sigma(id)^0 == 1). FC (task-11-brief.md): "k-base off-by-one ->
// TBL-2 FAILS" -- the EXPECT_NE below is the direct contrast that would
// catch that off-by-one.
TEST(Table, TBL2_RowIndexZeroIsDepthOneNotDepthZero) {
    Params params = Params::instantiate();
    const Encoder& enc = params.encoder;
    const BucketOracle& G = params.oracle;

    for (u64 id : {1ull, 42ull, 123456789ull, 999999999999ull}) {
        PowerSumTable table;
        table.init(std::vector<u64>{id}, enc, G);
        auto row = table.row(G.of(id));
        EXPECT_EQ(row[0], enc.sigma(id)) << "id=" << id;
        EXPECT_NE(row[0], Fp(1)) << "id=" << id << " (row[0] must not be sigma(id)^0)";
    }
}

// --- TBL-3: edit(x,+1) then edit(x,-1) returns the table to its exact
// prior state (IDENTITY), over seeded random ids/buckets 0..9 -- a formal,
// seeded superset of core_selfcheck.cpp's hand-picked-id version.
TEST(Table, TBL3_EditPlusMinusIdentitySeeded) {
    Params params = Params::instantiate();
    const Encoder& enc = params.encoder;
    const BucketOracle& G = params.oracle;

    for (u64 seed = 0; seed <= 9; ++seed) {
        SCOPED_TRACE(::testing::Message() << "seed=" << seed);
        u64 state = seed ^ 0x3333333333333333ull;
        u64 unrelated_id = sympsica_test::splitmix64_next(state) % (1ull << 60);
        u64 id = sympsica_test::splitmix64_next(state) % (1ull << 60);

        PowerSumTable table;
        table.edit(unrelated_id, +1, enc, G); // nontrivial prior state elsewhere
        u32 beta = G.of(id);
        auto prior = table.row(beta);

        table.edit(id, +1, enc, G);
        table.edit(id, -1, enc, G);

        EXPECT_EQ(table.row(beta), prior);
    }
}

// --- TBL-4: syndrome cancellation for A=B -- all d_k = 0, every touched
// bucket, over 10 random sets (seeds 0..9).
TEST(Table, TBL4_SyndromeCancellationAeqB) {
    Params params = Params::instantiate();
    const Encoder& enc = params.encoder;
    const BucketOracle& G = params.oracle;

    for (u64 seed = 0; seed <= 9; ++seed) {
        SCOPED_TRACE(::testing::Message() << "seed=" << seed);
        u64 state = seed ^ 0x7777777777777777ull;
        std::vector<u64> ids;
        for (int i = 0; i < 5; ++i) {
            ids.push_back(sympsica_test::splitmix64_next(state) % (1ull << 60));
        }

        PowerSumTable table_a, table_b;
        table_a.init(ids, enc, G); // A
        table_b.init(ids, enc, G); // B == A (same id set)

        for (u64 id : ids) {
            u32 beta = G.of(id);
            auto row_a = table_a.row(beta);
            auto row_b = table_b.row(beta);
            for (u64 k = 0; k < Params::K; ++k) {
                EXPECT_EQ(row_a[k].sub(row_b[k]), Fp(0))
                    << "beta=" << beta << " k=" << k;
            }
        }
    }
}

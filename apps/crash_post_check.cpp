// apps/crash_post_check.cpp — task-28-brief.md PLAN-REVIEW REVISIONS R3
// (Important, replacing the withdrawn item 5): "complete POST state" is
// currently established by only four scalars (query_no, J.size(),
// cache.size(), my_size -- apps/crash_probe.cpp's `--mode inspect`,
// consumed by test/e2e/run_crash_matrix.py). Those four scalars can all
// match while the cache key set, cache VALUES, t_share, or my_ids diverge
// -- a partial commit that writes the right SHAPE but wrong CONTENTS would
// pass the existing check. This binary is a genuine SEMANTIC checker: it
// loads BOTH parties' state files directly (via the real production
// PartyState::load()/PartyState::check_against(), no re-derivation of
// anything from scratch) and validates the cross-party state R3 requires.
//
// SCOPE (Phase-6 gate review, Important 2 -- BINDING): what this binary
// establishes is CROSS-PARTY KEY-SET plus AGGREGATE consistency, NOT
// "every cache row reconstructs to that bucket's correct count". See the
// claim boundary on check 7 below. Cite it as such; do not call it a
// complete POST-state proof.
//
// The checks:
//
//   1. exact my_ids (each party's persisted id set matches the caller-
//      supplied expected set exactly).
//   2. oracle_salt matches across both parties (the restored oracle is the
//      SAME oracle both parties' tables were actually built under --
//      task-26-brief.md's own N2/R6-N2 fix is what makes this field
//      available at all).
//   3. table rebuilt under the restored oracle: PartyState::check_against
//      (real production method, core/state.cpp) against a BucketOracle
//      reconstructed directly from the persisted oracle_salt.
//   4. J empty for both parties (POST implies a fully committed query).
//   5. query_no matches the caller-supplied expected value, for both
//      parties.
//   6. exact cache key set: R's cache keys == S's cache keys (both parties
//      derive the SAME union_set in Query::run's full/incremental paths --
//      src/protocols/query.cpp -- so a genuine committed query has
//      identical cache key sets across parties; a partial write that
//      drops/adds a key on one side, or between the two, diverges here).
//   7. AGGREGATE cache/t_share consistency across the two parties: opens
//      (R.cache[beta].v + S.cache[beta].v) for every common key and sums
//      them, then checks that sum equals the OPENED t_share
//      (R.t_share.v + S.t_share.v) -- t_share == sum(cache), reconstructed
//      from the persisted bytes of BOTH independent processes, not merely
//      trusted from commit()'s own in-memory invariant.
//
//      CLAIM BOUNDARY (gate Important 2): this is an AGGREGATE check. No
//      individual opened cache[beta] is compared against an independently
//      expected per-bucket symmetric-difference rank, so any perturbation
//      that preserves the sum passes. Concretely: add delta to R's
//      persisted cache share at beta1 and subtract delta at beta2, leaving
//      t_share/keys/ids/table/salt/J/query_no untouched -- every check here
//      passes while BOTH reconstructed per-bucket values are wrong, and a
//      later Incremental query that re-evaluates only one of those buckets
//      would then open a wrong count. An internally consistent partial
//      commit that pre-creates the final key set, updates a subset of
//      values, and sets t_share to that subset's sum is likewise accepted.
//      What check 7 DOES close is the exact demonstrated corruption in
//      which cache values change while t_share does not; the assertion is
//      not tautological. Strengthening it to a complete POST-state proof
//      requires an independent expected value per opened bucket --
//      Phase-7+ entry obligation 2.
//
// This is read-only introspection over already-public PartyState fields
// and one already-existing production method (check_against); it adds no
// new protocol behavior and changes no default build output (see
// query.cpp's SYMPSICA_PARTIAL_CACHE_COMMIT for the flag-guarded,
// default-OFF negative construction used to demonstrate check 7 failing --
// R6-NOTAUTO, task-28-report.md).
//
// Usage: crash_post_check --state-r PATH --state-s PATH --ids-r "csv"
//        --ids-s "csv" --expect-query-no N
// Prints one line per check ([PASS]/[FAIL] <name>: <detail>) and exits 0
// iff every check passed, nonzero otherwise.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "sympsica/core/state.hpp"
#include "sympsica/utils/encoding.hpp"
#include "sympsica/utils/field.hpp"

using namespace sympsica;

namespace {

std::vector<u64> parse_csv(const std::string& s) {
    std::vector<u64> out;
    if (s.empty()) return out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (!tok.empty()) out.push_back(std::stoull(tok));
    }
    return out;
}

struct Config {
    std::string state_r, state_s;
    std::vector<u64> ids_r, ids_s;
    u64 expect_query_no = 0;
};

Config parse_args(int argc, char** argv) {
    Config cfg;
    bool have_state_r = false, have_state_s = false, have_ids_r = false, have_ids_s = false,
         have_qno = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            SYMPSICA_REQUIRE(i + 1 < argc, "crash_post_check: missing value after flag");
            return std::string(argv[++i]);
        };
        if (a == "--state-r") {
            cfg.state_r = next();
            have_state_r = true;
        } else if (a == "--state-s") {
            cfg.state_s = next();
            have_state_s = true;
        } else if (a == "--ids-r") {
            cfg.ids_r = parse_csv(next());
            have_ids_r = true;
        } else if (a == "--ids-s") {
            cfg.ids_s = parse_csv(next());
            have_ids_s = true;
        } else if (a == "--expect-query-no") {
            cfg.expect_query_no = std::stoull(next());
            have_qno = true;
        } else {
            SYMPSICA_REQUIRE(false, "crash_post_check: unrecognized flag");
        }
    }
    SYMPSICA_REQUIRE(have_state_r && have_state_s && have_ids_r && have_ids_s && have_qno,
                      "crash_post_check: missing required flag "
                      "(--state-r/--state-s/--ids-r/--ids-s/--expect-query-no)");
    return cfg;
}

bool g_all_pass = true;

void report(bool pass, const std::string& name, const std::string& detail) {
    std::cout << (pass ? "[PASS] " : "[FAIL] ") << name << ": " << detail << "\n";
    if (!pass) g_all_pass = false;
}

bool ids_match(std::vector<u64> a, std::vector<u64> b) {
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return a == b;
}

} // namespace

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);

    PartyState st_r, st_s;
    st_r.load(cfg.state_r);
    st_s.load(cfg.state_s);

    // 1. exact my_ids.
    report(ids_match(st_r.my_ids, cfg.ids_r), "my_ids_r",
           "party R persisted my_ids must exactly equal the expected id set");
    report(ids_match(st_s.my_ids, cfg.ids_s), "my_ids_s",
           "party S persisted my_ids must exactly equal the expected id set");

    // 2. oracle_salt matches across both parties (the "restored oracle" is
    // the SAME oracle for both sides).
    const bool salt_match = (st_r.oracle_salt == st_s.oracle_salt);
    report(salt_match, "oracle_salt_cross_party",
           "R and S must have restored the IDENTICAL oracle_salt");

    // 3. table rebuilt under the restored oracle (real production
    // check_against, one BucketOracle reconstructed per party from that
    // party's OWN persisted salt -- robust even if check 2 above fails).
    Encoder enc;
    const BucketOracle oracle_r(st_r.oracle_salt);
    const BucketOracle oracle_s(st_s.oracle_salt);
    report(st_r.check_against(cfg.ids_r, enc, oracle_r), "table_r",
           "party R's table must equal an independent rebuild from my_ids under the restored oracle");
    report(st_s.check_against(cfg.ids_s, enc, oracle_s), "table_s",
           "party S's table must equal an independent rebuild from my_ids under the restored oracle");

    // 4. J empty (POST implies a fully committed query).
    report(st_r.J.empty(), "J_empty_r", "party R's J must be empty after a committed query");
    report(st_s.J.empty(), "J_empty_s", "party S's J must be empty after a committed query");

    // 5. expected query_no.
    {
        std::ostringstream d;
        d << "party R query_no must equal " << cfg.expect_query_no << ", got " << st_r.query_no;
        report(st_r.query_no == cfg.expect_query_no, "query_no_r", d.str());
    }
    {
        std::ostringstream d;
        d << "party S query_no must equal " << cfg.expect_query_no << ", got " << st_s.query_no;
        report(st_s.query_no == cfg.expect_query_no, "query_no_s", d.str());
    }

    // 6. exact cache key set across parties.
    std::set<u32> keys_r, keys_s;
    for (const auto& [beta, unused] : st_r.cache) keys_r.insert(beta);
    for (const auto& [beta, unused] : st_s.cache) keys_s.insert(beta);
    report(keys_r == keys_s, "cache_key_set",
           "R and S cache key sets must be IDENTICAL (both derive the same union_set in a "
           "committed query)");

    // 7. AGGREGATE: t_share == sum(cache) over every common key, both
    // reconstructed from the two independently persisted files. NOT a
    // per-bucket value check -- see the claim boundary in the file header
    // (Phase-6 gate review, Important 2): any sum-preserving perturbation
    // of individual cache entries passes this check.
    if (keys_r == keys_s) {
        Fp combined_sum(0);
        for (u32 beta : keys_r) combined_sum = combined_sum.add(st_r.cache.at(beta).v.add(st_s.cache.at(beta).v));
        const Fp opened_t_share = st_r.t_share.v.add(st_s.t_share.v);
        std::ostringstream d;
        d << "sum over " << keys_r.size() << " reconstructed cache entries must equal reconstructed "
             "t_share (R.t_share + S.t_share)";
        report(combined_sum == opened_t_share, "t_share_eq_sum_cache", d.str());
    } else {
        report(false, "t_share_eq_sum_cache", "SKIPPED -- cache key sets already diverged (check 6)");
    }

    std::cout << (g_all_pass ? "crash_post_check: ALL CHECKS PASS\n"
                              : "crash_post_check: AT LEAST ONE CHECK FAILED\n");
    return g_all_pass ? 0 : 1;
}

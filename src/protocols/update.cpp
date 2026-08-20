#include "sympsica/protocols/update.hpp"

#include <algorithm>
#include <unordered_set>
#include <vector>

#include "sympsica/utils/common.hpp"

namespace sympsica {

namespace {

// dedup(v) — first-occurrence-wins deduplication (fix round 1, R-DUP ruling:
// a within-list duplicate is a redundant edit, governed by the same
// FT1/step-1-filter philosophy as every other filtered edit).
std::vector<u64> dedup(std::span<const u64> v) {
    std::vector<u64> out;
    out.reserve(v.size());
    std::unordered_set<u64> seen;
    for (u64 x : v) {
        if (seen.insert(x).second) out.push_back(x);
    }
    return out;
}

} // namespace

void Update::apply(PartyState& st, std::span<const u64> I, std::span<const u64> D,
                    const Encoder& enc, const BucketOracle& G) {
    // R6-N2 guard (task-26-brief.md, plan-review R3): every edit below
    // buckets ids via `G` (table.edit(x, +-1, enc, G), J.insert(G.of(x))).
    // If `G` disagrees with the salt st.table was actually built under, an
    // insert/delete here silently edits the WRONG bucket instead of
    // cancelling/extending the row a later query will read -- N2's exact
    // failure mode, reachable from ordinary Update::apply calls, not just
    // SaltManager::refresh/Query::run.
    st.require_salt_match(G);

    // =========================================================================
    // STEP 1 FILTER -- three stages, in this order (fix round 1, controller
    // ruling, binding):
    //   (1) DEDUP:      collapse within-list duplicates of I and of D to a
    //                    single first occurrence. A duplicate is a redundant
    //                    edit: it must be a FULL no-op the second time (no
    //                    double-apply on a duplicate insert, no abort on a
    //                    duplicate delete), exactly like every other filtered
    //                    edit below.
    //   (2) PAIR DROP:   an id present in BOTH the (deduped) I and D lists is
    //                    dropped from BOTH -- checked against the raw
    //                    (deduped) lists, not against the post-membership-
    //                    filtered I'/D' (which are disjoint by construction,
    //                    so that check would be vacuous).
    //   (3) MEMBERSHIP:  I' keeps only ids not currently in st.my_ids; D'
    //                    keeps only ids currently in st.my_ids.
    // This whole block is intentionally ONE contiguous, clearly-delimited
    // region: Task 11's SYMPSICA_NO_FILTER build-time flag disables ALL
    // THREE stages together (not just the membership filter) by #ifdef'ing
    // this region out -- UPD-5's negative leg (a duplicate insert corrupting
    // state) depends on dedup being inside the disabled region, not outside
    // it.
    // =========================================================================
#ifdef SYMPSICA_NO_FILTER
    // R-NOFILTER (task-11-brief.md, TEST-ONLY -- NEVER defined in the
    // default build; CMake option SYMPSICA_NO_FILTER defaults OFF): the
    // entire three-stage filter above is skipped -- I/D are used directly,
    // unfiltered, duplicates and all. This exists solely so a second,
    // separately-configured build dir (build-nofilter/) can demonstrate
    // TV-F5: without this filter, a duplicate/redundant edit corrupts
    // state (double-applies a duplicate insert, etc.) instead of being a
    // clean no-op, and the UPD-3/UPD-4/UPD-5 KATs (which assert the
    // FILTERED/correct behavior) fail against that corruption.
    std::vector<u64> i_prime(I.begin(), I.end());
    std::vector<u64> d_prime(D.begin(), D.end());
#else
    std::vector<u64> i_dedup = dedup(I);
    std::vector<u64> d_dedup = dedup(D);

    std::unordered_set<u64> i_set(i_dedup.begin(), i_dedup.end());
    std::unordered_set<u64> d_set(d_dedup.begin(), d_dedup.end());
    std::unordered_set<u64> my_set(st.my_ids.begin(), st.my_ids.end());

    std::vector<u64> i_prime, d_prime;
    i_prime.reserve(i_dedup.size());
    d_prime.reserve(d_dedup.size());
    for (u64 x : i_dedup) {
        if (!my_set.count(x) && !d_set.count(x)) i_prime.push_back(x);
    }
    for (u64 x : d_dedup) {
        if (my_set.count(x) && !i_set.count(x)) d_prime.push_back(x);
    }
#endif
    // ======================================================= END STEP 1 FILTER

    for (u64 x : i_prime) {
        st.my_ids.push_back(x);
        st.table.edit(x, +1, enc, G);
        st.J.insert(G.of(x));
        ++st.my_size;
    }

    for (u64 x : d_prime) {
        auto it = std::find(st.my_ids.begin(), st.my_ids.end(), x);
        SYMPSICA_REQUIRE(it != st.my_ids.end(),
                          "Update::apply: id in D' missing from my_ids (invariant violation)");
        st.my_ids.erase(it);
        st.table.edit(x, -1, enc, G);
        st.J.insert(G.of(x));
        --st.my_size;
    }
}

} // namespace sympsica

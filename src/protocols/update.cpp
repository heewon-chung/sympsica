#include "sympsica/protocols/update.hpp"

#include <algorithm>
#include <unordered_set>
#include <vector>

#include "sympsica/utils/common.hpp"

namespace sympsica {

void Update::apply(PartyState& st, std::span<const u64> I, std::span<const u64> D,
                    const Encoder& enc, const BucketOracle& G) {
    std::unordered_set<u64> i_raw(I.begin(), I.end());
    std::unordered_set<u64> d_raw(D.begin(), D.end());
    std::unordered_set<u64> my_set(st.my_ids.begin(), st.my_ids.end());

    std::vector<u64> i_prime, d_prime;
    i_prime.reserve(I.size());
    d_prime.reserve(D.size());
    for (u64 x : I) {
        if (!my_set.count(x) && !d_raw.count(x)) i_prime.push_back(x);
    }
    for (u64 x : D) {
        if (my_set.count(x) && !i_raw.count(x)) d_prime.push_back(x);
    }

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

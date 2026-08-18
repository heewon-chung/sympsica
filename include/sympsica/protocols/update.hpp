#ifndef SYMPSICA_PROTOCOLS_UPDATE_HPP
#define SYMPSICA_PROTOCOLS_UPDATE_HPP

#include <span>

#include "sympsica/core/state.hpp"
#include "sympsica/utils/encoding.hpp"
#include "sympsica/utils/field.hpp"

namespace sympsica {

// Update — Protocol 3 (local), plan W3.5 / design §protocols/update.hpp.
// ZERO communication: apply() is pure local computation on this party's own
// PartyState, no Channel parameter.
class Update {
public:
    // apply(st, I, D, enc, G):
    //   1. Filter (Protocol 3 line 2, FT1 + UPD-4's same-epoch pair): build
    //      I' from I, keeping only ids not already in st.my_ids AND not
    //      also present in the raw D list; build D' from D, keeping only
    //      ids currently in st.my_ids AND not also present in the raw I
    //      list. An id present in BOTH raw I and raw D (a same-epoch pair)
    //      is thereby dropped from BOTH candidate sets regardless of
    //      current membership -- see task-10-report.md for why this
    //      raw-I/raw-D check (not a literal I'∩D' on the post-membership-
    //      filtered sets) is the reading that actually implements UPD-4.
    //   2. For x in I': st.my_ids push, table.edit(x,+1), J.insert(G.of(x)),
    //      my_size++.
    //   3. For x in D': erase from my_ids, table.edit(x,-1),
    //      J.insert(G.of(x)), my_size--.
    // Skipped elements (present-insert, absent-delete, same-epoch pair)
    // touch NOTHING -- not even J (UPD-4).
    //
    // Signature note: `I`/`D` are `span<const u64>` (plan text shows
    // `span<u64>`); apply() only ever reads them to build I'/D', so the
    // const-qualified span is the more accurate/idiomatic signature --
    // documented as a minor deviation.
    static void apply(PartyState& st, std::span<const u64> I, std::span<const u64> D,
                       const Encoder& enc, const BucketOracle& G);
};

} // namespace sympsica

#endif // SYMPSICA_PROTOCOLS_UPDATE_HPP

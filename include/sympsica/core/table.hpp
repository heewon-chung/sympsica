#ifndef SYMPSICA_CORE_TABLE_HPP
#define SYMPSICA_CORE_TABLE_HPP

#include <array>
#include <span>
#include <unordered_map>

#include "sympsica/utils/encoding.hpp"
#include "sympsica/utils/field.hpp"
#include "sympsica/utils/params.hpp"

namespace sympsica {

// PowerSumTable — sparse bucketed power-sum table (Protocol 2; design
// §core/table.hpp; plan W3.2). Storage: bucket id (BucketOracle::of's
// [1..M] range) -> the K=7 power sums sigma(id)^1 .. sigma(id)^7,
// accumulated (signed) over every id currently assigned to that bucket.
// Index 0 stores depth 1 (TBL-2 pin): row(beta)[0] == sum of sigma(id) over
// every id in bucket beta, NOT sigma(id)^0 == 1.
class PowerSumTable {
public:
    // edit(id, sign, enc, G): row = map[G.of(id)]; for k in 0..K-1:
    //   row[k] +=/-= sigma(id)^(k+1)   (sign in {+1, -1} only; plan W3.2,
    //   verbatim recurrence: y = sigma(id); acc = y; row[k] +/- acc;
    //   acc *= y).
    void edit(u64 id, int sign, const Encoder& enc, const BucketOracle& G);

    // init(ids, enc, G): loop edit(id, +1, enc, G) for every id (plan W3.2).
    void init(std::span<const u64> ids, const Encoder& enc, const BucketOracle& G);

    // row(beta): the K=7 power sums for bucket beta, or the zero array if
    // beta has no row yet (padding-row semantics, design §table.hpp).
    std::array<Fp, Params::K> row(u32 beta) const;

    // rebuild(ids, enc, fresh): clear() then init(ids, enc, fresh) -- the
    // salt-refresh path (plan W3.2 / design §table.hpp). DEVIATION from the
    // plan/design's abbreviated one-arg signature `rebuild(freshOracle)`:
    // PowerSumTable itself does not remember the ids or Encoder it was
    // built from (plan W3.2: "PartyState keeps std::vector<u64> my_ids ...
    // the party's plaintext set IS state"), so `ids` and `enc` must be
    // passed in explicitly by the caller (PartyState/SaltManager) at
    // rebuild time; see task-10-report.md.
    void rebuild(std::span<const u64> ids, const Encoder& enc, const BucketOracle& fresh);

    void clear() { rows_.clear(); }

    // set_row — direct row mutator, used by PartyState::load() to restore a
    // deserialized table without re-running edit() (API addition beyond the
    // plan; needed for the save/load round trip, R4).
    void set_row(u32 beta, const std::array<Fp, Params::K>& row) { rows_[beta] = row; }

    const std::unordered_map<u32, std::array<Fp, Params::K>>& rows() const { return rows_; }

private:
    std::unordered_map<u32, std::array<Fp, Params::K>> rows_;
};

} // namespace sympsica

#endif // SYMPSICA_CORE_TABLE_HPP

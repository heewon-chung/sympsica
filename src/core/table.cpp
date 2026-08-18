#include "sympsica/core/table.hpp"

#include "sympsica/utils/common.hpp"

namespace sympsica {

void PowerSumTable::edit(u64 id, int sign, const Encoder& enc, const BucketOracle& G) {
    SYMPSICA_REQUIRE(sign == 1 || sign == -1, "PowerSumTable::edit: sign must be +1 or -1");

    Fp y = enc.sigma(id);
    Fp acc = y;
    // operator[] value-initializes a fresh entry as std::array<Fp,K>(),
    // which zero-initializes every Fp element: Fp's default constructor is
    // trivial, so value-initialization (unlike default-initialization)
    // reliably zeroes it -- see field.hpp's "uninitialized on default
    // construction" note.
    auto& row = rows_[G.of(id)];
    for (u64 k = 0; k < Params::K; ++k) {
        row[k] = (sign > 0) ? row[k].add(acc) : row[k].sub(acc);
        acc = acc.mul(y);
    }
}

void PowerSumTable::init(std::span<const u64> ids, const Encoder& enc, const BucketOracle& G) {
    for (u64 id : ids) {
        edit(id, +1, enc, G);
    }
}

std::array<Fp, Params::K> PowerSumTable::row(u32 beta) const {
    auto it = rows_.find(beta);
    if (it == rows_.end()) {
        return std::array<Fp, Params::K>{}; // absent-row semantics: zero row
    }
    return it->second;
}

void PowerSumTable::rebuild(std::span<const u64> ids, const Encoder& enc,
                             const BucketOracle& fresh) {
    clear();
    init(ids, enc, fresh);
}

} // namespace sympsica

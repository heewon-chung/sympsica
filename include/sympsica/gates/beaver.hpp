#ifndef SYMPSICA_GATES_BEAVER_HPP
#define SYMPSICA_GATES_BEAVER_HPP

#include <span>
#include <vector>

#include "sympsica/core/pools.hpp"
#include "sympsica/core/share.hpp"
#include "sympsica/utils/net.hpp"

namespace sympsica {

// BeaverEngine — batched Beaver multiplication (task-13 brief, W4.1): ONE
// call to mul() is ONE round of communication, no matter how many products
// are being computed. For each i, `mul` takes a triple (a,b,c) from `pool`,
// computes this party's local share of d_i = x_i - a_i and e_i = y_i - b_i,
// then batch-opens EVERY d_i/e_i across the whole call in a SINGLE
// send/recv of 2N field elements (N = x.size()) rather than one round per
// product. This is a masked opening (both operands stay secret; only the
// public difference from a fresh, independent triple is revealed) -- NOT a
// call to core/share.hpp's open function (R-GREP's grep-guard reserves that
// call for the final output opening and tests only), so this class
// implements its own wire packing directly via serdes.hpp.
//
// z_i = c_i + d*b_i + e*a_i + (role == Receiver ? d*e : 0) -- the standard
// Beaver identity, with the public cross term d*e added by exactly ONE
// party (Receiver) so it is not double-counted once both parties' shares
// are summed.
class BeaverEngine {
public:
    explicit BeaverEngine(Role role) : role_(role) {}

    Role role() const { return role_; }

    // z[i] = x[i] * y[i] (as fresh Shares), for i in [0, x.size()).
    // PRECOND: x.size() == y.size(); `pool` has >= x.size() triples
    // available (consumed via take(), one per product, in no particular
    // pairing -- any triple works with any product index). Sends and
    // receives exactly 2*x.size() Fp elements (16*x.size() bytes) over
    // `ch`, in ONE round.
    std::vector<Share> mul(std::span<const Share> x, std::span<const Share> y, TriplePool& pool,
                            Channel& ch);

private:
    Role role_;
};

} // namespace sympsica

#endif // SYMPSICA_GATES_BEAVER_HPP

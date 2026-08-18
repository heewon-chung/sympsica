#ifndef SYMPSICA_CORE_SHARE_HPP
#define SYMPSICA_CORE_SHARE_HPP

#include "sympsica/utils/field.hpp"
#include "sympsica/utils/net.hpp"

namespace sympsica {

// Share — one party's additive share of an Fp value (design doc §2,
// §core/share.hpp; plan W3.1). Reconstruction only ever happens through
// masked openings inside gates (Phase 4) or the final on-demand output
// opening (open(), below).
struct Share {
    Fp v;

    // Minor addition beyond the plan's literal `struct Share { Fp v; };`:
    // a default operator== for test/assertion convenience (mirrors Fp's own
    // defaulted operator==, field.hpp).
    constexpr bool operator==(const Share&) const = default;
};

enum class Role { Receiver, Sender };

// affine(a, x, b) = a*x + b applied to ONE party's Share of x (design
// §core/share.hpp, R5: design is authoritative for the API). This helper is
// intentionally symmetric/unconditional -- it always adds b to the result;
// it is the CALLER's responsibility to decide, per protocol convention,
// which party's call passes a nonzero b so that summing both parties'
// results reconstructs a*x + b exactly once (e.g. Query's final local
// affine conversion 2^{-1}(nA + nB - t) adds the public nA+nB term only on
// the receiver side, design §protocols/query.hpp, FT9); every other caller
// passes b = Fp(0).
Share affine(Fp a, Share x, Fp b);

// open(ch, mine) — sends `mine`, receives the peer's Share over `ch`, and
// returns the reconstructed Fp = mine.v + theirs.v (plan W3.1, verbatim
// order: send mine, recv theirs, return sum). BINDING (plan W3.1): used
// ONLY by the final output opening and by tests -- every intermediate
// reconstruction inside a gate must instead be a masked opening, never a
// raw open() of an unmasked Share. Task 11's grep-guard test asserts no
// other call site.
Fp open(Channel& ch, Share mine);

} // namespace sympsica

#endif // SYMPSICA_CORE_SHARE_HPP

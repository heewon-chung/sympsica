#ifndef SYMPSICA_UTILS_FIELD_HPP
#define SYMPSICA_UTILS_FIELD_HPP

#include <cstdint>
#include <limits>

#include "sympsica/utils/common.hpp"

static_assert(std::numeric_limits<unsigned __int128>::digits == 128,
              "build with -std=gnu++20");

namespace sympsica {

using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u8 = std::uint8_t;
using u128 = unsigned __int128;

// Fp — arithmetic over F_p, p = 2^61 - 1 (a Mersenne prime). INVARIANT: every
// Fp value produced by this class is the canonical representative in
// [0, P) (task-2 brief, "Global constants"). Header-only, constexpr-friendly
// (W1.1).
class Fp {
public:
    static constexpr u64 P = 2305843009213693951ull;

    // Uninitialized on default construction (mirrors libOTe's Fp<mod,T,TT>
    // convention of an uninitialized value member — see coeff_ctx.hpp) so Fp
    // stays trivially constructible; callers always assign a canonical value
    // before use.
    u64 v;

    constexpr Fp() = default;
    constexpr explicit Fp(u64 val) : v(val) {}

    constexpr bool operator==(const Fp&) const = default;

    // add/sub assume both operands are already canonical (< P), which holds
    // for every Fp this class produces.
    constexpr Fp add(Fp b) const {
        u64 r = v + b.v;
        if (r >= P) r -= P;
        return Fp(r);
    }

    constexpr Fp sub(Fp b) const {
        u64 r = v - b.v;
        if (r > v) r += P;
        return Fp(r);
    }

    // mul: Mersenne-prime reduction (2^61 == 1 mod P, since P = 2^61-1), i.e.
    // t = lo + hi*2^61 == lo + hi (mod P). The product of two canonical
    // (< 2^61) values fits in 122 bits; folding lo+hi can itself exceed P by
    // up to roughly 2x, hence the REQUIRED double fold + conditional subtract
    // (task-2 brief, W1.1) rather than a single fold.
    constexpr Fp mul(Fp b) const {
        u128 t = (u128)v * b.v;
        u64 r = (u64)(t & P) + (u64)(t >> 61);
        r = (r & P) + (r >> 61);
        if (r >= P) r -= P;
        return Fp(r);
    }

    constexpr Fp neg() const {
        return v == 0 ? Fp(0) : Fp(P - v);
    }

    constexpr bool is_zero() const { return v == 0; }

    // Left-to-right square-and-multiply, at most 63 squarings (skips leading
    // zero bits of e; e is a 64-bit exponent).
    constexpr Fp pow(u64 e) const {
        if (e == 0) return Fp(1);
        int top = 63;
        while (!((e >> top) & 1u)) --top;
        Fp result = *this;
        for (int i = top - 1; i >= 0; --i) {
            result = result.mul(result);
            if ((e >> i) & 1u) result = result.mul(*this);
        }
        return result;
    }

    // Fermat's little theorem: x^(P-2) == x^-1 (mod P). PRECOND v != 0.
    Fp inv() const {
        SYMPSICA_REQUIRE(v != 0, "Fp::inv: division by zero");
        return pow(P - 2);
    }

    // Reduces an arbitrary u64 mod P via the same double-fold used by mul():
    // x plays the role of mul's up-to-122-bit product t here (x >> 61 < 8,
    // since x < 2^64), so the same two-step fold + conditional subtract
    // suffices.
    static constexpr Fp from_u64(u64 x) {
        u64 r = (x & P) + (x >> 61);
        r = (r & P) + (r >> 61);
        if (r >= P) r -= P;
        return Fp(r);
    }
};

} // namespace sympsica

#endif // SYMPSICA_UTILS_FIELD_HPP

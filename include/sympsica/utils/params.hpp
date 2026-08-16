#ifndef SYMPSICA_UTILS_PARAMS_HPP
#define SYMPSICA_UTILS_PARAMS_HPP

#include <array>
#include <iosfwd>

#include "sympsica/utils/encoding.hpp"
#include "sympsica/utils/field.hpp"

namespace sympsica {

// Params — the single parameter authority (design doc: "every module reads
// it by const reference — no local copies of constants"). Holds every public
// parameter from the task-2 brief's "Global constants" section plus the
// current salt epoch (task-2 brief, W1.5).
struct Params {
    static constexpr u64 P = Fp::P;
    static constexpr u64 T = 4;
    static constexpr u64 K = 7;
    static constexpr u64 C = 4;               // digit chunks
    static constexpr u64 W = 16;               // digit width in bits
    static constexpr u64 TOP_DIGIT_BITS = 13;  // top digit is narrower (13 bits)
    static constexpr u64 M = BucketOracle::M;  // = 2^31 buckets
    static constexpr u64 T_MAX = 1ull << 14;
    static constexpr u64 U_MAX = 1ull << 10;   // = 1024
    static constexpr u64 PHI = 1ull << 13;

    // Owns the canonical generator g (Encoder) and the current salt epoch
    // (BucketOracle) — the "single authority" drift guard: every module that
    // needs g or the salt reads it from here rather than recomputing/copying.
    Encoder encoder;
    BucketOracle oracle;

    // Frozen values: default-constructs encoder (deterministic smallest-g
    // search) and oracle (all-zero salt, epoch 0).
    static Params instantiate();

    u64 g() const { return encoder.generator(); }
    const std::array<u8, 32>& salt() const { return oracle.salt(); }

    // Config echo contract (design doc): dumps every parameter to `os`.
    void echo(std::ostream& os) const;
};

} // namespace sympsica

#endif // SYMPSICA_UTILS_PARAMS_HPP

#ifndef SYMPSICA_UTILS_ENCODING_HPP
#define SYMPSICA_UTILS_ENCODING_HPP

#include <array>
#include <span>

#include "sympsica/utils/field.hpp"

namespace sympsica {

// Encoder — id -> Fp injection via g^id, g the smallest generator of F_p^*
// (task-2 brief, W1.3). INVARIANT: sigma is injective and never 0 (g != 0).
class Encoder {
public:
    // Computes and caches the smallest generator g of F_p^* (p = Fp::P):
    // g is a generator iff g^((p-1)/q) != 1 for every prime q dividing p-1.
    Encoder();

    // PRECOND id < 2^60 (id=0 -> sigma(0) = g^0 = 1, legal).
    Fp sigma(u64 id) const;

    u64 generator() const { return g_; }

    // Testability hook (W1.3): computes g^id mod p for an arbitrary small
    // modulus/generator pair via plain modular exponentiation, independent of
    // Fp (which is hardcoded to p = 2^61-1). Used to exercise the ENC-2
    // concrete row over the toy field F_101, g=2, without templatizing Fp.
    static u64 sigma_generic(u64 g, u64 p, u64 id);

private:
    u64 g_;
};

// BucketOracle — id -> bucket index [1..M] via a BLAKE3 ROM instantiation,
// salt-prefixed (task-2 brief, W1.4). State: a 32-byte salt (all-zero salt =
// epoch 0).
class BucketOracle {
public:
    // Bucket count. Mirrors Params::M (task-2 brief's "Global constants" is
    // the single authority both read from; see params.hpp).
    static constexpr u64 M = 1ull << 31;

    // All-zero salt = epoch 0.
    BucketOracle() = default;
    explicit BucketOracle(std::array<u8, 32> salt) : salt_(salt) {}

    // h = BLAKE3(salt || LE64(id)); bucket = (h as LE u64 over bytes 0..7) % M + 1.
    // Modulo bias from the 64-bit-into-M reduction is < 2^-33, documented and
    // acceptable (task-2 brief, W1.4).
    u32 of(u64 id) const;

    // salt' = BLAKE3(r_R || r_S) (32-byte output) — the joint salt-refresh
    // primitive; r_R/r_S are unspecified-length byte contributions from each
    // party.
    static BucketOracle refreshed(std::span<const u8> r_R, std::span<const u8> r_S);

    const std::array<u8, 32>& salt() const { return salt_; }

private:
    std::array<u8, 32> salt_{};
};

} // namespace sympsica

#endif // SYMPSICA_UTILS_ENCODING_HPP

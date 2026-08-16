#include "sympsica/utils/encoding.hpp"

#include <array>
#include <cstring>

#include <blake3.h>

#include "sympsica/utils/common.hpp"

namespace sympsica {

namespace {

// Unique prime factors of p-1 = 2 * 3^2 * 5^2 * 7 * 11 * 13 * 31 * 41 * 61 *
// 151 * 331 * 1321 (task-2 brief, W1.3 — fixed, given verbatim; multiplicity
// does not matter for the generator test g^((p-1)/q) != 1).
constexpr std::array<u64, 12> kFpMinusOneFactors = {
    2ull, 3ull, 5ull, 7ull, 11ull, 13ull, 31ull, 41ull, 61ull, 151ull, 331ull, 1321ull
};

} // namespace

Encoder::Encoder() {
    for (u64 candidate = 2; ; ++candidate) {
        Fp gc(candidate);
        bool ok = true;
        for (u64 q : kFpMinusOneFactors) {
            if (gc.pow((Fp::P - 1) / q) == Fp(1)) {
                ok = false;
                break;
            }
        }
        if (ok) {
            g_ = candidate;
            return;
        }
    }
}

Fp Encoder::sigma(u64 id) const {
    SYMPSICA_REQUIRE(id < (1ull << 60), "Encoder::sigma: id must be < 2^60");
    return Fp(g_).pow(id);
}

u64 Encoder::sigma_generic(u64 g, u64 p, u64 id) {
    u64 result = 1 % p;
    u64 base = g % p;
    while (id > 0) {
        if (id & 1u) {
            result = static_cast<u64>((unsigned __int128)result * base % p);
        }
        base = static_cast<u64>((unsigned __int128)base * base % p);
        id >>= 1;
    }
    return result;
}

namespace {

// LE64(id) — explicit shifts, not memcpy of a host-endian word (matches the
// serdes.hpp wire-encoding discipline: the hashed input must be reproducible
// across hosts of either endianness).
void append_le64(std::array<u8, 40>& msg, std::size_t offset, u64 id) {
    for (int i = 0; i < 8; ++i) {
        msg[offset + i] = static_cast<u8>(id >> (8 * i));
    }
}

} // namespace

u32 BucketOracle::of(u64 id) const {
    std::array<u8, 40> msg{}; // 32-byte salt || LE64(id)
    std::memcpy(msg.data(), salt_.data(), salt_.size());
    append_le64(msg, salt_.size(), id);

    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, msg.data(), msg.size());
    std::array<u8, 32> out{};
    blake3_hasher_finalize(&hasher, out.data(), out.size());

    u64 h = 0;
    for (int i = 0; i < 8; ++i) {
        h |= static_cast<u64>(out[i]) << (8 * i);
    }
    return static_cast<u32>(h % M) + 1;
}

BucketOracle BucketOracle::refreshed(std::span<const u8> r_R, std::span<const u8> r_S) {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, r_R.data(), r_R.size());
    blake3_hasher_update(&hasher, r_S.data(), r_S.size());
    std::array<u8, 32> salt{};
    blake3_hasher_finalize(&hasher, salt.data(), salt.size());
    return BucketOracle(salt);
}

} // namespace sympsica

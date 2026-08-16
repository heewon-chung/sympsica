#ifndef SYMPSICA_UTILS_SERDES_HPP
#define SYMPSICA_UTILS_SERDES_HPP

#include <span>
#include <vector>

#include "sympsica/utils/field.hpp"

namespace sympsica {

// serdes.hpp — the ONLY wire codec in sympsica (task-2 brief, W1.6): 64-bit
// little-endian words, always via explicit byte shifts (never memcpy of a
// host-endian word), so the wire format is LE even on a big-endian host.

// write_fp/read_fp — 8-byte LE encoding of a single Fp.
void write_fp(std::span<u8> out, Fp x);
Fp read_fp(std::span<const u8> in);

// Wire size in bytes of a length-prefixed vector of `count` u64 values
// (an 8-byte LE count prefix followed by `count` 8-byte LE values).
constexpr u64 u64_vec_wire_size(u64 count) { return 8 + 8 * count; }

// write_u64_vec/read_u64_vec — length-prefixed u64 LE vectors (index sets /
// bucket indices, per the task-2 brief). `out` must be at least
// u64_vec_wire_size(values.size()) bytes.
void write_u64_vec(std::span<u8> out, std::span<const u64> values);
std::vector<u64> read_u64_vec(std::span<const u8> in);

} // namespace sympsica

#endif // SYMPSICA_UTILS_SERDES_HPP

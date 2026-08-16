#include "sympsica/utils/serdes.hpp"

#include "sympsica/utils/common.hpp"

namespace sympsica {

namespace {

void write_u64_le(u8* dst, u64 x) {
    for (int i = 0; i < 8; ++i) {
        dst[i] = static_cast<u8>(x >> (8 * i));
    }
}

u64 read_u64_le(const u8* src) {
    u64 x = 0;
    for (int i = 0; i < 8; ++i) {
        x |= static_cast<u64>(src[i]) << (8 * i);
    }
    return x;
}

} // namespace

void write_fp(std::span<u8> out, Fp x) {
    SYMPSICA_REQUIRE(out.size() >= 8, "write_fp: buffer too small");
    write_u64_le(out.data(), x.v);
}

Fp read_fp(std::span<const u8> in) {
    SYMPSICA_REQUIRE(in.size() >= 8, "read_fp: buffer too small");
    u64 v = read_u64_le(in.data());
    SYMPSICA_REQUIRE(v < Fp::P, "read_fp: value not canonical (>= P)");
    return Fp(v);
}

void write_u64_vec(std::span<u8> out, std::span<const u64> values) {
    SYMPSICA_REQUIRE(out.size() >= u64_vec_wire_size(values.size()),
                      "write_u64_vec: buffer too small");
    write_u64_le(out.data(), values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        write_u64_le(out.data() + 8 + 8 * i, values[i]);
    }
}

std::vector<u64> read_u64_vec(std::span<const u8> in) {
    SYMPSICA_REQUIRE(in.size() >= 8, "read_u64_vec: buffer too small for length prefix");
    u64 count = read_u64_le(in.data());
    // Overflow-safe bound check: u64_vec_wire_size(count) = 8 + 8*count wraps
    // around for count >= ~2^61, which would let a corrupt length prefix
    // slip past a naive `in.size() >= u64_vec_wire_size(count)` check and
    // crash via std::vector's bad_alloc instead of SYMPSICA_REQUIRE's error
    // channel. Compare count directly against the buffer's capacity instead.
    SYMPSICA_REQUIRE(count <= (in.size() - 8) / 8,
                      "read_u64_vec: length prefix exceeds buffer");
    std::vector<u64> result(count);
    for (u64 i = 0; i < count; ++i) {
        result[i] = read_u64_le(in.data() + 8 + 8 * i);
    }
    return result;
}

} // namespace sympsica

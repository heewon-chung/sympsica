#include "sympsica/core/state.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <unistd.h>
#include <unordered_set>
#include <utility>
#include <vector>

#include "sympsica/utils/common.hpp"
#include "sympsica/utils/params.hpp"
#include "sympsica/utils/serdes.hpp"

namespace sympsica {

namespace {

// Local LE helpers for the u32 bucket-id scalars in the on-disk format
// (table rows' keys, J's elements, cache's keys). serdes.hpp covers Fp
// (8 bytes) and length-prefixed u64 vectors only; R4 explicitly permits
// "explicit LE for scalars" beyond those two primitives -- these mirror
// serdes.cpp's own write_u64_le/read_u64_le pattern (explicit byte shifts,
// never a memcpy of a host-endian word) at u32 width.
void write_u32_le(u8* dst, u32 x) {
    for (int i = 0; i < 4; ++i) dst[i] = static_cast<u8>(x >> (8 * i));
}

u32 read_u32_le(const u8* src) {
    u32 x = 0;
    for (int i = 0; i < 4; ++i) x |= static_cast<u32>(src[i]) << (8 * i);
    return x;
}

void write_u64_le(u8* dst, u64 x) {
    for (int i = 0; i < 8; ++i) dst[i] = static_cast<u8>(x >> (8 * i));
}

u64 read_u64_le(const u8* src) {
    u64 x = 0;
    for (int i = 0; i < 8; ++i) x |= static_cast<u64>(src[i]) << (8 * i);
    return x;
}

} // namespace

void PartyState::save(const std::string& path) const {
    // R1: table/cache rows are serialized in SORTED key order for
    // deterministic bytes, even though both are stored as unordered_map.
    std::vector<std::pair<u32, std::array<Fp, Params::K>>> sorted_table(
        table.rows().begin(), table.rows().end());
    std::sort(sorted_table.begin(), sorted_table.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<std::pair<u32, Share>> sorted_cache(cache.begin(), cache.end());
    std::sort(sorted_cache.begin(), sorted_cache.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    const u64 my_ids_bytes = u64_vec_wire_size(my_ids.size());
    const u64 table_bytes = 8 + sorted_table.size() * (4 + 8 * Params::K);
    const u64 j_bytes = 8 + J.size() * 4;
    const u64 cache_bytes = 8 + sorted_cache.size() * (4 + 8);
    const u64 t_share_bytes = 8;
    const u64 my_size_bytes = 8;
    const u64 total = my_ids_bytes + table_bytes + j_bytes + cache_bytes +
                       t_share_bytes + my_size_bytes;

    std::vector<u8> buf(total);
    u8* p = buf.data();

    write_u64_vec(std::span<u8>(p, my_ids_bytes), my_ids);
    p += my_ids_bytes;

    write_u64_le(p, sorted_table.size());
    p += 8;
    for (const auto& [beta, row] : sorted_table) {
        write_u32_le(p, beta);
        p += 4;
        for (u64 k = 0; k < Params::K; ++k) {
            write_fp(std::span<u8>(p, 8), row[k]);
            p += 8;
        }
    }

    write_u64_le(p, J.size());
    p += 8;
    for (u32 beta : J) { // std::set already iterates in ascending order
        write_u32_le(p, beta);
        p += 4;
    }

    write_u64_le(p, sorted_cache.size());
    p += 8;
    for (const auto& [beta, sh] : sorted_cache) {
        write_u32_le(p, beta);
        p += 4;
        write_fp(std::span<u8>(p, 8), sh.v);
        p += 8;
    }

    write_fp(std::span<u8>(p, 8), t_share.v);
    p += 8;

    write_u64_le(p, my_size);
    p += 8;

    SYMPSICA_REQUIRE(static_cast<u64>(p - buf.data()) == total,
                      "PartyState::save: buffer size mismatch (internal bug)");

    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    SYMPSICA_REQUIRE(f != nullptr, "PartyState::save: could not open tmp file for writing");
    std::size_t written = std::fwrite(buf.data(), 1, buf.size(), f);
    SYMPSICA_REQUIRE(written == buf.size(), "PartyState::save: short write");
    SYMPSICA_REQUIRE(std::fflush(f) == 0, "PartyState::save: fflush failed");
    SYMPSICA_REQUIRE(fsync(fileno(f)) == 0, "PartyState::save: fsync failed");
    SYMPSICA_REQUIRE(std::fclose(f) == 0, "PartyState::save: fclose failed");

    // Atomic-commit substrate for FT6: the rename is atomic on the same
    // filesystem, so a reader never observes a partially-written `path`.
    SYMPSICA_REQUIRE(std::rename(tmp.c_str(), path.c_str()) == 0,
                      "PartyState::save: atomic rename over destination failed");
}

void PartyState::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    SYMPSICA_REQUIRE(f.is_open(), "PartyState::load: could not open file for reading");
    std::streamoff size = f.tellg();
    SYMPSICA_REQUIRE(size >= 0, "PartyState::load: could not determine file size");
    f.seekg(0);
    std::vector<u8> buf(static_cast<std::size_t>(size));
    if (!buf.empty()) {
        SYMPSICA_REQUIRE(
            static_cast<bool>(f.read(reinterpret_cast<char*>(buf.data()), size)),
            "PartyState::load: short read");
    }

    std::span<const u8> rest(buf);

    SYMPSICA_REQUIRE(rest.size() >= 8, "PartyState::load: truncated (my_ids length prefix)");
    u64 my_ids_bytes = u64_vec_wire_size(read_u64_le(rest.data()));
    SYMPSICA_REQUIRE(rest.size() >= my_ids_bytes, "PartyState::load: truncated (my_ids body)");
    my_ids = read_u64_vec(rest.subspan(0, my_ids_bytes));
    rest = rest.subspan(my_ids_bytes);

    table.clear();
    SYMPSICA_REQUIRE(rest.size() >= 8, "PartyState::load: truncated (table row count)");
    u64 table_count = read_u64_le(rest.data());
    rest = rest.subspan(8);
    for (u64 i = 0; i < table_count; ++i) {
        SYMPSICA_REQUIRE(rest.size() >= 4 + 8 * Params::K,
                          "PartyState::load: truncated (table row)");
        u32 beta = read_u32_le(rest.data());
        rest = rest.subspan(4);
        std::array<Fp, Params::K> row{};
        for (u64 k = 0; k < Params::K; ++k) {
            row[k] = read_fp(rest.subspan(0, 8));
            rest = rest.subspan(8);
        }
        table.set_row(beta, row);
    }

    J.clear();
    SYMPSICA_REQUIRE(rest.size() >= 8, "PartyState::load: truncated (J count)");
    u64 j_count = read_u64_le(rest.data());
    rest = rest.subspan(8);
    for (u64 i = 0; i < j_count; ++i) {
        SYMPSICA_REQUIRE(rest.size() >= 4, "PartyState::load: truncated (J element)");
        J.insert(read_u32_le(rest.data()));
        rest = rest.subspan(4);
    }

    cache.clear();
    SYMPSICA_REQUIRE(rest.size() >= 8, "PartyState::load: truncated (cache count)");
    u64 cache_count = read_u64_le(rest.data());
    rest = rest.subspan(8);
    for (u64 i = 0; i < cache_count; ++i) {
        SYMPSICA_REQUIRE(rest.size() >= 4 + 8, "PartyState::load: truncated (cache entry)");
        u32 beta = read_u32_le(rest.data());
        rest = rest.subspan(4);
        Fp v = read_fp(rest.subspan(0, 8));
        rest = rest.subspan(8);
        cache[beta] = Share{v};
    }

    SYMPSICA_REQUIRE(rest.size() >= 8, "PartyState::load: truncated (t_share)");
    t_share.v = read_fp(rest.subspan(0, 8));
    rest = rest.subspan(8);

    SYMPSICA_REQUIRE(rest.size() >= 8, "PartyState::load: truncated (my_size)");
    my_size = read_u64_le(rest.data());
    rest = rest.subspan(8);
}

bool PartyState::check_against(std::span<const u64> expected_ids, const Encoder& enc,
                                const BucketOracle& G) const {
    PowerSumTable expected;
    expected.init(expected_ids, enc, G);

    // Compare row-for-row over the UNION of keys present in either table,
    // via row() (not a map-size/key-set comparison): PowerSumTable::edit()
    // never erases a row that cancels back to zero (found while testing
    // this method -- see task-10-report.md), so a table that has seen an
    // insert-then-delete on some bucket can carry a present-but-zero row
    // there. That is semantically identical to an absent row (both read
    // back as the zero array via row()), so comparing key-sets directly
    // would spuriously flag such a table as mismatched.
    std::unordered_set<u32> betas;
    for (const auto& [beta, unused] : expected.rows()) betas.insert(beta);
    for (const auto& [beta, unused] : table.rows()) betas.insert(beta);

    for (u32 beta : betas) {
        if (expected.row(beta) != table.row(beta)) return false;
    }
    return true;
}

} // namespace sympsica

#ifndef SYMPSICA_TEST_FIXTURE_SUPPORT_HPP
#define SYMPSICA_TEST_FIXTURE_SUPPORT_HPP

// test/utils/fixture_support.hpp — test-only infrastructure for the Phase-1
// KAT suite (task-3-brief.md, W1.8): a pinned splitmix64 PRNG + FNV-1a-style
// digest (bit-for-bit mirrors of ref/reference.py's splitmix64_next() /
// digest_update() — see that file's top comment for the shared spec both
// sides must match) and a minimal line-based reader for the fixture files
// emitted by `ref/reference.py emit` (on-disk format documented in
// test/fixtures/README.md).
//
// Deliberately NOT a general-purpose parser: the fixture format is small and
// fully controlled by reference.py, so this reader only needs "first token
// is a key, remaining whitespace-separated tokens are that row's values;
// several rows may share a key (fld4, tbl1)".

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "sympsica/utils/common.hpp"

namespace sympsica_test {

using u64 = std::uint64_t;

// --- pinned splitmix64 (Sebastiano Vigna, public domain) -------------------
// Mirrors ref/reference.py's splitmix64_next() bit-for-bit: uint64_t's
// well-defined wraparound-on-overflow plays the role of Python's explicit
// `& MASK64` masking there.
inline u64 splitmix64_next(u64& state) {
    state += 0x9E3779B97F4A7C15ull;
    u64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z = z ^ (z >> 31);
    return z;
}

// --- pinned digest (word-wise FNV-1a-style) ---------------------------------
// Mirrors ref/reference.py's digest_update() bit-for-bit. Starting value is
// the standard FNV-1a 64-bit offset basis.
constexpr u64 kFnvOffsetBasis = 0xCBF29CE484222325ull;
constexpr u64 kFnvPrime = 0x100000001B3ull;

inline void digest_update(u64& digest, u64 x) {
    digest ^= x;
    digest *= kFnvPrime;
}

// --- fixture reader ----------------------------------------------------------
class Fixture {
public:
    explicit Fixture(const std::string& path) {
        std::ifstream f(path);
        SYMPSICA_REQUIRE(f.is_open(), ("Fixture: could not open " + path).c_str());
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream iss(line);
            std::vector<std::string> tokens;
            std::string tok;
            while (iss >> tok) tokens.push_back(tok);
            if (!tokens.empty()) rows_.push_back(std::move(tokens));
        }
    }

    // Full row (row[0] is the key itself, row[1..] its values) of the first
    // row whose key equals `key`. Aborts if no such row exists.
    const std::vector<std::string>& one(const std::string& key) const {
        for (const auto& row : rows_) {
            if (row[0] == key) return row;
        }
        SYMPSICA_REQUIRE(false, ("Fixture: key not found: " + key).c_str());
        static const std::vector<std::string> unreachable; // SYMPSICA_REQUIRE aborts above
        return unreachable;
    }

    // Values (key stripped) of every row whose key equals `key`.
    std::vector<std::vector<std::string>> all(const std::string& key) const {
        std::vector<std::vector<std::string>> result;
        for (const auto& row : rows_) {
            if (row[0] == key) result.emplace_back(row.begin() + 1, row.end());
        }
        return result;
    }

    // Parses one(key)[idx + 1] (idx is 0-based over the row's VALUES, i.e.
    // excluding the key at row[0]) as a u64.
    u64 u64_at(const std::string& key, std::size_t idx = 0) const {
        const auto& row = one(key);
        SYMPSICA_REQUIRE(idx + 1 < row.size(), ("Fixture: value index out of range for key: " + key).c_str());
        return std::stoull(row[idx + 1]);
    }

private:
    std::vector<std::vector<std::string>> rows_;
};

// SYMPSICA_SOURCE_DIR is injected by CMakeLists.txt as a compile definition
// on the sympsica_tests target so fixture paths resolve regardless of
// ctest's working directory.
inline std::string fixture_path(const std::string& relative_to_source_dir) {
    return std::string(SYMPSICA_SOURCE_DIR) + "/" + relative_to_source_dir;
}

} // namespace sympsica_test

#endif // SYMPSICA_TEST_FIXTURE_SUPPORT_HPP

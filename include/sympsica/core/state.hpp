#ifndef SYMPSICA_CORE_STATE_HPP
#define SYMPSICA_CORE_STATE_HPP

#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "sympsica/core/share.hpp"
#include "sympsica/core/table.hpp"
#include "sympsica/utils/encoding.hpp"
#include "sympsica/utils/field.hpp"

namespace sympsica {

// PartyState — one party's full mutable state across the protocol lifetime
// (plan W3.3; design §core/state.hpp). `my_ids` is the party's plaintext id
// set: the paper's parties hold their own sets in the clear (plan W3.2's
// rebuild() note), so table.hpp's rebuild() and every id-list-consuming
// call reads it from here, not from the table itself.
struct PartyState {
    std::vector<u64> my_ids;
    PowerSumTable table;
    std::set<u32> J;

    // R1 (controller ruling): unordered_map per plan W3.3 (plan wins over
    // design's std::map) -- BUT save() serializes its contents in SORTED
    // key order for deterministic on-disk bytes (FT6 and the load-back
    // round trip depend on this; unordered_map's own iteration order is
    // unspecified and MUST NOT reach the wire/disk format).
    std::unordered_map<u32, Share> cache;
    Share t_share;
    u64 my_size = 0;

    // query_no — task-17-brief.md R-QNO: number of queries this party has
    // COMMITTED so far (incremented inside Query::run's atomic commit,
    // AFTER evaluation, alongside cache/t_share/J -- so a crash before
    // commit never bumps it). firstQuery = (query_no == 0). Additive
    // save()/load() format extension (R-QNO: authorized, Phase-3-owned
    // file); pre-Phase-5 state files do not carry this field and are
    // INCOMPATIBLE with load() from this point on -- acceptable for this
    // PoC (documented in task-17-report.md), since no on-disk state
    // predates this change in practice.
    u64 query_no = 0;

    // save(path): serializes every field to `path + ".tmp"`, fsyncs it, then
    // std::rename()s it over `path` -- the atomic-commit substrate FT6
    // relies on (plan W3.3). R4: every field goes through utils/serdes
    // primitives (write_fp/read_fp, write_u64_vec/read_u64_vec) plus
    // explicit LE for the u32 bucket-id scalars serdes.hpp does not itself
    // cover -- the serdes-only rule applies to disk exactly as to wire.
    // Pools (TriplePool/ZtGatePool) are NOT part of PartyState (R4: the
    // design struct has none; pools are runtime-only) and are never
    // serialized here.
    void save(const std::string& path) const;

    // load(path): the inverse of save() -- overwrites every field of *this
    // from `path`'s on-disk bytes. Aborts (SYMPSICA_REQUIRE) on a truncated
    // or malformed file.
    void load(const std::string& path);

    // check_against — test hook (item 6, controller ruling). The plan names
    // `invariant_check(const Ref&)`, but `Ref` is ref/reference.py's Python
    // reference class, which has no C++ counterpart; RENAMED (documented
    // deviation, item 6) to a minimal form that recomputes a table from
    // `expected_ids` via a throwaway PowerSumTable and compares it
    // row-for-row against `this->table`. The actual cross-check against the
    // Python reference happens in Task 11's tests via fixtures, not here.
    bool check_against(std::span<const u64> expected_ids, const Encoder& enc,
                        const BucketOracle& G) const;
};

} // namespace sympsica

#endif // SYMPSICA_CORE_STATE_HPP

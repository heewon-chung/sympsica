#ifndef SYMPSICA_CORE_STATE_HPP
#define SYMPSICA_CORE_STATE_HPP

#include <array>
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
    // task-24-brief.md CARRIED FINDING (Task-20-controller-found trap, not a
    // live bug -- apps/party_main.cpp already explicitly zero-inits a fresh
    // party's t_share, and that explicit init is intentionally left in
    // place below): every OTHER scalar member here self-initializes
    // (my_size/query_no both `= 0`), which invites the reasonable but wrong
    // assumption that t_share does too. Without this initializer, a
    // default-constructed PartyState left t_share's Fp reading whatever
    // garbage happened to occupy that memory -- undefined behavior, and
    // exactly what bit a Task-20 implementer's first draft. Fp(0) is the
    // canonical zero of the field (field.hpp), matching what firstQuery's
    // REPLACE semantics (query_no == 0) expect to overwrite regardless.
    Share t_share{Fp(0)};
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

    // oracle_salt — task-26-brief.md R6-N2 fix (plan-review-tasks-25-28.md
    // R3, BINDING over the brief's earlier "salt epoch" language): the
    // BucketOracle::salt() this party's `table`/`cache` were actually built
    // under. NOT a reconstructible numeric epoch -- SaltManager::refresh
    // derives the new oracle from BOTH parties' fresh contributions
    // (BucketOracle::refreshed(r_R, r_S), salt.cpp), and a restarting party
    // does not possess the peer's old contribution, so a counter cannot
    // recover the oracle after the fact. The 32-byte salt itself is the
    // only durable material that can. Defaults to all-zero (epoch 0),
    // matching a fresh Params::instantiate()'s default BucketOracle().
    // Additive save()/load() format extension, same R-QNO precedent
    // (Phase-3-owned file, authorized); pre-this-change state files are
    // truncated here and correctly abort on load() rather than silently
    // defaulting to zero from missing bytes -- same documented
    // incompatibility query_no's own addition already established.
    //
    // Bug this fixes (N2): SaltManager::refresh installs a new
    // params.oracle and rebuilds `table` under it but, pre-fix, saved only
    // the fields above -- no salt/oracle field existed here at all. A
    // restarting process (apps/party_main.cpp) always constructed epoch-0
    // Params BEFORE loading state, so a state file committed after a
    // maintenance day reloaded with a table built under the fresh salt
    // while every subsequent Update::apply/Query::run bucketed under
    // epoch-0 -- silently wrong, no abort. See require_salt_match() below,
    // the guard this field makes possible.
    std::array<u8, 32> oracle_salt{};

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

    // require_salt_match(G) — task-26-brief.md R6-N2 production guard
    // (plan-review-tasks-25-28.md R3): SYMPSICA_REQUIRE-aborts unless
    // `oracle_salt` matches `G.salt()` byte-for-byte. Call sites (ALL
    // real production entry points, not a check tucked away where a
    // future caller could bypass it -- R3's explicit requirement):
    // Update::apply (update.cpp), Query::run's every path (query.cpp,
    // covers both the ordinary caller and SaltManager::refresh's own
    // internal forced-full call, since by the time refresh() reaches that
    // call it has already updated st.oracle_salt/params.oracle together),
    // and SaltManager::refresh's own ENTRY point (salt.cpp, BEFORE it
    // derives the new oracle -- checks against the OLD salt this party's
    // CURRENT table/cache were built under). A mismatch means a caller is
    // about to bucket ids under a DIFFERENT oracle than the one the
    // party's stored table reflects -- exactly N2's silent-corruption
    // failure mode.
    void require_salt_match(const BucketOracle& G) const;
};

} // namespace sympsica

#endif // SYMPSICA_CORE_STATE_HPP

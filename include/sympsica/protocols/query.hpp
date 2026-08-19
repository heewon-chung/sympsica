#ifndef SYMPSICA_PROTOCOLS_QUERY_HPP
#define SYMPSICA_PROTOCOLS_QUERY_HPP

#include <string>
#include <vector>

#include "sympsica/core/share.hpp"
#include "sympsica/core/state.hpp"
#include "sympsica/protocols/setup.hpp"
#include "sympsica/utils/net.hpp"
#include "sympsica/utils/params.hpp"

namespace sympsica {

// SwitchRule — Protocol 4's path selector (task-17-brief.md W5.2; design
// doc §protocols/query.hpp). Pure function, ZERO communication: decide()
// only ever reads values the CALLER already has in hand (Query::run's own
// round 0, R-ROUND0, is what actually produces `nB`/`counterpartAnnounced`
// over the wire).
class SwitchRule {
public:
    enum class Path { Incremental, FullPublic, FullAnnounced };

    // decide(pp, nA, nB, myJ, firstQuery, counterpartAnnounced) — verbatim
    // plan text (W5.2):
    //   FullPublic     if firstQuery OR
    //                     2*u_max >= min(nA, m) + min(nB, m)
    //   FullAnnounced  if myJ > u_max (this party's own announce bit) OR
    //                     counterpartAnnounced
    //   Incremental    otherwise
    // Boundary (TV-F12, binding): myJ == u_max -> Incremental (the
    // announce bit fires STRICTLY above u_max, never at it). `nA`/`nB` are
    // the two parties' current raw-set sizes (order-insensitive: only their
    // sum enters the size test); `myJ` is THIS party's own |J| (dirty
    // bucket count) BEFORE padding.
    static Path decide(const Params& pp, u64 nA, u64 nB, u64 myJ, bool firstQuery,
                        bool counterpartAnnounced);
};

namespace detail {

// commit(st, betas, new_shares, replace, state_path) — the ATOMIC COMMIT
// shared by both Query::run paths (query.cpp's own top comment on this
// function has the full FT6/W5.3/W5.4 semantics). Exposed here (moved out
// of query.cpp's anonymous namespace, task-20-brief.md M1 fix round 1, then
// namespaced round 2) so test/protocols/kat_query.cpp's FC3 negative can
// invoke the REAL commit logic directly instead of hand-deriving a
// comparison from already-real numbers. `detail` (not bare `sympsica`) is
// this codebase's own established precedent for "internal, but needs
// cross-TU reach including tests" -- core/pools.hpp's CorrelationPool<T> and
// protocols/setup.hpp's SetupOtState both already live there; `commit` is a
// generic enough name that it belongs beside them, not planted unqualified
// at the top of a namespace that keeps growing. Not part of the class-based
// public API (Query::run is): a free function, same as it always was, just
// no longer file-local.
void commit(PartyState& st, const std::vector<u32>& betas, const std::vector<Share>& new_shares,
            bool replace, const std::string& state_path);

} // namespace detail

// Query — Protocol 4 + full evaluation (task-17-brief.md W5.3/W5.4; design
// doc §protocols/query.hpp). Dispatches per SwitchRule; every symmetric
// round goes through Channel::exchange() (R-XCHG) -- see query.cpp's own
// top comment for the exact wire layout of each round.
//
// DESIGN-DOC DELTAS (documented deviations from the design doc's bare
// `static Share run(Role, PartyState&, Pools&, Channel&, const Params&)`):
//
//   1. R-SEED: a `u64 seed` parameter. The incremental/full-path padding
//      steps (W5.3 step 1 / W5.4's supports padding) need a PRG seed to
//      sample decoy bucket indices deterministically per party -- the
//      design doc has no such input anywhere upstream (party_main, not
//      built yet at this task, is expected to pass --seed through here).
//
//   2. state_path: PartyState has no path field of its own (task-17-brief.md's
//      literal field list for PartyState is my_ids/table/J/cache/t_share/
//      my_size/query_no -- no path), yet W5.3 step 4 / W5.4's REPLACE step
//      both end with "PartyState::save() (tmp+rename)" INSIDE Query::run's
//      own atomic commit (this is exactly where R-CRASH's "pre-serialize"
//      hook lives -- "in Query's commit, after all evals, before save").
//      save() takes an explicit path argument (core/state.hpp), so
//      Query::run must be handed one to call it with; `state_path` is that
//      argument, appended after `seed` for the same additive-parameter
//      reason.
//
// Both deltas are used exactly once, purely to satisfy needs the plan text
// itself imposes on this function -- see task-17-report.md.
//
// TASK-18 ADDITIVE EXTENSIONS (task-18-brief.md; do NOT restructure
// anything above -- both are new trailing default-valued parameters):
//
//   3. R-FORCEFULL: `bool force_full = false`. Round 0 (my_size/announce
//      exchange) still always runs -- SaltManager::refresh's own convert()
//      step needs the counterpart's size exactly like any other query --
//      but when true, path selection SKIPS SwitchRule::decide() entirely
//      and goes straight to the full path (run_full's FullPublic/REPLACE
//      semantics: supports exchange, no announce bit), which is exactly
//      what a maintenance event's "immediately run Query full path" step
//      needs.
//
//   4. `SwitchRule::Path* path_out = nullptr` (R-JSONL support): when
//      non-null, receives the path actually taken (the forced value when
//      `force_full` is set). Query::run's return value is only the count
//      Share -- callers that need to LABEL a query event (party_main's
//      JSONL "path" field) have no other way to learn which branch ran
//      without duplicating round 0's own networking.
class Query {
public:
    static Share run(Role role, PartyState& st, Pools& pools, Channel& ch, const Params& params,
                      const std::string& state_path, u64 seed, bool force_full = false,
                      SwitchRule::Path* path_out = nullptr);

    // open_count(ch, mine) — R-OPEN (task-18-brief.md): the ONE production
    // call site for core/share.hpp's `open` function outside test/** and
    // core/share.{hpp,cpp} themselves (grep-guard's reserved exclusion,
    // test/core/grep_guard_no_open_callsites.sh). Both parties call this
    // with their own Query::run/SaltManager::refresh return value (the
    // converted-but-unopened count Share) to learn the public count --
    // intentionally symmetric, neither party is a designated "revealer".
    static u64 open_count(Channel& ch, Share mine);
};

} // namespace sympsica

#endif // SYMPSICA_PROTOCOLS_QUERY_HPP

#ifndef SYMPSICA_PROTOCOLS_QUERY_HPP
#define SYMPSICA_PROTOCOLS_QUERY_HPP

#include <string>

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
class Query {
public:
    static Share run(Role role, PartyState& st, Pools& pools, Channel& ch, const Params& params,
                      const std::string& state_path, u64 seed);
};

} // namespace sympsica

#endif // SYMPSICA_PROTOCOLS_QUERY_HPP

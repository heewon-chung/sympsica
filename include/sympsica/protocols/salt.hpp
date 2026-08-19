#ifndef SYMPSICA_PROTOCOLS_SALT_HPP
#define SYMPSICA_PROTOCOLS_SALT_HPP

#include <string>

#include "sympsica/core/share.hpp"
#include "sympsica/core/state.hpp"
#include "sympsica/protocols/setup.hpp"
#include "sympsica/utils/field.hpp"
#include "sympsica/utils/net.hpp"
#include "sympsica/utils/params.hpp"

namespace sympsica {

// SaltManager — Protocol W5.5, the salt-refresh maintenance event
// (task-18-brief.md; design doc's periodic re-salting).
class SaltManager {
public:
    // due(query_no, phi, maintenance_day, force) — R-TRIGGER: a pure,
    // ZERO-communication function, so the phi branch is unit-testable
    // without running 2^13 real queries. `query_no` is
    // PartyState::query_no -- the number of queries this party has
    // COMMITTED so far, BEFORE the query about to run.
    //
    // CONVENTION (documented per R-TRIGGER's own requirement): query_no==0
    // (no query has ever committed yet) is NEVER due via the phi branch --
    // there is nothing accumulated yet to warrant a refresh, and a refresh
    // before the very first query would be a maintenance event with no
    // query history behind it. The phi branch fires when query_no is a
    // positive multiple of phi (phi, 2*phi, 3*phi, ...): once every phi
    // committed queries, the NEXT query triggers maintenance.
    // maintenance_day and force are unconditional ORs, independent of
    // query_no/phi entirely -- either one alone makes the call due.
    // phi==0 is guarded (never due via the phi branch, no division).
    static bool due(u64 query_no, u64 phi, bool maintenance_day, bool force);

    // refresh(...) — W5.5, the unified maintenance event:
    //   1. R-SALTX: ONE Channel::exchange() round of this party's 32-byte
    //      contribution against the counterpart's; salt' =
    //      BucketOracle::refreshed(r_R, r_S) -- input order FIXED as
    //      (receiver's contribution, sender's contribution) regardless of
    //      which role THIS party plays, so both parties derive
    //      byte-identical salt (the Receiver always generates r_R, the
    //      Sender always generates r_S -- see salt.cpp's own comment on
    //      how each contribution is sampled).
    //   2. Installs the new oracle into `params.oracle` (Params owns the
    //      "single authority" salt/epoch, params.hpp) -- `params` is taken
    //      by non-const reference for exactly this mutation.
    //   3. `st.table.rebuild(st.my_ids, params.encoder, params.oracle)` --
    //      every row recomputed under the fresh salt.
    //   4. R-FORCEFULL: immediately runs Query's forced-full path
    //      (`Query::run(..., /*force_full=*/true)`) as the maintenance
    //      query itself -- FullPublic/REPLACE semantics (supports
    //      exchange, no announce bit); Query::run's own commit() clears
    //      `st.J` and advances `st.query_no` exactly as any other
    //      committed query would.
    // Returns the converted-but-UNOPENED count Share (same contract as
    // Query::run) -- the caller (party_main) opens it via
    // Query::open_count, identically to every other query path.
    static Share refresh(Role role, PartyState& st, Params& params, Pools& pools, Channel& ch,
                          const std::string& state_path, u64 seed);
};

// ScheduleSync — R-SCHED2's day-boundary pair-drift guard (task-18-brief.md
// W5.7: "party_main defends against pair drift: at each day boundary the
// parties exchange() a digest of (day string, query flag, maintenance
// flag) and SYMPSICA_REQUIRE equality -- mismatch aborts, never hangs").
// Co-located here (not inlined into apps/party_main.cpp) purely for
// unit-testability: kat_salt.cpp's FC1 exercises it directly without
// needing to link party_main.cpp's own main() into the test binary.
// party_main calls sync() once per schedule day, using values read from
// its OWN --schedule file, before applying that day's Update::apply.
class ScheduleSync {
public:
    // Pure, zero-communication digest: an FNV-1a-style fold of (day's raw
    // bytes, a 0 separator byte, query-flag byte, maintenance-flag byte).
    // NOT a cryptographic commitment -- R-SCHED2 only needs to catch an
    // HONEST schedule-pair-generation drift (FC1), not an adversarial
    // party.
    static u64 digest(const std::string& day, bool query, bool maintenance);

    // sync(ch, day, query, maintenance) -- ONE Channel::exchange() round of
    // digest()'s 8-byte output; SYMPSICA_REQUIRE aborts (deterministically,
    // never hangs -- FC1) if the two parties' digests disagree.
    static void sync(Channel& ch, const std::string& day, bool query, bool maintenance);
};

// OverflowChecker — W5.6 (SCOPE DECISION, no implementer discretion): the
// depth-T'=7 overflow detector needs syndromes to depth 2*T'-1=13 > K=7;
// the stored PowerSumTable rows cannot support it, and an MPC deep-scan
// circuit (mu(7)) is OUT of PoC scope. This stub exists only so protocols/
// has the symbol the design doc's shape calls for; it ALWAYS dies via
// SYMPSICA_REQUIRE(false, ...) and never returns a value on any code path
// (FC2: no `return` statement anywhere in the definition). The real,
// working detector lives ONLY in ref/reference.py's `detect_overflow()`
// (Phase-6 demo: plants a 5-collision bucket, shows detection +
// salt-refresh transience).
class OverflowChecker {
public:
    [[noreturn]] static bool check(const PartyState& st, u64 t_prime);
};

} // namespace sympsica

#endif // SYMPSICA_PROTOCOLS_SALT_HPP

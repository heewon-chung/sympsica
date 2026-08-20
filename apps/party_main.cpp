// apps/party_main.cpp — Protocol W5.7 (task-18-brief.md): the two-process
// CLI binary that drives Setup/Update/Query/SaltManager over a real TCP
// Channel against ITS OWN --schedule file (per-party schedule stream,
// R-SCHED2), writing one JSONL record per query event (R-JSONL) and
// echoing its effective configuration to stderr on start.
//
// Flags (verbatim, W5.7): --role r|s --listen <port>|--connect host:port
// --schedule file.json --state dir --out file.jsonl --seed u64
// [--force-refresh] [--budget-s f]. Exactly one of --listen/--connect is
// required, independent of --role (either role may listen or connect --
// the Channel constructor's own server/client distinction is what matters,
// not which protocol role this party plays).
//
// [--update-only] (task-24-brief.md SC1/CG-A, additive): a ZERO-
// communication mode. When set, --listen/--connect/--out/--seed's networked
// counterpart are not needed (--seed is still required by the flag parser
// for uniformity, but this mode never reads it): the schedule is applied
// via Update::apply ONLY (local, no Channel, no Setup::run, no per-day
// query/maintenance handling) and the process exits 0 without ever having
// constructed a Channel. Every schedule day's `query` flag MUST be false in
// this mode (SYMPSICA_REQUIRE) -- a query day needs a Channel, which this
// mode exists specifically to never construct.
//
// Schedule JSON (W5.7, this party's OWN stream): a top-level array of day
// objects, `[{"day":"2023-01-05","insert":[ids],"delete":[ids],
// "query":true,"maintenance":false}]`. Parsed by a small hand-rolled
// strict parser scoped to EXACTLY this schema (see JsonParser below for its
// documented limits) -- no new external JSON dependency (task-18-brief.md
// requirement 2).
//
// Per-day control flow (R-SCHED2 / R-SYMPROV, in order):
//   1. ScheduleSync::sync(ch, day, query, maintenance) -- the pair-drift
//      guard, BEFORE Update::apply (per salt.hpp's own doc comment on
//      ScheduleSync -- this project's inherited, already-tested contract).
//   2. Update::apply(st, insert, delete, enc, oracle) -- local, zero
//      communication.
//   3. If this day's `query` flag is set: R-SYMPROV's day-boundary
//      provisioning handshake (exchange_provision(), below) THEN
//      Setup::refill_offline to a target BOTH parties compute identically
//      (see exchange_provision's doc comment for why this, not the
//      remaining()-count itself, is what actually eliminates the
//      asymmetric-exhaustion hazard) THEN either SaltManager::refresh
//      (if SaltManager::due()) or Query::run, THEN Query::open_count, THEN
//      one R-JSONL record line.
//   4. --budget-s soft guard checked at the end of the day's processing
//      (never mid-query): once exceeded, finish writing this day's record
//      and stop consuming further schedule days, exit 0 with a stderr
//      note.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "sympsica/core/state.hpp"
#include "sympsica/protocols/query.hpp"
#include "sympsica/protocols/salt.hpp"
#include "sympsica/protocols/setup.hpp"
#include "sympsica/protocols/update.hpp"
#include "sympsica/utils/common.hpp"
#include "sympsica/utils/net.hpp"
#include "sympsica/utils/params.hpp"

using namespace sympsica;

namespace {

// ---------------------------------------------------------------------------
// JsonParser — a small, STRICT, hand-rolled recursive-descent parser scoped
// to EXACTLY the W5.7 schedule schema (task-18-brief.md requirement 2: "no
// new external deps ... a small strict parser for exactly this schema is
// acceptable -- document its limits").
//
// LIMITS (documented, deliberate, not general-purpose JSON):
//   - Numbers: unsigned decimal integers only (no floats, no negatives, no
//     exponents) -- the schema only ever carries u64 ids.
//   - Strings: a `\c` escape passes `c` through literally (sufficient for
//     this schema's plain "YYYY-MM-DD" day strings, which contain no
//     special characters); no \uXXXX/\n/\t escapes, no UTF-8 validation.
//   - No nested objects/arrays beyond the one fixed level the schedule
//     schema itself uses (a top-level array of flat day-objects, where the
//     only array-valued fields are `insert`/`delete`).
//   - Object keys must be exactly one of the schedule schema's five known
//     names; an unrecognized key aborts (SYMPSICA_REQUIRE) rather than
//     being silently skipped -- a typo'd schedule file must fail loudly,
//     not query the wrong ids.
// ---------------------------------------------------------------------------
class JsonParser {
public:
    explicit JsonParser(const std::string& text) : s_(text) {}

    void skip_ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }

    void expect(char c) {
        skip_ws();
        SYMPSICA_REQUIRE(i_ < s_.size() && s_[i_] == c, "party_main: JSON parse error: unexpected character");
        ++i_;
    }

    bool try_char(char c) {
        skip_ws();
        if (i_ < s_.size() && s_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }

    std::string parse_string() {
        skip_ws();
        SYMPSICA_REQUIRE(i_ < s_.size() && s_[i_] == '"', "party_main: JSON parse error: expected a string");
        ++i_;
        std::string out;
        while (i_ < s_.size() && s_[i_] != '"') {
            if (s_[i_] == '\\' && i_ + 1 < s_.size()) {
                out.push_back(s_[i_ + 1]);
                i_ += 2;
            } else {
                out.push_back(s_[i_]);
                ++i_;
            }
        }
        SYMPSICA_REQUIRE(i_ < s_.size(), "party_main: JSON parse error: unterminated string");
        ++i_; // closing quote
        return out;
    }

    u64 parse_uint() {
        skip_ws();
        std::size_t start = i_;
        while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        SYMPSICA_REQUIRE(i_ > start, "party_main: JSON parse error: expected an unsigned integer");
        return std::stoull(s_.substr(start, i_ - start));
    }

    bool parse_bool() {
        skip_ws();
        if (s_.compare(i_, 4, "true") == 0) {
            i_ += 4;
            return true;
        }
        if (s_.compare(i_, 5, "false") == 0) {
            i_ += 5;
            return false;
        }
        SYMPSICA_REQUIRE(false, "party_main: JSON parse error: expected true/false");
        return false; // unreachable
    }

    std::vector<u64> parse_uint_array() {
        expect('[');
        std::vector<u64> out;
        skip_ws();
        if (try_char(']')) return out;
        while (true) {
            out.push_back(parse_uint());
            skip_ws();
            if (try_char(',')) continue;
            expect(']');
            break;
        }
        return out;
    }

    bool at_end() {
        skip_ws();
        return i_ >= s_.size();
    }

private:
    const std::string& s_;
    std::size_t i_ = 0;
};

struct ScheduleDay {
    std::string day;
    std::vector<u64> insert_ids;
    std::vector<u64> delete_ids;
    bool query = false;
    bool maintenance = false;
};

std::vector<ScheduleDay> parse_schedule(const std::string& text) {
    JsonParser p(text);
    p.expect('[');
    std::vector<ScheduleDay> days;
    p.skip_ws();
    if (p.try_char(']')) return days;
    while (true) {
        p.expect('{');
        ScheduleDay day;
        bool have_day = false, have_query = false, have_maintenance = false;
        p.skip_ws();
        while (true) {
            std::string key = p.parse_string();
            p.expect(':');
            if (key == "day") {
                day.day = p.parse_string();
                have_day = true;
            } else if (key == "insert") {
                day.insert_ids = p.parse_uint_array();
            } else if (key == "delete") {
                day.delete_ids = p.parse_uint_array();
            } else if (key == "query") {
                day.query = p.parse_bool();
                have_query = true;
            } else if (key == "maintenance") {
                day.maintenance = p.parse_bool();
                have_maintenance = true;
            } else {
                SYMPSICA_REQUIRE(false, "party_main: schedule parse error: unrecognized day field");
            }
            if (p.try_char(',')) continue;
            p.expect('}');
            break;
        }
        SYMPSICA_REQUIRE(have_day && have_query && have_maintenance,
                          "party_main: schedule parse error: a day object is missing "
                          "day/query/maintenance");
        days.push_back(std::move(day));
        if (p.try_char(',')) continue;
        p.expect(']');
        break;
    }
    return days;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    SYMPSICA_REQUIRE(f.is_open(), "party_main: failed to open input file");
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

// ---------------------------------------------------------------------------
// Config / CLI parsing.
// ---------------------------------------------------------------------------

struct Config {
    char role_char = 0; // 'r' or 's'
    bool is_server = false;
    std::string address;
    std::string schedule_path;
    std::string state_dir;
    std::string out_path;
    u64 seed = 0;
    bool force_refresh = false;
    double budget_s = -1.0; // <= 0 means "unset, no budget"
    // task-23-brief.md R6-AUDIT-SCALE: OFF by default (empty = no dump),
    // additive-only observability channel. When set, a JSON audit dump of
    // both pools' generated/consumed_ids/remaining plus the shared
    // next_corr_id is written to this path at process exit -- test/e2e/
    // run_e2e_gate.py's own W6.4/W6.6(ii) audit reads it; no other consumer
    // of this binary is affected by this flag's absence.
    std::string audit_out_path;
    // task-24-brief.md SC1/CG-A: OFF by default, additive-only. See this
    // file's top comment for the mode's full contract.
    bool update_only = false;
};

Config parse_args(int argc, char** argv) {
    Config cfg;
    bool have_role = false, have_addr = false, have_schedule = false, have_state = false, have_out = false,
         have_seed = false;
    std::string listen_port, connect_addr;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            SYMPSICA_REQUIRE(i + 1 < argc, "party_main: missing value after flag");
            return std::string(argv[++i]);
        };
        if (a == "--role") {
            std::string r = next();
            SYMPSICA_REQUIRE(r == "r" || r == "s", "party_main: --role must be 'r' or 's'");
            cfg.role_char = r[0];
            have_role = true;
        } else if (a == "--listen") {
            listen_port = next();
            cfg.is_server = true;
            have_addr = true;
        } else if (a == "--connect") {
            connect_addr = next();
            cfg.is_server = false;
            have_addr = true;
        } else if (a == "--schedule") {
            cfg.schedule_path = next();
            have_schedule = true;
        } else if (a == "--state") {
            cfg.state_dir = next();
            have_state = true;
        } else if (a == "--out") {
            cfg.out_path = next();
            have_out = true;
        } else if (a == "--seed") {
            cfg.seed = std::stoull(next());
            have_seed = true;
        } else if (a == "--force-refresh") {
            cfg.force_refresh = true;
        } else if (a == "--budget-s") {
            cfg.budget_s = std::stod(next());
        } else if (a == "--audit-out") {
            cfg.audit_out_path = next();
        } else if (a == "--update-only") {
            cfg.update_only = true;
        } else {
            SYMPSICA_REQUIRE(false, "party_main: unrecognized flag");
        }
    }
    // --update-only needs neither --listen/--connect nor --out (task-24-
    // brief.md SC1/CG-A: it constructs no Channel and writes no JSONL
    // records, since it never reaches a query day) -- every other flag
    // (including --seed, unread in this mode, kept required for a uniform
    // CLI contract) stays mandatory exactly as before.
    SYMPSICA_REQUIRE(have_role && have_schedule && have_state && have_seed &&
                          (cfg.update_only || (have_addr && have_out)),
                      "party_main: missing a required flag (--role/--schedule/--state/--seed, "
                      "plus --listen|--connect/--out unless --update-only)");
    cfg.address = cfg.is_server ? ("127.0.0.1:" + listen_port) : connect_addr;
    return cfg;
}

std::string bool_json(bool b) { return b ? "true" : "false"; }

std::string config_json(const Config& cfg) {
    std::ostringstream oss;
    oss << "{\"role\":\"" << cfg.role_char << "\""
        << ",\"is_server\":" << bool_json(cfg.is_server) << ",\"address\":\"" << cfg.address << "\""
        << ",\"schedule\":\"" << cfg.schedule_path << "\",\"state\":\"" << cfg.state_dir << "\""
        << ",\"out\":\"" << cfg.out_path << "\",\"seed\":" << cfg.seed
        << ",\"force_refresh\":" << bool_json(cfg.force_refresh) << ",\"budget_s\":"
        << (cfg.budget_s > 0 ? std::to_string(cfg.budget_s) : std::string("null")) << "}";
    return oss.str();
}

// ---------------------------------------------------------------------------
// Channel connection, with a client-side retry loop (the listener side may
// not have started accepting yet by the time this process's connect()
// attempts its first dial -- same retry pattern kat_setup.cpp/kat_query.cpp
// already use for their own two-thread harnesses; here it also covers the
// two-PROCESS launch race the smoke E2E driver introduces).
// ---------------------------------------------------------------------------

std::unique_ptr<Channel> connect_channel(const Config& cfg) {
    if (cfg.is_server) {
        return std::make_unique<Channel>(cfg.address, /*is_server=*/true);
    }
    std::unique_ptr<Channel> ch;
    for (int attempt = 0; attempt < 400 && !ch; ++attempt) {
        try {
            ch = std::make_unique<Channel>(cfg.address, /*is_server=*/false);
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    SYMPSICA_REQUIRE(ch != nullptr, "party_main: failed to connect to peer at --connect address");
    return ch;
}

// ---------------------------------------------------------------------------
// R-SYMPROV: day-boundary provisioning handshake, exchanged separately from
// (but at the same per-day point as) R-SCHED2's ScheduleSync::sync -- kept
// as its own Channel::exchange() round rather than folding into
// ScheduleSync::digest()'s fixed, already-tested 8-byte contract (that
// contract is exercised directly by kat_salt.cpp's FC1; extending its wire
// shape would entangle two independently-testable concerns).
//
// Wire: 24 bytes = {u64 my_size, u64 my_triples_remaining,
// u64 my_gates_remaining}, LE. The ACTUAL asymmetric-exhaustion mitigation
// (R-SYMPROV's "top BOTH parties up to the SAME target") comes from the
// caller computing `target` as a PUBLIC function of (my_size, their_size)
// alone -- both parties therefore compute the IDENTICAL target regardless
// of their own current remaining() counts, so refill_offline's per-party
// top-up-to-target is symmetric by construction; the remaining() fields
// exchanged here are diagnostic (they let a future extension short-circuit
// the refill when both sides already hold enough) rather than load-bearing
// for the symmetry guarantee itself -- documented in task-18-report.md.
// ---------------------------------------------------------------------------

struct ProvisionInfo {
    u64 size = 0;
    u64 triples_remaining = 0;
    u64 gates_remaining = 0;
};

void write_u64_le(u8* dst, u64 x) {
    for (int i = 0; i < 8; ++i) dst[i] = static_cast<u8>(x >> (8 * i));
}

u64 read_u64_le(const u8* src) {
    u64 x = 0;
    for (int i = 0; i < 8; ++i) x |= static_cast<u64>(src[i]) << (8 * i);
    return x;
}

ProvisionInfo exchange_provision(Channel& ch, u64 my_size, const Pools& pools) {
    u8 out_buf[24];
    write_u64_le(out_buf + 0, my_size);
    write_u64_le(out_buf + 8, static_cast<u64>(pools.triples.remaining()));
    write_u64_le(out_buf + 16, static_cast<u64>(pools.gates.remaining()));
    u8 in_buf[24];
    ch.exchange(std::span<const u8>(out_buf, 24), std::span<u8>(in_buf, 24));

    ProvisionInfo info;
    info.size = read_u64_le(in_buf + 0);
    info.triples_remaining = read_u64_le(in_buf + 8);
    info.gates_remaining = read_u64_le(in_buf + 16);
    return info;
}

// R-REFILL-CALLER's tight bound: incremental always pads to EXACTLY
// 2*Params::U_MAX (fixed, independent of nA/nB -- query.cpp's own R-ROUND0
// wire layout); the full path's union is bounded by
// min(nA,M)+min(nB,M) <= nA+nB. Rather than duplicating SwitchRule::decide's
// own path-selection logic here (a second copy that could drift out of sync
// with query.cpp's real decision), this takes the max of the two documented
// bounds -- always safe (covers whichever path Query::run/SaltManager::
// refresh actually takes) and still tight relative to Params::M (which
// would be a wildly oversized, non-"actual bound" choice task-18-brief.md's
// R-REFILL-CALLER explicitly warns against for smoke-scale schedules).
u64 pool_evaluation_target(u64 my_size, u64 their_size) {
    return std::max<u64>(2 * Params::U_MAX, my_size + their_size);
}

std::string path_label(SwitchRule::Path p) {
    switch (p) {
        case SwitchRule::Path::Incremental: return "incremental";
        case SwitchRule::Path::FullPublic: return "full_public";
        case SwitchRule::Path::FullAnnounced: return "full_announced";
    }
    return "unknown";
}

std::string jsonl_record(const std::string& day, u64 query_no, const std::string& path,
                          bool announce, u64 count, u64 online_bytes, u64 online_rounds, double wall_s) {
    std::ostringstream oss;
    oss << "{\"day\":\"" << day << "\",\"query_no\":" << query_no << ",\"path\":\"" << path << "\""
        << ",\"announce\":" << bool_json(announce) << ",\"count\":" << count
        << ",\"online_bytes\":" << online_bytes << ",\"online_rounds\":" << online_rounds
        << ",\"wall_s\":" << wall_s << "}";
    return oss.str();
}

// task-23-brief.md R6-AUDIT-SCALE: the E2E-scale full-transcript audit dump
// (W6.4/W6.6(ii)), written ONLY when --audit-out is passed. Dumps the raw
// facts test/e2e/run_e2e_gate.py needs for the three of R6-AUDIT-TRANSCRIPT's
// four properties that are independently checkable from outside the pool
// (see audit_pool_transcript.hpp's header for why the fourth, cross-refill
// global uniqueness, is relied on via CorrelationPool::refill's own
// enforcement instead) -- generated()/consumed_ids()/remaining() (already-existing
// CorrelationPool<T> accessors, core/pools.hpp) plus the shared
// next_corr_id (already a public Pools field, protocols/setup.hpp) -- and
// nothing else; this binary does not itself assert anything about them
// (business-logic assertions on this data live in the Python driver, same
// as every other SC/FC check that script already makes).
std::string audit_json(const Pools& pools) {
    auto dump_ids = [](const std::vector<u64>& ids) {
        std::ostringstream oss;
        oss << "[";
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if (i != 0) oss << ",";
            oss << ids[i];
        }
        oss << "]";
        return oss.str();
    };
    std::ostringstream oss;
    oss << "{\"next_corr_id\":" << pools.next_corr_id << ",\"triples\":{\"generated\":"
        << pools.triples.generated() << ",\"remaining\":" << pools.triples.remaining()
        << ",\"consumed_ids\":" << dump_ids(pools.triples.consumed_ids()) << "}"
        << ",\"gates\":{\"generated\":" << pools.gates.generated()
        << ",\"remaining\":" << pools.gates.remaining()
        << ",\"consumed_ids\":" << dump_ids(pools.gates.consumed_ids()) << "}}";
    return oss.str();
}

} // namespace

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);

    const Role role = cfg.role_char == 'r' ? Role::Receiver : Role::Sender;

    std::filesystem::create_directories(cfg.state_dir);
    const std::string state_path = (std::filesystem::path(cfg.state_dir) / "state.bin").string();

    // R6-N2 (task-26-brief.md, plan-review R3): load state BEFORE
    // constructing Params, and construct Params.oracle FROM the loaded
    // state's persisted salt -- never unconditionally epoch-0. Order
    // matters: a state file committed after a maintenance day carries a
    // table/cache built under the FRESH salt; constructing Params first
    // (the pre-fix order) always started at epoch 0 regardless, so every
    // Update::apply/Query::run after a restart silently bucketed under the
    // wrong oracle (N2). This block must run before ANY Update/Setup/Query
    // use of `params`.
    PartyState st;
    if (std::filesystem::exists(state_path)) {
        st.load(state_path);
    } else {
        // Fresh party: explicit zero-init beyond PartyState's own in-class
        // defaults (my_size/query_no/oracle_salt already default to 0) --
        // st.t_share is otherwise indeterminate until the first committed
        // query's REPLACE semantics overwrite it (harmless in practice,
        // since query_no==0 always forces SwitchRule::decide's firstQuery
        // branch, but explicit is cheaper than reasoning about it).
        st.t_share = Share{Fp(0)};
    }

    Params params = Params::instantiate();
    params.oracle = BucketOracle(st.oracle_salt); // R6-N2: restore the persisted salt, not epoch 0

    // stderr config echo (W5.7): Params::echo() + the effective CLI flags
    // as one JSON line. Params::echo() prints salt() -- on a reload this
    // now shows the RESTORED salt (all-zero only for a genuinely fresh
    // party), which is itself a check (task-26-brief.md's revised N2
    // instructions: "the echo must show the restored salt, not zeros").
    params.echo(std::cerr);
    std::cerr << config_json(cfg) << "\n";
    std::cerr.flush();

    const std::vector<ScheduleDay> schedule = parse_schedule(read_file(cfg.schedule_path));

    // task-24-brief.md SC1/SC2/CG-A: zero-communication update-only mode.
    // test/core/grep_guard_no_channel_update_only.sh scans the marked block
    // immediately below for the bare token "Channel" (its own header
    // comment explains why a bare-word scan, not a narrower "Channel("-style
    // pattern) -- keep the block itself, and any string literal inside it,
    // free of that token; this explanatory comment sits OUTSIDE the markers
    // specifically so its own prose (which legitimately needs to say
    // "Channel") is not itself scanned. Update::apply is independently
    // proven Channel-free by the guard's second check: update.hpp/
    // update.cpp (include/sympsica/protocols/update.hpp,
    // src/protocols/update.cpp) never #include net.hpp at all, so the
    // Channel type is not even nameable there.
    if (cfg.update_only) {
        // BEGIN-UPDATE-ONLY-NO-CHANNEL
        for (const ScheduleDay& day : schedule) {
            SYMPSICA_REQUIRE(!day.query,
                              "party_main: --update-only mode forbids a query day in the schedule "
                              "(a query needs network communication, which this mode never performs)");
            Update::apply(st, day.insert_ids, day.delete_ids, params.encoder, params.oracle);
        }
        st.save(state_path);
        std::cerr << "channel_construction_count=" << Channel::construction_count() << "\n";
        return 0;
        // END-UPDATE-ONLY-NO-CHANNEL
    }

    std::unique_ptr<Channel> ch = connect_channel(cfg);

    Pools pools = Setup::run(role, *ch, params, PoolSizes{0, 0});

    // task-23-brief.md R6-CGB-PROMOTE: the post-Setup PkOpCounter snapshot,
    // additive to the pre-existing final-value line below (was: only the
    // final value was ever printed). test/e2e/run_e2e_gate.py greps BOTH
    // lines and asserts they're equal (and nonzero, SC4) at the end of
    // every E2E seed run -- promoting the old unasserted stderr print into
    // a real gate, per this task's own claim-B (CLM-B) requirement.
    std::cerr << "pkop_counter_after_setup=" << PkOpCounter::value() << "\n";

    // task-19-brief.md carried item (b): std::ios::trunc (unchanged, a
    // deliberate choice, not an oversight). A crash-matrix or E2E replay
    // that re-invokes THIS binary against the SAME --out path would lose
    // earlier records; instead of switching to append (which would risk
    // silently mixing an old run's stale JSONL lines into a fresh one if a
    // caller ever forgot to clean up), every driver in this task
    // (test/e2e/run_e2e_gate.py, test/e2e/run_smoke.py) always launches
    // party_main against a FRESH, distinct --out path per invocation --
    // never re-invoking it against a pre-existing one. The crash matrix
    // itself (test/e2e/run_crash_matrix.py) does not even go through
    // party_main/--out at all (apps/crash_probe.cpp, a separate minimal
    // binary, has no --out flag).
    std::ofstream out(cfg.out_path, std::ios::out | std::ios::trunc);
    SYMPSICA_REQUIRE(out.is_open(), "party_main: failed to open --out JSONL file for writing");

    const auto t_program_start = std::chrono::steady_clock::now();
    bool budget_exceeded = false;

    for (const ScheduleDay& day : schedule) {
        if (budget_exceeded) break;

        // R-SCHED2: pair-drift guard, BEFORE Update::apply (salt.hpp's own
        // ScheduleSync doc comment).
        ScheduleSync::sync(*ch, day.day, day.query, day.maintenance);

        Update::apply(st, day.insert_ids, day.delete_ids, params.encoder, params.oracle);

        if (day.query) {
            // R-SYMPROV: provisioning handshake + symmetric top-up, BEFORE
            // the query itself (R-REFILL-CALLER: party_main, not Query,
            // owns between-query pool maintenance).
            ProvisionInfo their = exchange_provision(*ch, st.my_size, pools);
            const u64 target = pool_evaluation_target(st.my_size, their.size);
            Setup::refill_offline(pools, role, *ch, params, PoolSizes{44 * target, 4 * target});

            const bool due = SaltManager::due(st.query_no, Params::PHI, day.maintenance, cfg.force_refresh);

            const auto t0 = std::chrono::steady_clock::now();
            const u64 bytes_before = ch->bytes_sent();
            const u64 sends_before = ch->sends_count();

            Share result{};
            std::string plabel;
            bool announce = false;
            if (due) {
                result = SaltManager::refresh(role, st, params, pools, *ch, state_path, cfg.seed);
                plabel = "maintenance_full";
            } else {
                SwitchRule::Path taken{};
                result = Query::run(role, st, pools, *ch, params, state_path, cfg.seed,
                                     /*force_full=*/false, &taken);
                plabel = path_label(taken);
                announce = (taken == SwitchRule::Path::FullAnnounced);
            }
            const u64 count = Query::open_count(*ch, result);

            const u64 online_bytes = ch->bytes_sent() - bytes_before;
            const u64 online_rounds = ch->sends_count() - sends_before;
            const double wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

            // st.query_no already advanced past this event's own commit
            // (Query::run's / SaltManager::refresh's atomic commit runs
            // strictly before this point).
            out << jsonl_record(day.day, st.query_no, plabel, announce, count, online_bytes,
                                 online_rounds, wall_s)
                << "\n";
            out.flush();
        }

        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t_program_start).count();
        if (cfg.budget_s > 0 && elapsed >= cfg.budget_s) {
            std::cerr << "party_main: --budget-s (" << cfg.budget_s << "s) exceeded after day \""
                      << day.day << "\" -- finishing current query and stopping (no mid-query abort)\n";
            budget_exceeded = true;
        }
    }

    out.close();

    // CLM-B cross-check (task-19-brief.md SC6/R-CLMB): PkOpCounter's final
    // value, echoed to stderr -- test/e2e/run_e2e_gate.py greps this so a
    // real E2E-scale schedule run gives a bonus, non-primary confirmation
    // alongside test/protocols_heavy/kat_clmb.cpp's own dedicated (small-
    // scale) test that PkOpCounter stays constant after Setup::run.
    std::cerr << "pkop_counter=" << PkOpCounter::value() << "\n";

    // task-23-brief.md R6-AUDIT-SCALE: the W6.4/W6.6(ii) audit dump, OFF by
    // default (empty cfg.audit_out_path). Additive only -- no existing
    // consumer of this binary passes --audit-out, so this is a no-op for
    // every caller except test/e2e/run_e2e_gate.py.
    if (!cfg.audit_out_path.empty()) {
        std::ofstream audit_out(cfg.audit_out_path, std::ios::out | std::ios::trunc);
        SYMPSICA_REQUIRE(audit_out.is_open(), "party_main: failed to open --audit-out file for writing");
        audit_out << audit_json(pools);
        audit_out.close();
    }

    return 0;
}

// apps/crash_probe.cpp — Task 19's SC3 crash-matrix helper binary
// (task-19-brief.md W5.8, controller ruling R-CRASHDRV: "the crash matrix
// is driven by SUBPROCESSES, not in-process fork ... a small C++ helper
// binary re-exec'ing itself with SYMPSICA_CRASH_AT set -- your choice,
// document it"). Chosen over party_main.cpp's own --schedule/JSONL
// machinery because the crash matrix needs precise, minimal control over
// exactly TWO queries per party (a "pre" query that gets saved cleanly,
// and a "target" query where SYMPSICA_CRASH_AT may fire) without
// party_main's day-loop/ScheduleSync/JSONL overhead getting in the way of
// that control -- see test/e2e/run_crash_matrix.py (the python driver that
// launches this binary as a real subprocess, R-CRASHDRV) for the full
// mechanism and the golden-byte comparison it performs.
//
// Modes (--mode):
//   init  : fresh PartyState with my_ids=<--ids>, table.init'd, my_size set,
//           SAVED to --state (query_no=0). No networking, no peer needed.
//   inspect: loads --state, prints ONE line of its structural fields
//           (query_no, J size, cache size, my_size) to stdout, no
//           networking, no mutation. Added for the crash matrix's R-INVCHK
//           "post" comparison (task-19-report.md's own discovery): raw
//           on-disk bytes CANNOT be compared bit-for-bit across two
//           INDEPENDENT process runs of a successfully-committed query,
//           because Setup::run's base-OT/silent-OT randomness is seeded
//           from oc::sysRandomSeed() (protocols/detail/setup_ot.hpp) --
//           genuine, correct production behavior, not a defect -- so the
//           actual Share VALUES a completed query commits differ run to
//           run even with identical ids/--seed. What IS deterministic
//           (and what R-INVCHK's OWN second clause already asks for --
//           "every field is mutually consistent: query_no, J emptiness,
//           cache key set vs t_share") is every STRUCTURAL field: which
//           buckets are touched, my_size, query_no. The crash matrix's PRE
//           comparison stays a full bit-exact file comparison (achievable:
//           an UNTOUCHED file's bytes are trivially stable); the POST
//           comparison uses this mode's structural fields instead.
//   query : loads --state (must already exist -- run `init` first), applies
//           Update::apply(<--extra-ids>, [], enc, oracle) if --extra-ids is
//           non-empty (the "day 2" insert), then runs ONE real two-party
//           Query::run (fresh Setup::run pools sized for this tiny scale)
//           + Query::open_count, prints the count to stdout, and SAVES to
//           --state. SYMPSICA_CRASH_AT (env, read by crash_point() inside
//           Query::run's commit()/PartyState::save()) may fire during this
//           mode's own save() -- this is the crash-matrix's induction point.
//
// RECOVERY DESIGN NOTE (both PRE- and POST-crash cases use the SAME `query`
// mode, deliberately -- no separate "just re-derive, don't redo" mode):
// this crash matrix runs at deliberately tiny scale (a handful of ids),
// where SwitchRule::decide ALWAYS selects the full path (2*u_max is vastly
// larger than any size here) -- and the full path's commit() uses REPLACE
// semantics (kat_query.cpp's own QRYFULL test: "stale pre-seeded cache is
// fully replaced, not accumulated"), which makes a full-path query
// PROVABLY IDEMPOTENT when re-run against unchanged underlying table
// state: cache/t_share are recomputed purely as a deterministic function
// of the CURRENT table + supports, not accumulated onto whatever was there
// before. So redoing `query` mode after a POST-crash recovery (the query
// was already fully committed) recomputes the IDENTICAL cache/t_share (the
// correct count, unchanged) -- it just harmlessly advances query_no one
// extra step; redoing it after a PRE-crash recovery (the query was never
// committed at all) is simply "run the query for the first time, this
// time without a crash" -- ordinarily correct either way. See
// test/e2e/run_crash_matrix.py for the empirical confirmation on both
// crash-outcome branches.
//
// Flags: --role r|s --listen PORT|--connect host:port --state PATH --mode
// {init,query} --ids "csv" [--extra-ids "csv"] --seed N.

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "sympsica/core/state.hpp"
#include "sympsica/protocols/query.hpp"
#include "sympsica/protocols/setup.hpp"
#include "sympsica/protocols/update.hpp"
#include "sympsica/utils/common.hpp"
#include "sympsica/utils/net.hpp"
#include "sympsica/utils/params.hpp"

using namespace sympsica;

namespace {

std::vector<u64> parse_csv(const std::string& s) {
    std::vector<u64> out;
    if (s.empty()) return out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (!tok.empty()) out.push_back(std::stoull(tok));
    }
    return out;
}

struct Config {
    char role_char = 0;
    bool is_server = false;
    std::string address;
    std::string state_path;
    std::string mode;
    std::vector<u64> ids;
    std::vector<u64> extra_ids;
    u64 seed = 0;
};

Config parse_args(int argc, char** argv) {
    Config cfg;
    std::string listen_port, connect_addr;
    bool have_role = false, have_addr = false, have_state = false, have_mode = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            SYMPSICA_REQUIRE(i + 1 < argc, "crash_probe: missing value after flag");
            return std::string(argv[++i]);
        };
        if (a == "--role") {
            std::string r = next();
            SYMPSICA_REQUIRE(r == "r" || r == "s", "crash_probe: --role must be 'r' or 's'");
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
        } else if (a == "--state") {
            cfg.state_path = next();
            have_state = true;
        } else if (a == "--mode") {
            cfg.mode = next();
            SYMPSICA_REQUIRE(cfg.mode == "init" || cfg.mode == "query" || cfg.mode == "inspect",
                              "crash_probe: --mode must be init|query|inspect");
            have_mode = true;
        } else if (a == "--ids") {
            cfg.ids = parse_csv(next());
        } else if (a == "--extra-ids") {
            cfg.extra_ids = parse_csv(next());
        } else if (a == "--seed") {
            cfg.seed = std::stoull(next());
        } else {
            SYMPSICA_REQUIRE(false, "crash_probe: unrecognized flag");
        }
    }
    SYMPSICA_REQUIRE(have_role && have_state && have_mode, "crash_probe: missing --role/--state/--mode");
    if (cfg.mode == "query") {
        SYMPSICA_REQUIRE(have_addr, "crash_probe: --listen or --connect required for query mode");
    }
    cfg.address = cfg.is_server ? ("127.0.0.1:" + listen_port) : connect_addr;
    return cfg;
}

std::unique_ptr<Channel> connect_channel(const Config& cfg) {
    if (cfg.is_server) return std::make_unique<Channel>(cfg.address, /*is_server=*/true);
    std::unique_ptr<Channel> ch;
    for (int attempt = 0; attempt < 400 && !ch; ++attempt) {
        try {
            ch = std::make_unique<Channel>(cfg.address, /*is_server=*/false);
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    SYMPSICA_REQUIRE(ch != nullptr, "crash_probe: failed to connect to peer");
    return ch;
}

} // namespace

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);
    Params params = Params::instantiate();
    const Role role = cfg.role_char == 'r' ? Role::Receiver : Role::Sender;

    if (cfg.mode == "init") {
        PartyState st;
        st.my_ids = cfg.ids;
        st.table.init(cfg.ids, params.encoder, params.oracle);
        st.my_size = cfg.ids.size();
        st.t_share = Share{Fp(0)};
        std::filesystem::create_directories(std::filesystem::path(cfg.state_path).parent_path());
        st.save(cfg.state_path);
        std::cerr << "crash_probe: init OK, my_size=" << st.my_size << "\n";
        return 0;
    }

    SYMPSICA_REQUIRE(std::filesystem::exists(cfg.state_path),
                      "crash_probe: --state file must already exist for query/inspect mode (run "
                      "--mode init first)");
    PartyState st;
    st.load(cfg.state_path);

    if (cfg.mode == "inspect") {
        // No networking, no mutation -- structural fields only (see this
        // file's own doc comment on why POST-golden comparison uses this
        // instead of raw bytes).
        std::cout << "query_no=" << st.query_no << " J=" << st.J.size() << " cache=" << st.cache.size()
                   << " my_size=" << st.my_size << "\n";
        return 0;
    }

    std::unique_ptr<Channel> ch = connect_channel(cfg);

    // mode == "query": apply this call's extra-ids (if any -- the "day 2"
    // insert) BEFORE the query, then run ONE real query. Small, fixed-size
    // pools: this binary only ever evaluates a handful of buckets (the
    // crash matrix runs at intentionally tiny scale, R-SCALE19's spirit
    // applied by analogy -- this isn't one of R-SCALE19's own explicitly-
    // scaled legs).
    if (!cfg.extra_ids.empty()) {
        Update::apply(st, cfg.extra_ids, std::vector<u64>{}, params.encoder, params.oracle);
    }

    Pools pools = Setup::run(role, *ch, params, PoolSizes{4000, 400});
    Share result = Query::run(role, st, pools, *ch, params, cfg.state_path, cfg.seed);
    u64 count = Query::open_count(*ch, result);
    std::cout << count << "\n";
    std::cerr << "crash_probe: query OK, count=" << count << " query_no=" << st.query_no << "\n";
    return 0;
}

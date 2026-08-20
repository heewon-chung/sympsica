// test/campaign/mask_campaign_gen.cpp — task-24-brief.md W6.6(i)/R6-MASKSTAT:
// the ONE-SHOT campaign generator for the 10^5-mask uniformity fixture.
//
// Runs steps 1-5 of the REAL production pipeline
// (protocols/detail/ztgate_pipeline.hpp's generate_ztgates -- the same
// function protocols/setup.cpp calls) for `--count` gates, in-process
// between two coroutines connected by coproto::LocalAsyncSocket (same
// pattern as test/integration/w24_boundary_masks.cpp -- semantic pipeline
// coverage, not a real-TCP-scale measurement; that is w24_pool_gate.cpp's
// job and is orthogonal to what this fixture needs). For each gate this
// records ONLY the logical mask r = mask_half_R XOR mask_half_S (the
// step-1 output the chi-square test operates on) -- the DPF key material
// itself is generated (a real, complete ZtGate is produced by every call,
// same as production) but deliberately never written to the fixture or to
// disk anywhere (R6-DPFKEY's spirit: nothing needs a DPF key on disk here).
//
// R6-MASKSTAT: the 10^5 count is SPEC-SIDE (task-24-brief.md, binding) and
// is a hardcoded default here, not a knob meant to be shrunk for runtime
// convenience -- --count exists only so the FC-non-vacuity leg and manual
// debugging can request a smaller run; the ctest registration
// (CMakeLists.txt) never overrides the default.
//
// Fixture format: test/fixtures/*.fixture's existing line-based convention
// ("key value1 value2 ...", '#' comments) -- one line per mask,
// `mask <u64 decimal>`, so test/utils/fixture_support.hpp's existing
// Fixture::all("mask") reader works unmodified. Raw masks, not a
// pre-binned histogram (R6-MASKSTAT: "so any later statistic can be
// recomputed").

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "coproto/Socket/LocalAsyncSock.h"
#include "cryptoTools/Crypto/PRNG.h"
#include "macoro/trace.h"
#include "macoro/sync_wait.h"
#include "macoro/when_all.h"

#include "sympsica/protocols/detail/ztgate_pipeline.hpp"
#include "sympsica/utils/common.hpp"
#include "sympsica/utils/field.hpp"

namespace {

namespace zt = sympsica::ztgate;
namespace oc = osuCrypto;
using sympsica::u64;

void eval(macoro::task<>& t0, macoro::task<>& t1) {
    auto r = macoro::sync_wait(macoro::when_all_ready(std::move(t0), std::move(t1)));
    std::get<0>(r).result();
    std::get<1>(r).result();
}

u64 pool_size(u64 gates, u64 extra_rejections) {
    return zt::kOtsPerGate * gates + zt::kAndGates * extra_rejections;
}

struct Config {
    std::string out_path;
    u64 count = 100000;
    u64 chunk_gates = 2000;
    u64 seed = 824000;
};

Config parse_args(int argc, char** argv) {
    Config cfg;
    bool have_out = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            SYMPSICA_REQUIRE(i + 1 < argc, "mask_campaign_gen: missing value after flag");
            return std::string(argv[++i]);
        };
        if (a == "--out") {
            cfg.out_path = next();
            have_out = true;
        } else if (a == "--count") {
            cfg.count = std::stoull(next());
        } else if (a == "--chunk-gates") {
            cfg.chunk_gates = std::stoull(next());
        } else if (a == "--seed") {
            cfg.seed = std::stoull(next());
        } else {
            SYMPSICA_REQUIRE(false, "mask_campaign_gen: unrecognized flag");
        }
    }
    SYMPSICA_REQUIRE(have_out, "mask_campaign_gen: --out is required");
    return cfg;
}

} // namespace

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);

    std::array<coproto::LocalAsyncSocket, 2> sock = coproto::LocalAsyncSocket::makePair();
    std::array<oc::PRNG, 2> ot_prng{oc::PRNG(oc::block(cfg.seed, 0)), oc::PRNG(oc::block(cfg.seed, 1))};
    std::array<oc::PRNG, 2> gate_prng{oc::PRNG(oc::block(cfg.seed + 1, 0)),
                                       oc::PRNG(oc::block(cfg.seed + 1, 1))};

    std::vector<u64> masks;
    masks.reserve(cfg.count);

    const auto t_start = std::chrono::steady_clock::now();
    u64 total_rejected = 0, total_resample_rounds = 0;

    u64 done = 0;
    while (done < cfg.count) {
        const u64 this_chunk = std::min(cfg.chunk_gates, cfg.count - done);

        std::array<zt::OtPool, 2> pool;
        {
            auto t0 = zt::generate_ot_pool(zt::Role::Receiver, pool_size(this_chunk, /*extra_rejections=*/16),
                                            ot_prng[0], sock[0], pool[0]);
            auto t1 = zt::generate_ot_pool(zt::Role::Sender, pool_size(this_chunk, /*extra_rejections=*/16),
                                            ot_prng[1], sock[1], pool[1]);
            eval(t0, t1);
        }

        std::array<zt::PipelineOpts, 2> opts;
        opts[0].count = this_chunk;
        opts[1].count = this_chunk;
        std::array<std::vector<zt::ZtGateOut>, 2> out;
        std::array<zt::PipelineStats, 2> stats;
        {
            auto t0 = zt::generate_ztgates(zt::Role::Receiver, sock[0], opts[0], gate_prng[0], pool[0], out[0],
                                            stats[0]);
            auto t1 = zt::generate_ztgates(zt::Role::Sender, sock[1], opts[1], gate_prng[1], pool[1], out[1],
                                            stats[1]);
            eval(t0, t1);
        }

        SYMPSICA_REQUIRE(out[0].size() == this_chunk && out[1].size() == this_chunk,
                          "mask_campaign_gen: pipeline produced the wrong gate count for this chunk");
        for (u64 g = 0; g < this_chunk; ++g) {
            masks.push_back(out[0][g].mask_half ^ out[1][g].mask_half);
        }
        total_rejected += stats[0].rejected;
        total_resample_rounds += stats[0].resample_rounds;

        done += this_chunk;
        const double elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
        std::fprintf(stderr, "[mask_campaign_gen] progress: %llu/%llu gates (%.1fs elapsed)\n",
                     (unsigned long long)done, (unsigned long long)cfg.count, elapsed_s);
        std::fflush(stderr);
    }

    const double wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();

    std::ofstream f(cfg.out_path, std::ios::out | std::ios::trunc);
    SYMPSICA_REQUIRE(f.is_open(), "mask_campaign_gen: failed to open --out fixture file for writing");
    f << "# test/campaign/mask_campaign_gen.cpp -- task-24-brief.md W6.6(i)/R6-MASKSTAT\n";
    f << "# " << cfg.count << " raw 61-bit logical masks (r = mask_half_R XOR mask_half_S), generated\n";
    f << "# through the REAL production pipeline (protocols/detail/ztgate_pipeline.hpp). Test-run\n";
    f << "# artifacts, NOT live secrets -- see this task's report for the SHA256 and invocation line.\n";
    for (u64 m : masks) {
        f << "mask " << m << "\n";
    }
    f.close();

    std::printf(
        "{\"mask_campaign\":{\"count\":%llu,\"wall_s\":%.3f,\"seed\":%llu,\"rejected\":%llu,"
        "\"resample_rounds\":%llu,\"out\":\"%s\"}}\n",
        (unsigned long long)cfg.count, wall_s, (unsigned long long)cfg.seed,
        (unsigned long long)total_rejected, (unsigned long long)total_resample_rounds, cfg.out_path.c_str());
    std::fflush(stdout);

    return 0;
}

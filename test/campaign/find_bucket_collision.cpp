// test/campaign/find_bucket_collision.cpp — task-25-brief.md R6-COLLIDE-PIN /
// R1 (plan-review revision): the committed search utility that finds a real
// FOUR-WAY collision under the production BucketOracle (m = 2^31 buckets,
// all-zero salt -- Params::instantiate()'s frozen default -- the exact same
// oracle every production party uses). This is a ONE-SHOT offline tool: its
// output (four colliding ids + their shared real bucket + the draw-index
// milestones at which multiplicity 2/3/4 were each first reached) is
// committed as named constants directly in
// test/gates/kat_multiid_bucket.cpp (R6-COLLIDE-PIN: "commit the ids it
// found as named constants in the test, so the test itself is instant and
// the exact values are pinned"). It is NOT re-run by a normal `ctest`
// invocation -- see its DISABLED ctest registration in CMakeLists.txt --
// only when regenerating the pinned constants (e.g. if the oracle ever
// changes, which the KAT's own re-verification assertion would catch).
//
// Deterministic and reproducible (R6-COLLIDE-PIN: "deterministic: fixed
// start value / fixed seed"): ids are drawn as the plain sequential
// integers start, start+1, start+2, ... -- BucketOracle::of is a BLAKE3 ROM
// instantiation, so sequential ids hash to effectively uniform buckets (no
// PRNG stream needed for reproducibility; the id itself already IS the
// draw index, so "the draw count at which multiplicity k was first
// reached" is simply the id value at that moment, minus `start - 1`).
//
// Algorithm (R1's "sort/chunk strategy is explicitly permitted... state
// what you used and the peak memory"): rather than a naive
// unordered_map<u32, vector<u64>> over ~2e7 draws (memory-hostile per R1 --
// most of those ~2e7 buckets would be singleton entries never needed
// again), this uses a flat std::vector<uint8_t> of size M = 2^31 (2 GiB)
// to track each bucket's hit count, CAPPED at 4. This bounds memory at
// O(M) = 2 GiB REGARDLESS of how many draws are needed (this machine has
// 64 GiB RAM; see task-25-report.md), rather than O(N draws). One
// streaming left-to-right pass finds:
//   - draws_first_pair:   the draw count at which SOME bucket first
//     reached multiplicity 2 (expected ballpark ~5.8e4, the classic
//     birthday-PAIR estimate the original brief text used).
//   - draws_first_triple: ditto for multiplicity 3.
//   - winner_bucket / draws_first_quad: the FIRST bucket to reach
//     multiplicity 4, and the draw count at which that happened (the
//     stopping condition; expected ballpark ~2e7 per R1's corrected
//     four-way estimate).
// The counting array itself does not store WHICH ids hit a bucket, only
// how many -- so a cheap SECOND pass (re-drawing ids 0..draws_first_quad,
// pure computation, no extra memory) recovers the exact 4 ids that hit
// winner_bucket.
//
// No hashmap anywhere: O(M) fixed memory (2 GiB), O(N) time, two passes.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "sympsica/utils/params.hpp"

using sympsica::BucketOracle;
using sympsica::Params;
using sympsica::u32;
using sympsica::u64;

namespace {

struct Args {
    u64 start = 1;                        // first id drawn (R6-COLLIDE-PIN: fixed start value)
    u64 max_draws = 200'000'000ull;        // safety cap; expected ballpark ~2e7 for a 4-way hit
};

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", arg.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--start") {
            a.start = std::stoull(next());
        } else if (arg == "--max-draws") {
            a.max_draws = std::stoull(next());
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", arg.c_str());
            std::exit(2);
        }
    }
    return a;
}

} // namespace

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);

    Params params = Params::instantiate();
    const BucketOracle& G = params.oracle;
    constexpr u64 M = BucketOracle::M; // 2^31

    std::fprintf(stderr,
                 "find_bucket_collision: start=%llu max_draws=%llu -- allocating %llu-byte "
                 "count array (M=2^31 buckets)...\n",
                 static_cast<unsigned long long>(args.start),
                 static_cast<unsigned long long>(args.max_draws),
                 static_cast<unsigned long long>(M));

    const auto t_alloc_start = std::chrono::steady_clock::now();
    std::vector<uint8_t> counts(static_cast<std::size_t>(M), 0); // index = bucket - 1
    const double alloc_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_alloc_start).count();
    std::fprintf(stderr, "find_bucket_collision: allocation+zero-init took %.3fs\n", alloc_s);

    const auto t_start = std::chrono::steady_clock::now();

    u64 draws_first_pair = 0, draws_first_triple = 0, draws_first_quad = 0;
    u32 winner_bucket = 0;
    bool found = false;

    u64 n_drawn = 0;
    for (; n_drawn < args.max_draws; ++n_drawn) {
        const u64 id = args.start + n_drawn;
        const u32 beta = G.of(id);
        uint8_t& c = counts[beta - 1];
        if (c < 4) ++c;
        if (c == 2 && draws_first_pair == 0) draws_first_pair = n_drawn + 1;
        if (c == 3 && draws_first_triple == 0) draws_first_triple = n_drawn + 1;
        if (c == 4) {
            draws_first_quad = n_drawn + 1;
            winner_bucket = beta;
            found = true;
            break;
        }
        if ((n_drawn + 1) % 5'000'000ull == 0) {
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
            std::fprintf(stderr, "  ...%llu draws (%.1fs elapsed)\n",
                         static_cast<unsigned long long>(n_drawn + 1), elapsed);
        }
    }

    if (!found) {
        std::fprintf(stderr,
                     "find_bucket_collision: FAILED to find a four-way collision within %llu "
                     "draws\n",
                     static_cast<unsigned long long>(args.max_draws));
        return 1;
    }

    // Free the big counting array before the second pass -- not needed anymore.
    counts.clear();
    counts.shrink_to_fit();

    // Second pass: recover the exact 4 ids that hit winner_bucket (cheap: pure
    // recomputation, no extra memory beyond a 4-entry result vector).
    std::vector<u64> winners;
    for (u64 i = 0; i < draws_first_quad; ++i) {
        const u64 wid = args.start + i;
        if (G.of(wid) == winner_bucket) {
            winners.push_back(wid);
            if (winners.size() == 4) break;
        }
    }

    const double elapsed_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();

    std::printf("start=%llu\n", static_cast<unsigned long long>(args.start));
    std::printf("bucket=%u\n", winner_bucket);
    std::printf("draws_first_pair=%llu\n", static_cast<unsigned long long>(draws_first_pair));
    std::printf("draws_first_triple=%llu\n", static_cast<unsigned long long>(draws_first_triple));
    std::printf("draws_first_quad=%llu\n", static_cast<unsigned long long>(draws_first_quad));
    std::printf("elapsed_seconds=%.3f\n", elapsed_s);
    if (winners.size() != 4) {
        std::fprintf(stderr, "INTERNAL ERROR: second pass found %zu ids, expected 4\n",
                     winners.size());
        return 1;
    }
    for (std::size_t i = 0; i < winners.size(); ++i) {
        std::printf("id%zu=%llu\n", i, static_cast<unsigned long long>(winners[i]));
    }
    return 0;
}

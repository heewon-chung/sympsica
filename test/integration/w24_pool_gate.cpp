// w24_pool_gate.cpp — W2.4: POOL-SCALE FEASIBILITY GATE (task-8 brief).
//
// Generates 512 complete ZtGates through the W2.1 pipeline (generate_ztgates,
// full steps 1-5) between TWO REAL OS PROCESSES connected by a real TCP
// socket pair on localhost (sympsica::Channel, via the socket() accessor
// this task adds to net.hpp) -- not coproto::LocalAsyncSocket. Records
// wall-clock, outgoing bytes and peak RSS per party, extrapolates linearly
// to the 8192-gate incremental-query pool, and FAILS the test if the
// extrapolation exceeds the 600s single-thread-loopback budget (this test
// IS the GO/NO-GO gate, not just a measurement).
//
// Process architecture (brief requirement 2, "your call, document"): a bare
// fork() from the test body, as the FIRST statement of the TEST (before
// either side has touched coproto/asio at all), rather than fork()+exec() of
// a re-invoked binary. Rationale: this test is the only place in the
// sympsica_tests_integration binary that touches a real coproto::asio socket
// (every other integration test uses LocalAsyncSocket, which never starts
// coproto's background io_context thread), so forking before any such
// contact avoids the classic fork()-after-threads-started hazard. The child
// never re-enters gtest machinery: it does its own protocol run with plain
// SYMPSICA_REQUIRE-style checks (no EXPECT/ASSERT, whose bookkeeping is
// process-local and would be invisible to the parent's test result), reports
// its numbers to the parent over a plain pipe, and calls _exit() directly
// (skipping atexit handlers, so gtest's own singletons/streams are not
// touched twice). The child's own real correctness data (per-sample DPF
// expansions) also travels over that same pipe -- NOT over the measured TCP
// channel, exactly mirroring how the in-process gtest tests in
// kat_dkg_vole61.cpp already have direct memory access to both parties'
// private outputs for verification purposes.
//
// Byte accounting (brief requirement 1): generate_ztgates drives
// coproto::Socket directly (send/recv on the type-erased socket the pipeline
// takes), never through sympsica::Channel::send()/recv() -- so
// Channel::bytes_sent() would read 0 for this traffic (see net.hpp's updated
// doc comment). bytes_out_{r,s} below are read from coproto::Socket's own
// bytesSent() counter (coproto/Socket/Socket.h), sampled immediately before
// and after the timed 512-gate loop on each party's own channel end.

#include <gtest/gtest.h>

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "cryptoTools/Crypto/PRNG.h"
// macoro/sync_wait.h uses std::source_location and basic_traceable but does
// not include macoro/trace.h itself -- it relies on an earlier transitive
// include (e.g. coproto/Socket/LocalAsyncSock.h -> macoro/barrier.h) that
// this file, which only needs a real TCP socket via sympsica::Channel and
// never touches LocalAsyncSocket, does not otherwise pull in. vendor/ is
// off-limits, so this file supplies the missing include directly instead.
#include "macoro/trace.h"
#include "macoro/sync_wait.h"

#include "sympsica/utils/coeff_ctx.hpp"
#include "sympsica/utils/field.hpp"
#include "sympsica/utils/net.hpp"
#include "ztgate_pipeline.hpp"

namespace {

namespace zt = sympsica::ztgate;
namespace oc = osuCrypto;
using sympsica::Channel;
using sympsica::Fp;
using sympsica::u64;

constexpr u64 kTotalGates = 512;
constexpr u64 kChunkSize = 64;  // brief requirement 4: progress printed every 64 gates
static_assert(kTotalGates % kChunkSize == 0, "chunking must partition kTotalGates exactly");

constexpr double kBudgetSeconds = 600.0;              // brief: <= 10 min single-thread loopback
constexpr double kExtrapolationFactor = 8192.0 / 512.0;  // 16x

const char* kAddress = "127.0.0.1:45231";

// Pool size for `gates` gates plus `extra_rejections` forced-resample slack
// (Task 5 minor: OtPool::take has no top-up). Boundary masks aside, a
// sampled 61-bit mask is rejected with probability 2^-61, so 512 samples
// essentially never trigger a resample; the slack is kept anyway as cheap
// insurance (60 extra OTs per unit of slack).
u64 pool_size(u64 gates, u64 extra_rejections) {
    return zt::kOtsPerGate * gates + zt::kAndGates * extra_rejections;
}

// Deterministic (not random) choice of >= 8 sample gates, spread across the
// run so early- and late-chunk gates are both covered. "Seeded choice" per
// the brief is satisfied by this being a fixed, reviewable list rather than
// a coin flip; a PRNG-driven choice would need its seed transmitted (or
// independently reproduced) across the process boundary for no benefit here,
// since the set doesn't need to be secret or run-dependent.
std::vector<u64> sample_gate_indices() {
    return {0, 56, 112, 168, 224, 280, 336, 392, 448, 505};
}

// --- peak RSS ----------------------------------------------------------------
// getrusage's ru_maxrss unit is platform-specific: bytes on macOS/BSD,
// kilobytes on Linux. Normalised to bytes here; this project's build targets
// macOS/ARM (CLAUDE.md), so the #else arm is the documented Linux behaviour,
// not independently verified on this host.
u64 read_peak_rss_bytes() {
    struct rusage ru {};
    ::getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
    return static_cast<u64>(ru.ru_maxrss);
#else
    return static_cast<u64>(ru.ru_maxrss) * 1024;
#endif
}

// --- one sampled gate's local expansion, kept for cross-process reconstruction
struct SampleGateData {
    u64 gate_index = 0;
    u64 mask_half = 0;
    std::array<u64, 4> digit_shares{};
    u64 mask_share = 0;
    u64 noncanonical = 0;               // this party's own non-canonical share count
    std::vector<u64> values;            // size kNumDigits*kDomain, this party's Fp.v
    std::vector<oc::u8> tags;           // size kNumDigits*kDomain
};

// Full-domain expand of ONE gate's key, for ONE party -- exactly what
// oc::RegularDpf::expand offers (task-5-report.md (a).5: it is the only
// expansion API, non-interactive but whole-domain; there is no cheaper
// point-only evaluation to reach for instead).
SampleGateData expand_own(zt::Role role, zt::ZtGateOut& g, u64 gate_index) {
    sympsica::CoeffCtxFp61 ctx;
    SampleGateData d;
    d.gate_index = gate_index;
    d.mask_half = g.mask_half;
    d.digit_shares = g.digit_shares;
    d.mask_share = g.mask_share.v;

    const u64 n = zt::kNumDigits * zt::kDomain;
    d.values.assign(n, 0);
    d.tags.assign(n, 0);
    u64 noncanon = 0;
    auto sink = [&](auto k, auto i, auto&& v, oc::block t) {
        const u64 idx = k * zt::kDomain + i;
        d.values[idx] = v.v;
        noncanon += (v.v >= Fp::P);
        d.tags[idx] = t.template get<oc::u8>(0) & 1;
    };
    oc::RegularDpf<Fp, sympsica::CoeffCtxFp61>::expand(zt::party_idx(role), zt::kDomain, g.key, sink, ctx);
    d.noncanonical = noncanon;
    return d;
}

// --- one party's full measured run --------------------------------------------
struct PartyResult {
    double wall_s = 0.0;
    u64 bytes_out = 0;
    u64 peak_rss = 0;
    u64 ots_consumed = 0;
    u64 rejected = 0;
    u64 resample_rounds = 0;
    std::vector<SampleGateData> samples;
};

// Runs the OT pool fill + all 512 gates (in kChunkSize-gate driver-level
// chunks, purely so progress can be printed every 64 gates -- brief
// requirement 2b's "driver-level chunking wrapper", applied here for
// progress reporting rather than performance: each chunk issues the exact
// same per-gate protocol messages as one 512-gate call would, over the SAME
// socket, single-threaded, sequentially. wall_s/bytes_out are scoped to
// exactly this loop, matching "wall-clock around the pipeline call").
PartyResult run_party(zt::Role role, Channel& ch, const char* tag) {
    PartyResult pr;

    oc::PRNG ot_prng(oc::block(0x57324ull, zt::party_idx(role)));
    zt::OtPool pool;
    macoro::sync_wait(
        zt::generate_ot_pool(role, pool_size(kTotalGates, /*extra_rejections=*/4), ot_prng, ch.socket(), pool));

    oc::PRNG gate_prng(oc::block(0x57325ull, zt::party_idx(role)));
    std::vector<zt::ZtGateOut> all_out;
    all_out.reserve(kTotalGates);
    zt::PipelineStats total_stats{};

    const u64 bytes_before = static_cast<u64>(ch.socket().bytesSent());
    const auto t_start = std::chrono::steady_clock::now();
    for (u64 c = 0; c < kTotalGates / kChunkSize; ++c) {
        zt::PipelineOpts opts;
        opts.count = kChunkSize;
        std::vector<zt::ZtGateOut> chunk_out;
        zt::PipelineStats chunk_stats;
        macoro::sync_wait(zt::generate_ztgates(role, ch.socket(), opts, gate_prng, pool, chunk_out, chunk_stats));
        for (auto& g : chunk_out) all_out.push_back(std::move(g));
        total_stats.rejected += chunk_stats.rejected;
        total_stats.resample_rounds += chunk_stats.resample_rounds;
        total_stats.ots_consumed += chunk_stats.ots_consumed;
        std::fprintf(stderr, "[W24 %s] progress: %llu/%llu gates\n", tag,
                     (unsigned long long)all_out.size(), (unsigned long long)kTotalGates);
    }
    const auto t_end = std::chrono::steady_clock::now();
    const u64 bytes_after = static_cast<u64>(ch.socket().bytesSent());

    pr.wall_s = std::chrono::duration<double>(t_end - t_start).count();
    pr.bytes_out = bytes_after - bytes_before;
    pr.peak_rss = read_peak_rss_bytes();  // scoped: taken right after gate generation,
                                           // before the sample-expansion pass below
    pr.ots_consumed = total_stats.ots_consumed;
    pr.rejected = total_stats.rejected;
    pr.resample_rounds = total_stats.resample_rounds;

    for (u64 idx : sample_gate_indices())
        pr.samples.push_back(expand_own(role, all_out[idx], idx));

    return pr;
}

// --- pipe IPC: child -> parent (test-only side channel, not the measured TCP
// path). Both ends are the SAME compiled binary (fork, not exec), so raw
// struct/array memcpy over the pipe is safe: no cross-ABI concern.

void pipe_write_all(int fd, const void* data, std::size_t n) {
    const auto* p = static_cast<const char*>(data);
    std::size_t off = 0;
    while (off < n) {
        ssize_t w = ::write(fd, p + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            SYMPSICA_REQUIRE(false, "w24_pool_gate: pipe write failed");
        }
        off += static_cast<std::size_t>(w);
    }
}

void pipe_read_all(int fd, void* data, std::size_t n) {
    auto* p = static_cast<char*>(data);
    std::size_t off = 0;
    while (off < n) {
        ssize_t r = ::read(fd, p + off, n - off);
        if (r < 0) {
            if (errno == EINTR) continue;
            SYMPSICA_REQUIRE(false, "w24_pool_gate: pipe read failed");
        }
        SYMPSICA_REQUIRE(r != 0, "w24_pool_gate: pipe closed early (child crashed?)");
        off += static_cast<std::size_t>(r);
    }
}

struct ChildSummary {
    double wall_s = 0.0;
    u64 bytes_out = 0;
    u64 peak_rss = 0;
    u64 ots_consumed = 0;
    u64 rejected = 0;
    u64 resample_rounds = 0;
    u64 num_samples = 0;
};

struct SampleHeaderWire {
    u64 gate_index;
    u64 mask_half;
    u64 digit_shares[4];
    u64 mask_share;
    u64 noncanonical;
};

void write_child_result(int fd, const PartyResult& pr) {
    ChildSummary s;
    s.wall_s = pr.wall_s;
    s.bytes_out = pr.bytes_out;
    s.peak_rss = pr.peak_rss;
    s.ots_consumed = pr.ots_consumed;
    s.rejected = pr.rejected;
    s.resample_rounds = pr.resample_rounds;
    s.num_samples = static_cast<u64>(pr.samples.size());
    pipe_write_all(fd, &s, sizeof(s));

    for (const auto& sample : pr.samples) {
        SampleHeaderWire hdr{};
        hdr.gate_index = sample.gate_index;
        hdr.mask_half = sample.mask_half;
        for (int k = 0; k < 4; ++k) hdr.digit_shares[k] = sample.digit_shares[k];
        hdr.mask_share = sample.mask_share;
        hdr.noncanonical = sample.noncanonical;
        pipe_write_all(fd, &hdr, sizeof(hdr));
        pipe_write_all(fd, sample.values.data(), sample.values.size() * sizeof(u64));
        pipe_write_all(fd, sample.tags.data(), sample.tags.size() * sizeof(oc::u8));
    }
}

void read_child_result(int fd, ChildSummary& out_summary, std::vector<SampleGateData>& out_samples) {
    pipe_read_all(fd, &out_summary, sizeof(out_summary));
    out_samples.resize(out_summary.num_samples);
    const u64 n = zt::kNumDigits * zt::kDomain;
    for (u64 i = 0; i < out_summary.num_samples; ++i) {
        SampleHeaderWire hdr{};
        pipe_read_all(fd, &hdr, sizeof(hdr));
        SampleGateData& d = out_samples[i];
        d.gate_index = hdr.gate_index;
        d.mask_half = hdr.mask_half;
        for (int k = 0; k < 4; ++k) d.digit_shares[k] = hdr.digit_shares[k];
        d.mask_share = hdr.mask_share;
        d.noncanonical = hdr.noncanonical;
        d.values.resize(n);
        d.tags.resize(n);
        pipe_read_all(fd, d.values.data(), n * sizeof(u64));
        pipe_read_all(fd, d.tags.data(), n * sizeof(oc::u8));
    }
}

} // namespace

TEST(W24PoolGate, PoolScaleFeasibilityGate512GatesOverRealTcp) {
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0) << "pipe() failed: " << std::strerror(errno);

    // Fork as the very first statement -- see this file's header comment for
    // why (no coproto/asio contact anywhere in this binary before this
    // point).
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0) << "fork() failed: " << std::strerror(errno);

    if (pid == 0) {
        // ---- child process: Role::Sender, TCP server (listens first) ----
        ::close(fds[0]);
        Channel ch(kAddress, /*is_server=*/true);
        PartyResult pr = run_party(zt::Role::Sender, ch, "S");
        write_child_result(fds[1], pr);
        std::fflush(stderr);
        ::close(fds[1]);
        _exit(0);  // not exit(): skip atexit handlers (gtest/coproto singletons
                   // must not be torn down twice across the fork).
    }

    // ---- parent process: Role::Receiver, TCP client (connects, with retry:
    // the child's accept() can race this connect(), same pattern as
    // test/utils/net_smoke.cpp's connect_client_with_retry) ----
    ::close(fds[1]);
    std::unique_ptr<Channel> ch;
    for (int attempt = 0; attempt < 300 && !ch; ++attempt) {
        try {
            ch = std::make_unique<Channel>(kAddress, /*is_server=*/false);
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    ASSERT_NE(ch, nullptr) << "parent failed to connect to the child's loopback TCP server";

    PartyResult pr_r = run_party(zt::Role::Receiver, *ch, "R");

    ChildSummary child_summary;
    std::vector<SampleGateData> child_samples;
    read_child_result(fds[0], child_summary, child_samples);
    ::close(fds[0]);

    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0)
        << "child process (Role::Sender) did not exit cleanly, status=" << status;

    // --- correctness on the sample (Task-5 assert block: point-function +
    // tag reconstruction + canonicality), cross-process ----------------------
    ASSERT_EQ(child_samples.size(), sample_gate_indices().size());
    ASSERT_EQ(pr_r.samples.size(), sample_gate_indices().size());

    for (std::size_t i = 0; i < pr_r.samples.size(); ++i) {
        const auto& own = pr_r.samples[i];
        const auto& peer = child_samples[i];
        ASSERT_EQ(own.gate_index, peer.gate_index);
        SCOPED_TRACE("sample gate " + std::to_string(own.gate_index));

        EXPECT_EQ(own.noncanonical, 0u) << "party R";
        EXPECT_EQ(peer.noncanonical, 0u) << "party S";

        const u64 r = own.mask_half ^ peer.mask_half;
        EXPECT_LT(r, Fp::P);
        const auto want_digits = zt::digit_split(r);
        for (u64 k = 0; k < zt::kNumDigits; ++k)
            EXPECT_EQ(own.digit_shares[k] ^ peer.digit_shares[k], want_digits[k]) << "digit " << k;
        EXPECT_EQ(Fp(own.mask_share).add(Fp(peer.mask_share)).v, Fp::from_u64(r).v);

        ASSERT_EQ(own.values.size(), zt::kNumDigits * zt::kDomain);
        ASSERT_EQ(peer.values.size(), own.values.size());
        u64 bad_value = 0, bad_tag = 0;
        for (u64 k = 0; k < zt::kNumDigits; ++k) {
            for (u64 leaf = 0; leaf < zt::kDomain; ++leaf) {
                const u64 idx = k * zt::kDomain + leaf;
                const u64 want = (leaf == want_digits[k]) ? 1u : 0u;
                const u64 recon_value = Fp(own.values[idx]).add(Fp(peer.values[idx])).v;
                const u64 recon_tag = own.tags[idx] ^ peer.tags[idx];
                bad_value += (recon_value != want);
                bad_tag += (recon_tag != want);
            }
        }
        EXPECT_EQ(bad_value, 0u) << "point-function reconstruction, full domain";
        EXPECT_EQ(bad_tag, 0u) << "tag reconstruction, full domain";
    }

    // --- W2.4 numbers: wall-clock, bytes, peak RSS, extrapolation, GO/NO-GO --
    // wall_s is the max of the two parties' own timed windows: the protocol
    // is synchronous over one TCP connection, so both windows are close in
    // practice, and the slower side is the honest bound on how long the
    // joint run actually took.
    const double wall_s = std::max(pr_r.wall_s, child_summary.wall_s);
    const double extrapolated_8192_s = wall_s * kExtrapolationFactor;
    const bool go = extrapolated_8192_s <= kBudgetSeconds;

    std::printf(
        "{\"w24\":{\"gates\":%llu,\"wall_s\":%.6f,\"bytes_out_r\":%llu,\"bytes_out_s\":%llu,"
        "\"peak_rss_r\":%llu,\"peak_rss_s\":%llu,\"extrapolated_8192_s\":%.6f,\"go\":%s}}\n",
        (unsigned long long)kTotalGates, wall_s, (unsigned long long)pr_r.bytes_out,
        (unsigned long long)child_summary.bytes_out, (unsigned long long)pr_r.peak_rss,
        (unsigned long long)child_summary.peak_rss, extrapolated_8192_s, go ? "true" : "false");
    std::fflush(stdout);

    std::fprintf(stderr,
                 "[W24] party R (Receiver): wall_s=%.3f bytes_out=%llu peak_rss=%llu ots=%llu "
                 "rejected=%llu resample_rounds=%llu\n"
                 "[W24] party S (Sender):   wall_s=%.3f bytes_out=%llu peak_rss=%llu ots=%llu "
                 "rejected=%llu resample_rounds=%llu\n",
                 pr_r.wall_s, (unsigned long long)pr_r.bytes_out, (unsigned long long)pr_r.peak_rss,
                 (unsigned long long)pr_r.ots_consumed, (unsigned long long)pr_r.rejected,
                 (unsigned long long)pr_r.resample_rounds, child_summary.wall_s,
                 (unsigned long long)child_summary.bytes_out, (unsigned long long)child_summary.peak_rss,
                 (unsigned long long)child_summary.ots_consumed, (unsigned long long)child_summary.rejected,
                 (unsigned long long)child_summary.resample_rounds);

    // The gate itself: NO-GO fails the test (this test IS the SC gate for
    // W2.4), rather than only reporting a number for a human to eyeball.
    ASSERT_TRUE(go) << "W2.4 NO-GO: extrapolated 8192-gate provisioning = " << extrapolated_8192_s
                     << "s exceeds the " << kBudgetSeconds << "s budget (measured " << wall_s
                     << "s over " << kTotalGates << " gates)";
}

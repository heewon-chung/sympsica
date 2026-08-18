// test/utils/net_exchange.cpp — Task 15 (task-15-brief.md): the NetExchange
// suite for Channel::exchange() (include/sympsica/utils/net.hpp), the
// deadlock-safe full-duplex send+recv primitive that retires the
// send-then-recv symmetric-open hazard Phase 4's final review flagged
// (src/gates/beaver.cpp's d/e open, src/gates/ztest.cpp's masked-z open;
// both already migrated to exchange() as part of this task).
//
// Test IDs carried in the test names:
//   SC1     — correctness: asymmetric out/in sizes, bit-exact round trip
//             both directions, over real loopback TCP (net_smoke.cpp
//             pattern, two threads).
//   SC2     — DEADLOCK STRESS: both parties exchange() >= 4 MiB each way
//             SIMULTANEOUSLY (>= 6x the largest Phase-5 single-layer send
//             of 655 KB), repeated >= 3x on one connection. This is the
//             regression net for the hazard (see FC-DLK ruling below), not
//             just a correctness check: a regression back to blocking
//             send()-then-recv() would deadlock on this buffer size long
//             before either side fully drains its peer, and the 60s ctest
//             TIMEOUT (CMakeLists.txt, `utils` target — see that file's
//             comment for why it is scoped to the whole target rather than
//             this one test) turns that hang into a hard, loud failure
//             instead of a silently-stalled CI run.
//   SC3     — counters: one exchange(out=N, in=M) advances bytes_sent() by
//             exactly N and sends_count() by exactly 1 on EACH party (net.
//             hpp's documented counter semantics — the recv half counts
//             nothing, same as plain recv() always has).
//   SC5     — production-path stress: a BeaverEngine::mul batch of
//             N = 40,960 products (the |U| = 2048 minors-L1 shape: 20
//             products/bucket * 2048 buckets), dealer triples, real
//             loopback TCP, under the same TIMEOUT discipline as SC2. This
//             exercises the ACTUAL migrated protocol call site
//             (src/gates/beaver.cpp), not just the exchange() primitive in
//             isolation; correctness is checked by reconstructing x*y on a
//             sampled subset (not all 40,960 — the point of this test is
//             the wire/timing path, not re-proving Beaver correctness,
//             which SD-3/SD-4/ZT-1/ZT-2 already cover exhaustively).
//   FC1     — negative: a peer that performs a PLAIN recv() of the WRONG
//             size opposite our exchange() call must hit a deterministic
//             error path, not hang. Traced (not assumed) through coproto's
//             source: SockScheduler::receiveDataTask
//             (coproto/Socket/SocketScheduler.h) detects the buffer-size
//             mismatch in its `buffer.size() != header.mSize` branch,
//             attaches `code::badBufferSize` to the mismatched
//             RecvOperation on the very next loop iteration
//             (AnyRecvOp::completePrev -> op.setError(...)), which
//             RecvAwaiterBase::await_resume() (coproto/Proto/Buffers.h)
//             converts into an uncaught BadReceiveBufferSize exception via
//             addTraceRethrow(). An uncaught exception escaping a
//             std::thread's entry function is standard-mandated to call
//             std::terminate() (default handler: abort()), killing the
//             WHOLE test process — including the other party's thread,
//             wherever it happened to be blocked — deterministically and
//             fast (bounded by loopback latency, not by anything hang-
//             shaped). Captured via EXPECT_DEATH; NOT downgraded to a
//             documented limitation, because this trace shows a real
//             exception path, not a hang.
//
// FC-DLK ruling (controller, task-15-brief.md, binding): NO deliberate-
// deadlock negative test is shipped in this file. A hang-based negative
// test is inherently flaky — its "pass" signal would be "the process did
// not hang long enough to trip the harness's timeout", which is a race
// against wall-clock time, not a property of the code. The hazard's
// regression net is SC2 (this primitive) and SC5 (the real protocol path)
// running under ctest's TIMEOUT: if exchange()'s concurrent composition
// ever regresses to blocking send()-then-recv(), those tests hang and
// TIMEOUT fails the suite loudly, which is the actual guarantee this task
// needs — a negative test that races the clock would not add anything a
// flaky test doesn't already subtract.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "sympsica/core/pools.hpp"
#include "sympsica/core/share.hpp"
#include "sympsica/gates/beaver.hpp"
#include "sympsica/utils/field.hpp"
#include "sympsica/utils/net.hpp"

using namespace sympsica;

namespace {

// ---------------------------------------------------------------------------
// Two-party TCP harness (net_smoke.cpp / test/gates/kat_ztest.cpp pattern).
// A dedicated port range (48100+) avoids collisions with net_smoke.cpp's
// fixed port (41231) inside this same sympsica_tests_utils binary.
// ---------------------------------------------------------------------------

std::atomic<int> g_port_counter{48100};

std::string next_address() {
    return "127.0.0.1:" + std::to_string(g_port_counter.fetch_add(1));
}

std::unique_ptr<Channel> connect_client_with_retry(const std::string& address) {
    std::unique_ptr<Channel> ch;
    for (int attempt = 0; attempt < 200 && !ch; ++attempt) {
        try {
            ch = std::make_unique<Channel>(address, /*is_server=*/false);
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    return ch;
}

template <typename ServerFn, typename ClientFn>
void run_two_party(const std::string& address, ServerFn&& server_fn, ClientFn&& client_fn) {
    std::thread server([&] {
        Channel ch(address, /*is_server=*/true);
        server_fn(ch);
    });
    auto client = connect_client_with_retry(address);
    ASSERT_NE(client, nullptr) << "client failed to connect to loopback server at " << address;
    client_fn(*client);
    server.join();
}

std::vector<u8> random_bytes(std::size_t n, u64 seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::vector<u8> out(n);
    for (auto& b : out) b = static_cast<u8>(byte_dist(rng));
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// SC1 — asymmetric out/in sizes (server: out=3000/in=1000; client: the
// mirror, out=1000/in=3000), bit-exact round trip both directions.
// ---------------------------------------------------------------------------

TEST(NetExchange, SC1_AsymmetricSizesRoundTripBitExactBothDirections) {
    const std::string address = next_address();
    const auto server_out = random_bytes(3000, 0xE1000001ull);
    const auto client_out = random_bytes(1000, 0xE1000002ull);

    std::vector<u8> server_in(1000);
    std::vector<u8> client_in(3000);

    run_two_party(
        address,
        [&](Channel& ch) {
            ch.exchange(std::span<const u8>(server_out), std::span<u8>(server_in));
        },
        [&](Channel& ch) {
            ch.exchange(std::span<const u8>(client_out), std::span<u8>(client_in));
        });

    EXPECT_EQ(server_in, client_out) << "server must receive exactly what the client sent";
    EXPECT_EQ(client_in, server_out) << "client must receive exactly what the server sent";
}

// ---------------------------------------------------------------------------
// SC2 — DEADLOCK STRESS: both parties exchange() >= 4 MiB each way
// simultaneously, repeated >= 3x on one connection. See file header comment
// for why this (not a deliberate hang-based negative) is the hazard's
// regression net, and CMakeLists.txt's `utils` target comment for the 60s
// ctest TIMEOUT this relies on.
// ---------------------------------------------------------------------------

TEST(NetExchange, SC2_DeadlockStressLargeBothWaysRepeatedOnOneConnection) {
    constexpr std::size_t kSize = 4u << 20; // 4 MiB
    constexpr int kRepeats = 3;
    const std::string address = next_address();

    const auto server_out = random_bytes(kSize, 0xE2000001ull);
    const auto client_out = random_bytes(kSize, 0xE2000002ull);

    run_two_party(
        address,
        [&](Channel& ch) {
            std::vector<u8> in(kSize);
            for (int r = 0; r < kRepeats; ++r) {
                ch.exchange(std::span<const u8>(server_out), std::span<u8>(in));
                EXPECT_EQ(in, client_out) << "server round " << r;
            }
        },
        [&](Channel& ch) {
            std::vector<u8> in(kSize);
            for (int r = 0; r < kRepeats; ++r) {
                ch.exchange(std::span<const u8>(client_out), std::span<u8>(in));
                EXPECT_EQ(in, server_out) << "client round " << r;
            }
        });
}

// ---------------------------------------------------------------------------
// SC3 — counters: one exchange(out=N, in=M) advances bytes_sent() by
// exactly N and sends_count() by exactly 1 on EACH party. N and M
// deliberately differ per party (server out=777/in=555, client out=555/
// in=777) so the test cannot pass by accident on a symmetric size.
// ---------------------------------------------------------------------------

TEST(NetExchange, SC3_CountersAdvanceByOutSizeAndOneSendPerCallOnEachParty) {
    const std::string address = next_address();
    constexpr std::size_t kServerOut = 777; // == client's `in` size
    constexpr std::size_t kClientOut = 555; // == server's `in` size

    const auto server_out = random_bytes(kServerOut, 0xE3000001ull);
    const auto client_out = random_bytes(kClientOut, 0xE3000002ull);

    u64 server_bytes_delta = 0, server_sends_delta = 0;
    u64 client_bytes_delta = 0, client_sends_delta = 0;

    run_two_party(
        address,
        [&](Channel& ch) {
            std::vector<u8> in(kClientOut);
            const u64 b0 = ch.bytes_sent();
            const u64 s0 = ch.sends_count();
            ch.exchange(std::span<const u8>(server_out), std::span<u8>(in));
            server_bytes_delta = ch.bytes_sent() - b0;
            server_sends_delta = ch.sends_count() - s0;
        },
        [&](Channel& ch) {
            std::vector<u8> in(kServerOut);
            const u64 b0 = ch.bytes_sent();
            const u64 s0 = ch.sends_count();
            ch.exchange(std::span<const u8>(client_out), std::span<u8>(in));
            client_bytes_delta = ch.bytes_sent() - b0;
            client_sends_delta = ch.sends_count() - s0;
        });

    EXPECT_EQ(server_bytes_delta, static_cast<u64>(kServerOut));
    EXPECT_EQ(server_sends_delta, 1u);
    EXPECT_EQ(client_bytes_delta, static_cast<u64>(kClientOut));
    EXPECT_EQ(client_sends_delta, 1u);
}

// ---------------------------------------------------------------------------
// SC5 — production-path stress: BeaverEngine::mul (src/gates/beaver.cpp,
// migrated to exchange() by this task) over N = 40,960 products, dealer
// triples (Task-13 precedent), real loopback TCP, under the same TIMEOUT
// discipline as SC2. Reconstructs x*y correctly on a sampled subset.
// ---------------------------------------------------------------------------

TEST(NetExchange, SC5_ProductionPathBeaverMulBatch40960ProductsUnderTimeout) {
    constexpr u64 kN = 40960; // |U|=2048 minors-L1 shape: 20 products/bucket * 2048 buckets
    const std::string address = next_address();

    std::mt19937_64 rng(0xE5000000ull);
    std::uniform_int_distribution<u64> dist(0, Fp::P - 1);

    std::vector<Triple> t0, t1;
    t0.reserve(kN);
    t1.reserve(kN);
    std::vector<Fp> x_plain(kN), y_plain(kN);
    for (u64 i = 0; i < kN; ++i) {
        Fp a(dist(rng)), b(dist(rng));
        Fp c = a.mul(b);
        Fp a0(dist(rng)), b0(dist(rng)), c0(dist(rng));
        t0.push_back(Triple{i, Share{a0}, Share{b0}, Share{c0}});
        t1.push_back(Triple{i, Share{a.sub(a0)}, Share{b.sub(b0)}, Share{c.sub(c0)}});
        x_plain[i] = Fp(dist(rng));
        y_plain[i] = Fp(dist(rng));
    }
    TriplePool pool0, pool1;
    pool0.refill(std::move(t0));
    pool1.refill(std::move(t1));

    std::vector<Share> x0(kN), x1(kN), y0(kN), y1(kN);
    for (u64 i = 0; i < kN; ++i) {
        Fp rx(dist(rng)), ry(dist(rng));
        x0[i] = Share{rx};
        x1[i] = Share{x_plain[i].sub(rx)};
        y0[i] = Share{ry};
        y1[i] = Share{y_plain[i].sub(ry)};
    }

    std::vector<Share> z0, z1;
    run_two_party(
        address,
        [&](Channel& ch) {
            BeaverEngine eng(Role::Receiver);
            z0 = eng.mul(x0, y0, pool0, ch);
        },
        [&](Channel& ch) {
            BeaverEngine eng(Role::Sender);
            z1 = eng.mul(x1, y1, pool1, ch);
        });

    ASSERT_EQ(z0.size(), kN);
    ASSERT_EQ(z1.size(), kN);

    // Sampled subset (deliberately not all 40,960 -- this test's point is
    // the migrated wire/timing path, not re-proving Beaver correctness).
    std::mt19937_64 sample_rng(0xE5A00000ull);
    std::uniform_int_distribution<u64> idx_dist(0, kN - 1);
    constexpr int kSamples = 200;
    for (int s = 0; s < kSamples; ++s) {
        const u64 i = idx_dist(sample_rng);
        const Fp z = z0[i].v.add(z1[i].v);
        const Fp want = x_plain[i].mul(y_plain[i]);
        EXPECT_EQ(z.v, want.v) << "product index " << i;
    }
}

// ---------------------------------------------------------------------------
// FC1 — a peer that performs a plain recv() of the WRONG size opposite our
// exchange() call hits a deterministic coproto error path (uncaught
// BadReceiveBufferSize -> std::terminate() -> abort()), not a hang. See
// file header comment for the traced path. "threadsafe" death-test style
// (kat_ztest.cpp's TVF3 precedent): this statement itself spawns threads,
// which is exactly what "threadsafe" (re-exec, not fork, of the whole
// binary for the death statement) is for.
// ---------------------------------------------------------------------------

TEST(NetExchange, FC1_PeerPlainRecvWrongSizeAbortsViaCoprotoErrorPath) {
    ::testing::GTEST_FLAG(death_test_style) = "threadsafe";
    const std::string address = next_address();
    constexpr std::size_t kSendSize = 64;
    constexpr std::size_t kWrongRecvSize = 32; // mismatched vs kSendSize

    EXPECT_DEATH(
        {
            std::thread bad_peer([&] {
                Channel ch(address, /*is_server=*/true);
                std::vector<u8> wrong(kWrongRecvSize);
                // Mismatched vs. the client's exchange() send below -> the
                // coproto badBufferSize error path fires here, throwing an
                // uncaught exception out of this thread's entry function.
                ch.recv(std::span<u8>(wrong));
            });
            auto client = connect_client_with_retry(address);
            std::vector<u8> out(kSendSize, 0xAB);
            std::vector<u8> in(kSendSize); // client's own recv half is moot:
                                            // the process aborts from
                                            // bad_peer's uncaught exception
                                            // before this could ever matter.
            if (client) client->exchange(std::span<const u8>(out), std::span<u8>(in));
            bad_peer.join();
        },
        "");
}

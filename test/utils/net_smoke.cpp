// task-4 brief, obligation (b): loopback smoke test for the real
// coproto/boost-backed Channel (net.hpp/net.cpp). Two threads act as
// server/client on localhost, round-trip a random 1 MiB buffer, and confirm
// Channel::bytes_sent() counts OUTGOING bytes only (send does not bump the
// receiver's counter; each side's counter equals exactly what that side
// sent).
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "sympsica/utils/net.hpp"

using namespace sympsica;

namespace {

constexpr std::size_t kBufSize = 1u << 20; // 1 MiB
const std::string kAddress = "127.0.0.1:41231";

// The server-side Channel's constructor blocks inside accept() until a peer
// connects; the client-side constructor's connect() can race the server's
// bind()+listen() (both happen on independent threads). Retry the client
// connect briefly rather than relying on a fixed sleep before it.
std::unique_ptr<Channel> connect_client_with_retry() {
    std::unique_ptr<Channel> ch;
    for (int attempt = 0; attempt < 200 && !ch; ++attempt) {
        try {
            ch = std::make_unique<Channel>(kAddress, /*is_server=*/false);
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    return ch;
}

} // namespace

TEST(NetSmoke, LoopbackRoundTripAndByteCount) {
    std::mt19937_64 rng(0xC0FFEE);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::vector<u8> sent(kBufSize);
    for (auto& b : sent) b = static_cast<u8>(byte_dist(rng));

    std::vector<u8> server_received(kBufSize);
    u64 server_bytes_sent = 0;

    std::thread server([&] {
        Channel ch(kAddress, /*is_server=*/true);
        ch.recv(std::span<u8>(server_received));
        ByteMeter meter(ch);
        ch.send(std::span<const u8>(server_received)); // echo it straight back
        server_bytes_sent = meter.bytes();
    });

    auto client = connect_client_with_retry();
    ASSERT_NE(client, nullptr) << "client failed to connect to loopback server";

    ByteMeter client_meter(*client);
    client->send(std::span<const u8>(sent));
    std::vector<u8> echoed(kBufSize);
    client->recv(std::span<u8>(echoed));

    server.join();

    EXPECT_EQ(server_received, sent);
    EXPECT_EQ(echoed, sent);
    // bytes_sent() counts OUTGOING bytes only: each side sent exactly one
    // kBufSize buffer, so each side's delta is kBufSize regardless of how
    // much it received.
    EXPECT_EQ(client_meter.bytes(), static_cast<u64>(kBufSize));
    EXPECT_EQ(server_bytes_sent, static_cast<u64>(kBufSize));
    EXPECT_EQ(client->bytes_sent(), static_cast<u64>(kBufSize));
}

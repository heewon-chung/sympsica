#include "sympsica/utils/net.hpp"

#include "coproto/Socket/AsioSocket.h"
#include "macoro/sync_wait.h"

#include "sympsica/utils/common.hpp"

// Real implementation (task-4 brief, obligation (b)) of the coproto TCP
// wrapper Phase 1 stubbed out (net.hpp's Phase-1 history + task-2-report.md's
// "Concerns" recorded why: coproto's TCP backend (coproto/Socket/AsioSocket.h)
// hard-includes <boost/asio.hpp>, which Phase 0/1 had not vendored/pinned).
// PINS.md's "Boost" section records the pin that unblocks this.
//
// coproto's natural interface is coroutine-based (macoro::task<>/co_await);
// per obligation (b)'s explicit allowance ("you may wrap synchronously via
// coproto::sync_wait ... document what you did"), Channel wraps it
// synchronously/blockingly with macoro::sync_wait around each coproto::Socket
// operation:
//   - construction: coproto::asioConnect(address, is_server) — the 2-arg
//     overload, which runs its own internally-owned boost::asio::io_context
//     on a background thread it manages itself
//     (coproto::detail::global_asio_io_context), so Channel does not need to
//     own/pump an io_context/thread of its own.
//   - send(data):   macoro::sync_wait(sock.send(data)) followed by
//     macoro::sync_wait(sock.flush()). send() alone only guarantees the data
//     has been scheduled/buffered internally (see coproto::Socket::send()'s
//     own doc comment, coproto/Socket/Socket.h); flush() is required to
//     guarantee the bytes have actually been written to the socket before
//     send() returns, which bytes_sent()'s contract (task-2 brief, Ruling 4:
//     "the sanity cross-check against the external netns byte counter")
//     depends on.
//   - recv(data):   macoro::sync_wait(sock.recv(data)).
//
// bytes_sent() counts OUTGOING bytes only, incremented by the requested
// send() size: coproto::Socket::send()/recv() here move raw byte spans with
// no additional wire framing coproto itself adds on top, so "bytes
// requested" and "bytes actually placed on the wire" coincide.

namespace sympsica {

struct Channel::Impl {
    coproto::Socket sock;
};

Channel::Channel(const std::string& address, bool is_server)
    : impl_(std::make_unique<Impl>()) {
    impl_->sock = coproto::asioConnect(address, is_server);
}

Channel::~Channel() = default;

void Channel::send(std::span<const u8> data) {
    macoro::sync_wait(impl_->sock.send(data));
    macoro::sync_wait(impl_->sock.flush());
    bytes_sent_ += data.size();
}

void Channel::recv(std::span<u8> data) {
    macoro::sync_wait(impl_->sock.recv(data));
}

u64 Channel::bytes_sent() const {
    return bytes_sent_;
}

} // namespace sympsica

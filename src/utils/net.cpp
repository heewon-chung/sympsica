#include "sympsica/utils/net.hpp"

#include <atomic>

#include "coproto/Socket/AsioSocket.h"
// macoro/sync_wait.h and macoro/when_all.h use std::source_location/
// basic_traceable but do not include macoro/trace.h themselves -- same gap
// test/gates/kat_ztest.cpp's header comment documents and works around by
// including macoro/trace.h first.
#include "macoro/trace.h"
#include "macoro/sync_wait.h"
#include "macoro/when_all.h"

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
//   - exchange(out, in) (task-15 brief -- deadlock-safe symmetric round,
//     see net.hpp's doc comment for the hazard this retires): composes the
//     send and recv AWAITABLES with macoro::when_all_ready(...) under ONE
//     outer sync_wait, instead of two separate sync_wait calls run back to
//     back. This is the difference that matters: when_all_ready's
//     implementation (macoro/detail/when_all_awaitable.h,
//     when_all_ready_awaitable::try_await -> start_tasks) calls .start() on
//     BOTH the send sub-task and the recv sub-task SYNCHRONOUSLY, before the
//     combined awaitable ever suspends -- i.e. sock.send(out)'s
//     await_suspend and sock.recv(in)'s await_suspend both run (posting
//     both operations onto coproto's SockScheduler) before sync_wait blocks
//     the calling thread on anything. Concretely (verified against
//     coproto/Socket/SocketScheduler.h): the recv operation gets registered
//     into the socket's persistent, always-running receiveDataTask
//     coroutine (started once at Channel construction, see
//     SockScheduler::init) BEFORE this call blocks, so that background task
//     starts actually draining the kernel socket's incoming bytes
//     concurrently with the outstanding send -- exactly the fix for the
//     hazard where the kernel recv buffer never gets drained because
//     neither party has posted a recv() yet. Two sequential sync_wait calls
//     (send() then recv(), the pre-task-15 pattern) cannot provide this:
//     the second sync_wait (recv) is not even entered, let alone posted to
//     the scheduler, until the first (send) has fully returned -- which is
//     precisely the ordering that deadlocks when both parties fill their
//     kernel send buffers before either posts a recv.
//
// bytes_sent() counts OUTGOING bytes only, incremented by the requested
// send() size: coproto::Socket::send()/recv() here move raw byte spans with
// no additional wire framing coproto itself adds on top, so "bytes
// requested" and "bytes actually placed on the wire" coincide.

namespace sympsica {

namespace {
// task-24-brief.md SC2/R6-CGA-RUNTIME: process-wide, always-compiled
// Channel-construction counter. std::atomic (not a plain u64) because
// test/utils/net_smoke.cpp and test/utils/net_exchange.cpp both construct
// Channels from two DIFFERENT threads within the same process (an
// in-process loopback harness), so concurrent increments are a real
// possibility here, unlike most other counters in this codebase.
std::atomic<u64> g_channel_construction_count{0};
} // namespace

struct Channel::Impl {
    coproto::Socket sock;
};

Channel::Channel(const std::string& address, bool is_server)
    : impl_(std::make_unique<Impl>()) {
    // Incremented FIRST, before coproto::asioConnect() runs -- so a
    // construction attempt that throws (e.g. a client's connect() racing an
    // as-yet-unstarted listener) is still counted as a real Channel
    // construction attempt, matching the SC2 grep-guard's own static
    // reasoning ("Channel(...) was invoked" is the fact being counted, not
    // "Channel(...) returned successfully").
    g_channel_construction_count.fetch_add(1, std::memory_order_relaxed);
    impl_->sock = coproto::asioConnect(address, is_server);
}

u64 Channel::construction_count() {
    return g_channel_construction_count.load(std::memory_order_relaxed);
}

Channel::~Channel() = default;

void Channel::send(std::span<const u8> data) {
    macoro::sync_wait(impl_->sock.send(data));
    macoro::sync_wait(impl_->sock.flush());
    bytes_sent_ += data.size();
    ++sends_count_;
}

void Channel::recv(std::span<u8> data) {
    macoro::sync_wait(impl_->sock.recv(data));
}

void Channel::exchange(std::span<const u8> out, std::span<u8> in) {
    // ONE sync_wait over the combined when_all_ready awaitable -- see this
    // file's top comment for why this (and not two sequential sync_waits)
    // is what makes send and recv run concurrently instead of one blocking
    // the other.
    macoro::sync_wait(macoro::when_all_ready(impl_->sock.send(out), impl_->sock.recv(in)));
    macoro::sync_wait(impl_->sock.flush());
    bytes_sent_ += out.size();
    ++sends_count_;
}

u64 Channel::bytes_sent() const {
    return bytes_sent_;
}

u64 Channel::sends_count() const {
    return sends_count_;
}

coproto::Socket& Channel::socket() {
    return impl_->sock;
}

} // namespace sympsica

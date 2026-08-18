#ifndef SYMPSICA_UTILS_NET_HPP
#define SYMPSICA_UTILS_NET_HPP

#include <memory>
#include <span>
#include <string>

#include "sympsica/utils/field.hpp"

// Forward declaration only (task-5-report.md Concern 1 / task-8 brief
// requirement 1): coproto::Socket is a plain, non-template class (see
// coproto/Socket/Socket.h), so this adds no include weight to consumers of
// Channel — the coproto/boost/macoro headers stay pimpl'd into net.cpp.
namespace coproto {
class Socket;
} // namespace coproto

namespace sympsica {

// Channel — coproto TCP wrapper (task-2 brief, Ruling 4; wired for real by
// task-4, obligation (b), once the boost pin landed — see PINS.md). send()/
// recv() are blocking; bytes_sent() counts OUTGOING bytes only (design doc:
// Channel's counter is the sanity cross-check against the external netns
// byte counter, FT11).
//
// Backed by coproto::asioConnect(...)'s AsioSocket, driven synchronously via
// macoro::sync_wait — see net.cpp's top comment for the full rationale. The
// coproto/boost/macoro headers that requires are kept out of this header
// (pimpl'd into net.cpp) so consumers of Channel do not have to pay for
// them, mirroring coeff_ctx.hpp's rationale for splitting libOTe's
// CoeffCtx.h out of field.hpp.
class Channel {
public:
    // Connects as server (listens) or client (dials), per `is_server`.
    // `address` is coproto::asioConnect's own "host:port" address format.
    Channel(const std::string& address, bool is_server);
    ~Channel();

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    // Blocking send/recv of exactly data.size() bytes.
    void send(std::span<const u8> data);
    void recv(std::span<u8> data);

    u64 bytes_sent() const;

    // sends_count() — task-14-brief.md R-ROUNDS (additive accessor, same
    // precedent as bytes_sent()/socket() above): counts Channel::send()
    // CALLS, not bytes and not recv() calls. This project's protocols are
    // synchronous send-then-recv, so one communication round is exactly one
    // send() call per party; SD-3/TV-F13 (test/gates/kat_symdiff.cpp) use
    // the before/after delta of this counter to pin SymDiffEvaluator's
    // batched round count. Same known bypass as bytes_sent(): traffic that
    // goes through socket() directly (the ztgate pipeline/DKG code) does
    // NOT increment this counter.
    u64 sends_count() const;

    // Bridge to the pipeline layer (task-8 brief, requirement 1 / controller
    // ruling): exposes the pimpl'd coproto::Socket by reference so a
    // libOTe/coproto protocol (e.g. test/integration/ztgate_pipeline.hpp's
    // `coproto::Socket&`-taking functions) can drive this Channel's
    // connection directly, without Channel re-exposing a coroutine API of
    // its own. IMPORTANT — bytes_sent()/ByteMeter do NOT observe traffic
    // sent this way: bytes_sent_ is only incremented inside Channel::send(),
    // and code that writes through socket() bypasses Channel::send()
    // entirely. Communication measured over this path must instead come from
    // coproto::Socket's own counters (bytesSent()/bytesReceived(), see
    // coproto/Socket/Socket.h) or from a byte count taken at the socket
    // layer by the caller.
    coproto::Socket& socket();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    u64 bytes_sent_ = 0;
    u64 sends_count_ = 0;
};

// ByteMeter — RAII scope measuring outgoing bytes sent on a Channel for one
// protocol phase (online/offline split): reads Channel::bytes_sent() at
// construction, reports the delta since then via bytes().
class ByteMeter {
public:
    explicit ByteMeter(const Channel& ch) : ch_(ch), start_(ch.bytes_sent()) {}

    u64 bytes() const { return ch_.bytes_sent() - start_; }

private:
    const Channel& ch_;
    u64 start_;
};

} // namespace sympsica

#endif // SYMPSICA_UTILS_NET_HPP

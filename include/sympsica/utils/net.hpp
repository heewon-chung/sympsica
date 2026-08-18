#ifndef SYMPSICA_UTILS_NET_HPP
#define SYMPSICA_UTILS_NET_HPP

#include <memory>
#include <span>
#include <string>

#include "sympsica/utils/field.hpp"

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    u64 bytes_sent_ = 0;
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

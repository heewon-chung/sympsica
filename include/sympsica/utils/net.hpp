#ifndef SYMPSICA_UTILS_NET_HPP
#define SYMPSICA_UTILS_NET_HPP

#include <span>
#include <string>

#include "sympsica/utils/field.hpp"

namespace sympsica {

// Channel — coproto TCP wrapper (task-2 brief, Ruling 4). send()/recv() are
// blocking; bytes_sent() counts OUTGOING bytes only (design doc: Channel's
// counter is the sanity cross-check against the external netns byte counter,
// FT11).
//
// STUB (task-2 brief, Ruling 4's stop clause): the vendored coproto Socket's
// TCP backend (coproto/Socket/AsioSocket.h) hard-includes <boost/asio.hpp>
// and <boost/asio/ssl.hpp>, neither vendored/pinned by Phase 0 nor added by
// this task (see task-2-report.md, "Concerns", for the full finding). Every
// method below aborts via SYMPSICA_REQUIRE(false, "net: wired at Phase 2")
// until that dependency is wired in.
class Channel {
public:
    // Connects as server (listens) or client (dials), per `is_server`.
    Channel(const std::string& address, bool is_server);
    ~Channel();

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    void send(std::span<const u8> data);
    void recv(std::span<u8> data);

    u64 bytes_sent() const;

private:
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

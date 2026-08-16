#include "sympsica/utils/net.hpp"

#include "sympsica/utils/common.hpp"

// STUB — see net.hpp's top comment and task-2-report.md's "Concerns" for the
// full finding: coproto's TCP socket backend requires boost::asio (+ TLS),
// which is not a sympsica dependency as of this task. Wiring this for real is
// Phase 2's job (task-2 brief, Ruling 4).

namespace sympsica {

Channel::Channel(const std::string& /*address*/, bool /*is_server*/) {
    SYMPSICA_REQUIRE(false, "net: wired at Phase 2");
}

Channel::~Channel() = default;

void Channel::send(std::span<const u8> /*data*/) {
    SYMPSICA_REQUIRE(false, "net: wired at Phase 2");
}

void Channel::recv(std::span<u8> /*data*/) {
    SYMPSICA_REQUIRE(false, "net: wired at Phase 2");
}

u64 Channel::bytes_sent() const {
    return bytes_sent_;
}

} // namespace sympsica

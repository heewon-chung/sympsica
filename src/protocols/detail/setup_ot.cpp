#include "sympsica/protocols/detail/setup_ot.hpp"

#include <algorithm>

#include "sympsica/protocols/setup.hpp"

namespace sympsica::detail {

macoro::task<> run_base_ots(Role role, SetupOtState& st, coproto::Socket& sock) {
    // Role-opposite call order (mirrors ztgate::generate_ot_pool): avoids
    // interleaving the two base-OT rounds on one shared socket.
    if (role == Role::Receiver) {
        co_await st.sender.genBaseOts(st.ot_prng, sock);
        PkOpCounter::increment();
        co_await st.recver.genBaseOts(st.ot_prng, sock);
        PkOpCounter::increment();
    } else {
        co_await st.recver.genBaseOts(st.ot_prng, sock);
        PkOpCounter::increment();
        co_await st.sender.genBaseOts(st.ot_prng, sock);
        PkOpCounter::increment();
    }
}

macoro::task<> fill_ot_pool(Role role, oc::u64 n, SetupOtState& st, coproto::Socket& sock,
                            ztgate::OtPool& pool) {
    // Same floor as ztgate::generate_ot_pool: below this the LPN
    // parameterisation is not worth configuring.
    constexpr oc::u64 kMinSilentOts = 128;
    auto req = std::max<oc::u64>(n, kMinSilentOts);

    st.sender.configure(req);
    st.recver.configure(req);

    pool.send.resize(req);
    pool.recv.resize(req);
    pool.choices.resize(req);

    if (role == Role::Receiver) {
        co_await st.sender.silentSend(pool.send, st.ot_prng, sock);
        co_await st.recver.silentReceive(pool.choices, pool.recv, st.ot_prng, sock,
                                          oc::OTType::Random);
    } else {
        co_await st.recver.silentReceive(pool.choices, pool.recv, st.ot_prng, sock,
                                          oc::OTType::Random);
        co_await st.sender.silentSend(pool.send, st.ot_prng, sock);
    }
}

} // namespace sympsica::detail

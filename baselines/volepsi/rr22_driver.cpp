// baselines/volepsi/rr22_driver.cpp -- W8.4: RR22 plain PSI over REAL coproto
// TCP between two processes; row labeled "RR22-CA(convention)": volePSI
// (archived @ ec76012) has NO cardinality output, so the CA row is |PSI| counted
// receiver-side under BMS+24's own plain-PSI-as-CA convention footnote (emitted
// into the record notes by run.sh). Single thread (numThreads=1), default
// MultType, semi-honest. Emits NO PHASE markers (phase-8-plan.md C: run.sh is
// the sole marker owner); prints RESULT lines on stdout only.
#include "volePSI/RsPsi.h"
#include "coproto/Socket/AsioSocket.h"
#include "macoro/sync_wait.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace oc;

int main(int argc, char** argv) {
    std::string role, address, gate; u64 n = 0, inter = 0, seed = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        if (k == "--role") role = argv[i + 1];
        else if (k == "--address") address = argv[i + 1];
        else if (k == "--n") n = std::stoull(argv[i + 1]);
        else if (k == "--inter") inter = std::stoull(argv[i + 1]);
        else if (k == "--seed") seed = std::stoull(argv[i + 1]);
        else if (k == "--start-gate") gate = argv[i + 1];
        else { std::fprintf(stderr, "unknown flag %s\n", k.c_str()); return 3; }
    }
    if ((role != "sender" && role != "receiver") || address.empty() || n == 0 || inter > n) {
        std::fprintf(stderr, "usage: rr22_driver --role sender|receiver --address ip:port --n N --inter I --seed S [--start-gate PATH]\n");
        return 3;
    }
    if (!gate.empty()) {   // A5 cooperative start gate: block until the sampler has taken our first sample
        int waited_ms = 0;
        while (!std::filesystem::exists(gate)) {
            if (waited_ms >= 30000) { std::fprintf(stderr, "start gate %s not created within 30 s\n", gate.c_str()); return 4; }
            std::this_thread::sleep_for(std::chrono::milliseconds(50)); waited_ms += 50;
        }
    }
    // Deterministic disjoint tagging: shared items tag 0 (identical on both
    // sides), role-unique items tag 1 (sender) / 2 (receiver) => |A n B| == inter.
    std::vector<block> items(n);
    u64 tag = (role == "sender") ? 1 : 2;
    for (u64 i = 0; i < inter; ++i) items[i] = block(0, i);
    for (u64 i = inter; i < n; ++i) items[i] = block(tag, i);
    auto chl = coproto::asioConnect(address, /*server=*/role == "receiver");
    if (role == "receiver") {
        volePSI::RsPsiReceiver recv;
        recv.init(n, n, 40, block(seed, seed), /*malicious=*/false, /*numThreads=*/1);
        macoro::sync_wait(recv.run(items, chl));
        macoro::sync_wait(chl.flush());
        std::printf("RESULT:count=%llu\n", (unsigned long long)recv.mIntersection.size());
    } else {
        volePSI::RsPsiSender send;
        send.init(n, n, 40, block(seed, seed ^ 1), false, 1);
        macoro::sync_wait(send.run(items, chl));
        macoro::sync_wait(chl.flush());
    }
    macoro::sync_wait(chl.close());
    return 0;
}

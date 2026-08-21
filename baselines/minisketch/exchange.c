/* baselines/minisketch/exchange.c -- W8.5: two-party minisketch symmetric-
 * difference exchange over TCP (bitcoin-core/minisketch @ 4a179c6). INADMISSIBLE-
 * LABELED reference row (non-private sketch, not a PSI protocol; label emitted by
 * run.sh). b=64, impl=0 (default), capacity=u. Alice: create+add+serialize+send
 * (8*capacity bytes); Bob: merge+decode -> |A xor B|. For equal-size sets,
 * |A n B| = (n_a + n_b - |A xor B|) / 2 (run.sh derives ca_from_symdiff).
 * Deterministic tagged ids: (tag<<32)|(i+1), tag 0 shared / 1 alice-only /
 * 2 bob-only. Emits NO PHASE markers (run.sh is the sole marker owner). */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <minisketch.h>

static int die(const char* m) { perror(m); return 1; }

int main(int argc, char** argv) {
    const char* role = NULL; const char* addr = NULL; const char* gate = NULL;
    int port = 0; uint64_t n = 0, inter = 0, cap = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--start-gate")) gate = argv[i + 1];
        else if (!strcmp(argv[i], "--role")) role = argv[i + 1];
        else if (!strcmp(argv[i], "--address")) addr = argv[i + 1];
        else if (!strcmp(argv[i], "--port")) port = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--n")) n = strtoull(argv[i + 1], 0, 10);
        else if (!strcmp(argv[i], "--inter")) inter = strtoull(argv[i + 1], 0, 10);
        else if (!strcmp(argv[i], "--capacity")) cap = strtoull(argv[i + 1], 0, 10);
        else { fprintf(stderr, "unknown flag %s\n", argv[i]); return 3; }
    }
    uint64_t expected_symdiff = 2 * (n - inter);
    if (!role || !addr || !port || !n || inter > n || !cap || expected_symdiff > cap) {
        fprintf(stderr, "usage: exchange --role alice|bob --address A --port P --n N --inter I --capacity C (2*(N-I) <= C) [--start-gate PATH]\n");
        return 3;
    }
    if (gate) {   /* A5 cooperative start gate: block until the sampler has taken our first sample */
        int waited_ms = 0;
        while (access(gate, F_OK) != 0) {
            if (waited_ms >= 30000) { fprintf(stderr, "start gate %s not created within 30 s\n", gate); return 4; }
            usleep(50000); waited_ms += 50;
        }
    }
    minisketch* sk = minisketch_create(64, 0, cap);
    if (!sk) { fprintf(stderr, "minisketch_create failed\n"); return 1; }
    uint64_t tag = strcmp(role, "alice") ? 2u : 1u;
    for (uint64_t i = 0; i < inter; ++i) minisketch_add_uint64(sk, (0ull << 32) | (i + 1));
    for (uint64_t i = inter; i < n; ++i) minisketch_add_uint64(sk, (tag << 32) | (i + 1));
    size_t ser = minisketch_serialized_size(sk);   /* == 8*cap at bits=64 */
    unsigned char* buf = malloc(ser);
    int fd;
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons(port);
    inet_pton(AF_INET, addr, &sa.sin_addr);
    if (!strcmp(role, "bob")) {                    /* bob = receiver = listener */
        int ls = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(ls, (struct sockaddr*)&sa, sizeof sa) || listen(ls, 1)) return die("bind/listen");
        fd = accept(ls, NULL, NULL);
        if (fd < 0) return die("accept");
        size_t got = 0;
        while (got < ser) { ssize_t r = read(fd, buf + got, ser - got); if (r <= 0) return die("read"); got += r; }
        minisketch* other = minisketch_create(64, 0, cap);
        minisketch_deserialize(other, buf);
        minisketch_merge(sk, other);               /* sk := symmetric-difference sketch */
        uint64_t* out = malloc(8 * cap);
        ssize_t k = minisketch_decode(sk, cap, out);
        if (k < 0) { fprintf(stderr, "RESULT:decode=FAILED\n"); return 5; }
        printf("RESULT:symdiff=%zd\n", k);
        printf("RESULT:bytes=%zu\n", ser);
        minisketch_destroy(other); free(out);
    } else {                                       /* alice = connector = sender */
        fd = socket(AF_INET, SOCK_STREAM, 0);
        for (int tries = 0; connect(fd, (struct sockaddr*)&sa, sizeof sa); ++tries) {
            if (tries > 50) return die("connect");
            usleep(200000); close(fd); fd = socket(AF_INET, SOCK_STREAM, 0);
        }
        minisketch_serialize(sk, buf);
        size_t sent = 0;
        while (sent < ser) { ssize_t w = write(fd, buf + sent, ser - sent); if (w <= 0) return die("write"); sent += w; }
    }
    close(fd); minisketch_destroy(sk); free(buf);
    return 0;
}

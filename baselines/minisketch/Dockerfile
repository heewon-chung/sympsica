# baselines/minisketch/Dockerfile -- W8.5 build substrate (bitcoin-core/minisketch
# @ 4a179c6; inadmissible-labeled reference row). Executed BY IMAGE ID.
FROM ubuntu:22.04
ARG DEBIAN_FRONTEND=noninteractive
LABEL sympsica.source_sha=4a179c61e3cbe3ac2b3c027764ce8eb5183155e1
RUN apt-get update && apt-get install -y build-essential cmake git
RUN git clone https://github.com/bitcoin-core/minisketch /minisketch \
 && cd /minisketch && git checkout 4a179c61e3cbe3ac2b3c027764ce8eb5183155e1 \
 && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4 \
 && LIB=$(find build -name 'libminisketch.a' | head -n1) && test -n "$LIB" \
 && cp "$LIB" /usr/local/lib/ && cp include/minisketch.h /usr/local/include/
COPY exchange.c /driver/exchange.c
RUN gcc -O2 -o /driver/exchange /driver/exchange.c -I/usr/local/include -L/usr/local/lib -lminisketch -lstdc++
CMD ["/bin/bash"]

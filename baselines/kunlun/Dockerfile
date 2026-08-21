# baselines/kunlun/Dockerfile -- CZZ+24 mqRPMT PSI-CA (Kunlun @ 395cf8e).
# STUB under controller ruling R8-KUNLUN (2026-08-21): authored at Task 32, NOT
# built, NOT smoked, upstream NOT probed -- UNVALIDATED BY CONSTRUCTION. CZZ+24
# stays a derived-labeled row. Build risk: hand-built OpenSSL 3.0 at Kunlun's
# hardcoded /usr/local/openssl prefix, curve25519.c line-211 `static` removal.
FROM ubuntu:20.04
ARG DEBIAN_FRONTEND=noninteractive
ARG LOG_N=20
LABEL sympsica.source_sha=395cf8ec48e73d9ab6906b51a85e2dbb41929b92
RUN apt-get update && apt-get install -y build-essential cmake git wget perl libomp-dev
RUN wget -q https://www.openssl.org/source/openssl-3.0.13.tar.gz -O /tmp/openssl.tar.gz \
 && sha256sum /tmp/openssl.tar.gz \
 && tar -xzf /tmp/openssl.tar.gz -C /tmp && cd /tmp/openssl-3.0.13 \
 && sed -n '211p' crypto/ec/curve25519.c | grep -q '^static ' \
 && sed -i '211s/^static //' crypto/ec/curve25519.c \
 && ./Configure no-shared enable-ec_nistp_64_gcc_128 no-ssl2 no-ssl3 no-comp \
      --prefix=/usr/local/openssl && make -j4 depend && make -j4 && make install_sw
RUN git clone https://github.com/yuchen1024/Kunlun /kunlun \
 && cd /kunlun && git checkout 395cf8ec48e73d9ab6906b51a85e2dbb41929b92 \
 && grep -q 'inline const size_t NUMBER_OF_THREADS = 8;' include/global.hpp \
 && sed -i 's/^inline const size_t NUMBER_OF_THREADS = 8;/inline const size_t NUMBER_OF_THREADS = 1;/' include/global.hpp \
 && grep -q 'size_t LOG_SENDER_ITEM_NUM = 20;' test/test_mqrpmt_psi_card.cpp \
 && sed -i "s/size_t LOG_SENDER_ITEM_NUM = 20;/size_t LOG_SENDER_ITEM_NUM = ${LOG_N};/" test/test_mqrpmt_psi_card.cpp \
 && grep -q 'size_t LOG_RECEIVER_ITEM_NUM = 20;' test/test_mqrpmt_psi_card.cpp \
 && sed -i "s/size_t LOG_RECEIVER_ITEM_NUM = 20;/size_t LOG_RECEIVER_ITEM_NUM = ${LOG_N};/" test/test_mqrpmt_psi_card.cpp
RUN cd /kunlun && mkdir build && cd build && cmake .. && make -j4 test_mqrpmt_psi_card
CMD ["/bin/bash"]

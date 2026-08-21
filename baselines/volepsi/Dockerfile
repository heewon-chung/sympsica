# baselines/volepsi/Dockerfile -- W8.4 build substrate (volePSI @ ec76012, archived
# 2025-09-03; RR22-CA convention row). Executed BY IMAGE ID (sha256:...) recorded
# in PINS.md at build time; the tag is an informational alias only.
#
# FINDING (Task 33, first real build of this substrate): the pinned apt-get
# package set alone is insufficient -- volePSI's cryptoTools submodule
# transitively fetches+builds libsodium via its own `autogen.sh -s`, which
# hard-fails ("libtool is required, but wasn't found on this system") without
# the autotools toolchain. `libtool autoconf automake pkg-config` added below;
# this is a build-substrate gap in our own Dockerfile, not a change to the
# pinned volePSI source (still checked out at the exact SHA below).
FROM ubuntu:22.04
ARG DEBIAN_FRONTEND=noninteractive
LABEL sympsica.source_sha=ec76012ed516e25d3f460af9b8680e1140a5d491
RUN apt-get update && apt-get install -y build-essential cmake git python3 libssl-dev libtool autoconf automake pkg-config
RUN git clone https://github.com/Visa-Research/volepsi.git /volepsi \
 && cd /volepsi && git checkout ec76012ed516e25d3f460af9b8680e1140a5d491 \
 && python3 build.py -DVOLE_PSI_ENABLE_BOOST=ON -DCMAKE_BUILD_TYPE=Release \
 && python3 build.py --install=/usr/local
COPY rr22_driver.cpp CMakeLists.txt /driver/
RUN cd /driver && cmake -B build -DVOLEPSI_HINT=/usr/local && cmake --build build
CMD ["/bin/bash"]

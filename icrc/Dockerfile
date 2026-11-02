# syntax=docker/dockerfile:1
# NGC release tag; the package-level SDK build is checked below.
ARG DOCA_IMAGE=nvcr.io/nvidia/doca/doca:devel-3.5.0
FROM ${DOCA_IMAGE}

USER root
ARG EXPECTED_DOCA_VERSION=3.5.0098
# Verified Arm SDK metadata locations; DPDK is outside the default search path.
ENV PKG_CONFIG_PATH=/opt/mellanox/doca/lib/aarch64-linux-gnu/pkgconfig:/opt/mellanox/dpdk/lib/aarch64-linux-gnu/pkgconfig
WORKDIR /opt/node-b

# The DOCA development image supplies the compiler, Meson/Ninja, pkg-config and
# SDK libraries. Deliberately do not run apt-get: the DPU is shared.

RUN pkg-config --modversion doca-common doca-flow doca-eth doca-dpdk-bridge libdpdk \
    && pkg-config --exact-version="${EXPECTED_DOCA_VERSION}" doca-common

COPY meson.build meson_options.txt ./
COPY include/ include/
COPY src/ src/
COPY tests/ tests/
COPY tools/ tools/
COPY config/groups.example.json config/groups.example.json
COPY config/groups.ipv6.example.json config/groups.ipv6.example.json

# The test suite is portable: no EAL, devices or traffic are accessed at build.
# Keep the build tree at its original path to retain Meson's library RUNPATH.
RUN meson setup build -Dhardware=true --buildtype=debugoptimized \
    && meson compile -C build \
    && meson test -C build --print-errorlogs \
    && ldd build/node_b > build/runtime-libraries.txt \
    && ! grep -q 'not found' build/runtime-libraries.txt \
    && grep -q 'libdoca_flow' build/runtime-libraries.txt \
    && grep -q 'libdoca_eth' build/runtime-libraries.txt

STOPSIGNAL SIGTERM
ENTRYPOINT ["/opt/node-b/build/node_b"]

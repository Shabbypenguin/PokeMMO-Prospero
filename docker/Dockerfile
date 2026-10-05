# pokemmo-ps5 toolchain image.
#
# Everything is pinned (commit or SHA-256) so a build today and a build in six
# months produce the same toolchain. Bump the ARGs deliberately, never silently.
#
# Contents:
#   /opt/ps5/boilerplate        ps5-native-app-boilerplate (FSELF writer, libc.prx shim, packaging)
#   /opt/ps5/payload-sdk        public PS5 payload SDK (inside the boilerplate's .deps, symlinked)
#   /opt/ps5/opengl             ps5-opengl prebuilt SDK (OpenGL 4.6 / EGL, static)
#   /opt/ps5/opengl-src         ps5-opengl source snapshot matching that SDK (native-app glue)

FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

# Optional extra CA certificates (corporate/proxy setups). Empty by default.
COPY docker/extra-ca/ /usr/local/share/ca-certificates/extra/

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl wget git unzip zip xz-utils file \
        make ninja-build ccache pkg-config \
        python3 python3-venv python3-pip \
        clang-18 lld-18 llvm-18 libclang-rt-18-dev libc++-18-dev libc++abi-18-dev \
        binutils gdb-multiarch \
    && update-ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && ln -sf /usr/bin/clang-18 /usr/local/bin/clang \
    && ln -sf /usr/bin/clang++-18 /usr/local/bin/clang++ \
    && ln -sf /usr/bin/ld.lld-18 /usr/local/bin/ld.lld \
    && ln -sf /usr/bin/llvm-ar-18 /usr/local/bin/llvm-ar \
    && ln -sf /usr/bin/llvm-ranlib-18 /usr/local/bin/llvm-ranlib \
    && ln -sf /usr/bin/llvm-objdump-18 /usr/local/bin/llvm-objdump \
    && ln -sf /usr/bin/llvm-readelf-18 /usr/local/bin/llvm-readelf

# ---- native-app boilerplate + payload SDK v0.42 + static zlib ---------------------------------
ARG BOILERPLATE_URL=https://github.com/blackbearreloaded/ps5-native-app-boilerplate.git
ARG BOILERPLATE_COMMIT=4f531c4b517f80bcb6b1267135848168d2250047
# zlib is pre-seeded from its GitHub release (byte-identical to zlib.net's, which is
# often unreachable); the boilerplate script still verifies the SHA-256 itself.
RUN git clone --quiet "$BOILERPLATE_URL" /opt/ps5/boilerplate \
    && git -C /opt/ps5/boilerplate checkout --quiet "$BOILERPLATE_COMMIT" \
    && mkdir -p /opt/ps5/boilerplate/.deps/native/zlib \
    && curl -fsSL -o /opt/ps5/boilerplate/.deps/native/zlib/zlib-1.3.2.tar.gz \
        https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz \
    && bash /opt/ps5/boilerplate/tools/setup-native-dependencies.sh \
    && ln -s /opt/ps5/boilerplate/.deps/native/ps5-payload-sdk /opt/ps5/payload-sdk

# The boilerplate generates its clean-room libc.prx runtime on first use; do it
# once here so every container starts with a ready runtime.
RUN make -C /opt/ps5/boilerplate --no-print-directory libc USE_CCACHE=0

# ---- ps5-opengl prebuilt SDK -------------------------------------------------------------------
ARG PS5_OPENGL_VERSION=1.0.1
ARG PS5_OPENGL_SHA256=aaa2e8957f55e1fc0b654dcb35a36e585f7e635d63952da40a28be7f64fde741
RUN cd /tmp \
    && curl -fsSLO "https://github.com/blackbearreloaded/ps5-opengl/releases/download/v${PS5_OPENGL_VERSION}/ps5-opengl-sdk-${PS5_OPENGL_VERSION}.tar.gz" \
    && echo "${PS5_OPENGL_SHA256}  ps5-opengl-sdk-${PS5_OPENGL_VERSION}.tar.gz" | sha256sum --check --strict \
    && tar -xzf "ps5-opengl-sdk-${PS5_OPENGL_VERSION}.tar.gz" \
    && mv "ps5-opengl-sdk-${PS5_OPENGL_VERSION}" /opt/ps5/opengl-bundle \
    && ln -s /opt/ps5/opengl-bundle/sdk /opt/ps5/opengl \
    && (cd /opt/ps5/opengl && sha256sum --check --strict --quiet manifest.sha256) \
    && mkdir -p /opt/ps5/opengl-src \
    && tar -xf /opt/ps5/opengl-bundle/sources/ps5-opengl.tar -C /opt/ps5/opengl-src --strip-components=1 \
    && rm -f "/tmp/ps5-opengl-sdk-${PS5_OPENGL_VERSION}.tar.gz"

ENV PS5_NATIVE_APP_TEMPLATE=/opt/ps5/boilerplate \
    PS5_PAYLOAD_SDK=/opt/ps5/payload-sdk \
    PS5_OPENGL_PREFIX=/opt/ps5/opengl \
    PS5_OPENGL_SOURCE=/opt/ps5/opengl-src \
    PS5_CLANG=/usr/bin/clang-18 \
    USE_CCACHE=1 \
    CCACHE_DIR=/work/.ccache

WORKDIR /work
CMD ["bash"]

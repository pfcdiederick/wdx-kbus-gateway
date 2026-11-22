# syntax=docker/dockerfile:1
#
# WAGO KBUS gateway as a container - multi-platform build with docker buildx.
# Alternative to the native package (tools/build-ipk.sh); run it with
# tools/deploy-docker.sh, which knows the mounts the KBUS access needs (docs/MANUAL.md 2.3).
# The build context is the PROJECT ROOT (src/, web/, sdk/); ignore file:
# Dockerfile.dockerignore next to this file. From the project root:
#
#   PFC300 (arm64):
#     docker buildx build --platform linux/arm64 -f docker/kbus-gateway/Dockerfile \
#         -t kbus-gateway:2.3.0-arm64 --load .
#   PFC100 / PFC200 (armv7): --platform linux/arm/v7
#
#   Simulation image for a PC (no SDK headers needed):
#     docker buildx build --platform linux/amd64 --build-arg REQUIRE_ADI=0 \
#         -f docker/kbus-gateway/Dockerfile -t kbus-gateway:sim --load .
#
# The build stage always runs natively on the build host and cross-compiles;
# the runtime stage has no RUN steps, so no QEMU emulation is required.

ARG DEBIAN_RELEASE=trixie

# ---------------------------------------------------------------------------
# Build stage (runs on the build host, cross-compiles for $TARGETPLATFORM)
# ---------------------------------------------------------------------------
FROM --platform=$BUILDPLATFORM debian:${DEBIAN_RELEASE} AS build
ARG TARGETARCH
ARG BUILDARCH
# 1 = fail when the WAGO SDK headers (sdk/include) are missing
ARG REQUIRE_ADI=1
SHELL ["/bin/bash", "-c"]

RUN set -eux; \
    case "${TARGETARCH}" in \
      arm)   triplet=arm-linux-gnueabihf; debarch=armhf ;; \
      arm64) triplet=aarch64-linux-gnu;   debarch=arm64 ;; \
      amd64) triplet=x86_64-linux-gnu;    debarch=amd64 ;; \
      *) echo "unsupported TARGETARCH ${TARGETARCH}"; exit 1 ;; \
    esac; \
    apt-get update; \
    if [ "${TARGETARCH}" = "${BUILDARCH}" ]; then \
        apt-get install -y --no-install-recommends make gcc libc6-dev binutils; \
    else \
        apt-get install -y --no-install-recommends make binutils-${triplet//_/-} \
            gcc-${triplet//_/-} libc6-dev-${debarch}-cross; \
    fi; \
    rm -rf /var/lib/apt/lists/*; \
    echo "${triplet}" > /triplet

WORKDIR /src
COPY Makefile VERSION ./
COPY src ./src
COPY web ./web
COPY sdk ./sdk

RUN set -eux; \
    triplet=$(cat /triplet); \
    make CC="${triplet}-gcc" REQUIRE_ADI="${REQUIRE_ADI}" CFLAGS="-O2 -g0"; \
    "${triplet}-strip" build/kbus-gateway

# ---------------------------------------------------------------------------
# Runtime stage
# ---------------------------------------------------------------------------
# glibc of this image must be >= the PFC firmware's glibc, because the WAGO
# libraries are loaded from the controller at runtime (see docs/MANUAL.md).
FROM debian:${DEBIAN_RELEASE}-slim AS runtime
LABEL org.opencontainers.image.title="kbus-gateway" \
      org.opencontainers.image.description="WAGO KBUS DI/DO/AI/AO and shared registers as wdx data layer for containers"

COPY --from=build /src/build/kbus-gateway /usr/local/bin/kbus-gateway
COPY --chmod=755 docker/kbus-gateway/entrypoint.sh /usr/local/bin/entrypoint.sh
# check of the dashboard at /kbus, run by the entrypoint before the gateway starts
COPY --chmod=644 docker/manager/kbus-dashboard-check.sh /usr/local/lib/kbus-gateway/kbus-dashboard-check.sh

# The same defaults as the native package (packaging/ipk/etc/kbus-gateway.conf).
# Mount /run/kbus-gateway from the controller so other containers find the socket.
ENV WDX_SOCKET=/run/kbus-gateway/wdx.sock \
    HTTP_BIND=127.0.0.1 \
    HTTP_PORT=8000 \
    HTTP_PRIORITY=30 \
    KBUS_CYCLE_MS=5 \
    KBUS_PRIORITY=40 \
    REGISTER_FILE=/etc/kbus-gateway-registers.conf \
    REGISTER_STORE=/home/kbus-gateway/registers.dat
EXPOSE 8000
STOPSIGNAL SIGTERM
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]

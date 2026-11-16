# syntax=docker/dockerfile:1
#
# WAGO KBUS HTTP gateway - multi-platform build with docker buildx.
#
#   PFC100 / PFC200 (armv7):
#     docker buildx build --platform linux/arm/v7 -t kbus-gateway:latest \
#         --output type=docker,dest=kbus-gateway.tar .
#
#   Simulation image for a PC (no SDK headers needed):
#     docker buildx build --platform linux/amd64 --build-arg REQUIRE_ADI=0 \
#         -t kbus-gateway:sim --load .
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
COPY Makefile ./
COPY src ./src
COPY sdk ./sdk

RUN set -eux; \
    triplet=$(cat /triplet); \
    make CC="${triplet}-gcc" REQUIRE_ADI="${REQUIRE_ADI}" CFLAGS="-O2 -g0"; \
    "${triplet}-strip" build/kbus-gateway

# ---------------------------------------------------------------------------
# Runtime stage
# ---------------------------------------------------------------------------
# glibc of this image must be >= the PFC firmware's glibc, because the WAGO
# libraries are loaded from the controller at runtime (see README).
FROM debian:${DEBIAN_RELEASE}-slim AS runtime
LABEL org.opencontainers.image.title="kbus-gateway" \
      org.opencontainers.image.description="REST I/O server for WAGO KBUS DI/DO/AI/AO channels"

COPY --from=build /src/build/kbus-gateway /usr/local/bin/kbus-gateway
COPY --chmod=755 docker/entrypoint.sh /usr/local/bin/entrypoint.sh

ENV HTTP_PORT=8000 \
    KBUS_CYCLE_MS=10 \
    KBUS_PRIORITY=40
EXPOSE 8000
STOPSIGNAL SIGTERM
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]

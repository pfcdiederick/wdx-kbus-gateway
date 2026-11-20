# kbus-iec104: KBUS inputs of the kbus-gateway on an IEC 60870-5-104 server (port 2404).
# Build context: the docker/ folder (for the shared lib/wdx_client.py). From the project root:
#
#   docker buildx build --platform linux/arm64 -f docker/iec104/Dockerfile -t kbus-iec104:arm64 --load docker
#
#   docker run -d --name kbus-iec104 --restart unless-stopped -p 2404:2404 \
#       -v /run/kbus-gateway:/run/kbus-gateway kbus-iec104:arm64
#
# Own parameter file without rebuilding: -v /home/kbus-iec104/wdx_iec104_config.ini:/app/wdx_iec104_config.ini
# (tools/deploy-docker.sh <pfc-ip> --iec104 creates and mounts it).
#
# Cross-build without emulation: the first stage runs on the PC's own platform and
# downloads the prebuilt c104 wheel for the target; the final image only copies it
# (no RUN step for the target, so no QEMU/binfmt is needed). c104 has wheels for
# arm64 (PFC300) and amd64, not for armv7 (PFC100/PFC200).

# c104 = Python binding of mz-automation/lib60870-C; no further Python dependencies
FROM --platform=$BUILDPLATFORM python:3.10-slim AS wheels
ARG TARGETARCH
RUN case "$TARGETARCH" in \
        arm64) P=manylinux2014_aarch64 ;; \
        amd64) P=manylinux_2_28_x86_64 ;; \
        *) echo "c104 has no prebuilt wheel for $TARGETARCH" >&2; exit 1 ;; \
    esac && \
    pip install --no-cache-dir --only-binary=:all: --platform "$P" --python-version 3.10 \
        --target /deps c104==2.2.1

FROM python:3.10-slim
WORKDIR /app
COPY --from=wheels /deps /usr/local/lib/python3.10/site-packages
COPY lib/wdx_client.py iec104/wdx_2_iec104.py iec104/wdx_iec104_config.ini ./
EXPOSE 2404
ENTRYPOINT ["python", "-u", "/app/wdx_2_iec104.py"]
CMD ["--config", "/app/wdx_iec104_config.ini"]

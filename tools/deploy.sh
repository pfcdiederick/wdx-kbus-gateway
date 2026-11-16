#!/usr/bin/env bash
# Build the gateway image with docker buildx, save it as an archive in the
# project directory, copy it to the PFC over SSH, load it and (re)start the
# container. See README section 2 (PFC100/PFC200) and section 2.10 (PFC300).
#
# The platform is taken from the controller (uname -m):
#   armv7l  (PFC100, PFC200)  -> linux/arm/v7  -> kbus-gateway-armv7.tar.gz
#   aarch64 (PFC300)          -> linux/arm64   -> kbus-gateway-arm64.tar.gz
#
#   tools/deploy.sh <pfc-ip> [http-port]
#
# Environment:
#   PFC_USER        SSH user                        (default root)
#   IMAGE           image name                      (default kbus-gateway:latest)
#   REQUIRE_ADI     1 = real KBUS (needs sdk/include), 0 = simulation only (default 1)
#   SKIP_BUILD      1 = reuse the existing archive  (default 0)
#   PLATFORM        linux/arm/v7 or linux/arm64     (default: detected on the PFC)
#   EXTRA_RUN_ARGS  extra "docker run" arguments, e.g. "-e KBUS_SIMULATION=1"
set -euo pipefail

PFC=${1:?usage: $0 <pfc-ip> [http-port]}
PORT=${2:-8000}
PFC_USER=${PFC_USER:-root}
IMAGE=${IMAGE:-kbus-gateway:latest}
REQUIRE_ADI=${REQUIRE_ADI:-1}
SKIP_BUILD=${SKIP_BUILD:-0}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [[ -z "${PLATFORM:-}" ]]; then
    machine=$(ssh "$PFC_USER@$PFC" uname -m)
    case "$machine" in
        armv7*)  PLATFORM=linux/arm/v7 ;;
        aarch64) PLATFORM=linux/arm64 ;;
        *) echo "unknown controller architecture '$machine'; set PLATFORM" >&2; exit 1 ;;
    esac
    echo ">>> controller $PFC is $machine -> $PLATFORM"
fi
case "$PLATFORM" in
    linux/arm/v7) ARCH=armv7 ;;
    linux/arm64)  ARCH=arm64 ;;
    *) echo "unsupported PLATFORM '$PLATFORM'" >&2; exit 1 ;;
esac
ARCHIVE="$ROOT/kbus-gateway-$ARCH.tar.gz"
[[ "$REQUIRE_ADI" == 1 ]] || ARCHIVE="$ROOT/kbus-gateway-$ARCH-sim.tar.gz"   # simulation-only build

if [[ "$SKIP_BUILD" != 1 ]]; then
    echo ">>> 1/4 building $IMAGE for $PLATFORM (REQUIRE_ADI=$REQUIRE_ADI)"
    docker buildx build --platform "$PLATFORM" \
        --build-arg REQUIRE_ADI="$REQUIRE_ADI" \
        -t "$IMAGE" --load "$ROOT"

    echo ">>> 2/4 saving image to $ARCHIVE"
    docker save "$IMAGE" | gzip > "$ARCHIVE"
    ls -lh "$ARCHIVE"
else
    echo ">>> 1-2/4 SKIP_BUILD=1: using existing $ARCHIVE"
    [[ -f "$ARCHIVE" ]] || { echo "$ARCHIVE not found" >&2; exit 1; }
fi

echo ">>> 3/4 copying archive to $PFC_USER@$PFC and loading it"
scp -q "$ARCHIVE" "$PFC_USER@$PFC:/tmp/kbus-gateway.tar.gz"
ssh "$PFC_USER@$PFC" 'docker load -i /tmp/kbus-gateway.tar.gz && rm -f /tmp/kbus-gateway.tar.gz && docker image prune -f >/dev/null'

echo ">>> 4/4 (re)starting container kbus-gateway"
# shellcheck disable=SC2087
ssh "$PFC_USER@$PFC" sh -s <<EOF
set -e
if pidof codesys3 >/dev/null 2>&1 || pidof plclinux_rt >/dev/null 2>&1; then
    echo "WARNING: a PLC runtime is running and owns the KBUS."
    echo "         Stop it first (e.g. WBM > PLC Runtime > 'None', or /etc/init.d/runtime stop)."
fi
docker rm -f kbus-gateway >/dev/null 2>&1 || true
# Mount these only when they exist: Docker refuses to start when a bind
# source is missing, and not every controller/firmware has all of them.
OPT=""
for f in /etc/config-tools/get_typelabel_value /dev/eeprom /dev/ttyKbus /dev/log; do
    case "\$f" in /etc/*) ro=":ro" ;; *) ro="" ;; esac
    if [ -e "\$f" ]; then OPT="\$OPT -v \$f:\$f\$ro"; else echo "note: \$f not present, not mounted"; fi
done
docker run -d --name kbus-gateway --restart unless-stopped \
    --privileged --network host \
    -v /usr/lib:/host/usr/lib:ro \
    -v /var/run/dbus:/var/run/dbus \
    -v /etc/specific:/etc/specific:ro \
    \$OPT \
    -e HTTP_PORT=$PORT \
    ${EXTRA_RUN_ARGS:-} \
    $IMAGE >/dev/null
sleep 6
docker ps -a --filter name=kbus-gateway --format '{{.Names}}: {{.Status}}'
docker logs kbus-gateway 2>&1
EOF

echo ">>> done. Test: curl http://$PFC:$PORT/DO/1   or   tools/test-rest.py $PFC node"

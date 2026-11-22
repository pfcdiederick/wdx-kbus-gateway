#!/usr/bin/env bash
# Run the gateway as a Docker container on the PFC instead of the native
# package, or switch back. Optionally also the example client and the
# management web interface.
#
#   tools/deploy-docker.sh <pfc-ip> [options]
#     (no option)    gateway container (replaces the native service)
#     --client       also the example client (docker/example-client)
#     --manager      also the management web interface (docker/manager): the dashboard at
#                    http(s)://<pfc-ip>/kbus through the controller's own web server
#     --104-gateway  also the IEC 60870-5-104 server 104-gateway (docker/104-gateway), port 2404
#                    (--iec104 = the same, old name)
#   Images: docker/<kbus-gateway|example-client|manager|104-gateway>/Dockerfile (docs/MANUAL.md, 6.1);
#   archives for the controller: build-docker/
#     --no-gateway   leave the gateway as it is (native or container); e.g. only --manager
#     --native       remove the gateway container, native service again
#                    (example client and manager stay: same socket)
#
# Manager login: the website is open by default; Reset Statistics, Local Control and
# the login settings always need the login (default admin / wago until a password is
# set on the Settings tab; stored in /home/kbus-manager/settings.json).
# Control socket /run/kbus-gateway-control (mode 700): mounted ONLY into the gateway
# and the manager: requests on it may use Local Control and Reset Statistics.
# /home/kbus-manager/manager.env (optional) holds extra environment, e.g. MGMT_READONLY=1.
#
# Only one program may own the KBUS: the native service is stopped and its boot
# link removed while the container runs (--native restores both). The native
# package can stay installed; its /etc/kbus-gateway-registers.conf is used by
# the container too.
#
# Environment:
#   PFC_USER    SSH user (default root)
#   SKIP_BUILD  1 = use the existing archives in build-docker/
#   GW_ENV      extra docker run arguments for the gateway, e.g. "-e KBUS_CYCLE_MS=2"
#   MG_ENV      extra docker run arguments for the manager, e.g. "-e MGMT_READONLY=1"
#   CL_ENV      extra docker run arguments for the example client, e.g. "-e PYTHON_RESPONSE="
#               (no automatic answers to Codesys/Message)
#   MG_PORT     port of the manager container on the controller (default 8090)
#   MG_BIND     address the manager port is published on (default 127.0.0.1: only via /kbus;
#               0.0.0.0 = also directly at http://<pfc-ip>:MG_PORT/)
#   MG_LIGHTTPD 0 = do not install /etc/lighttpd/apps.confd/kbus.conf (no /kbus on port 80/443)
#   IEC_PORT    port of the IEC 104 server on the controller (default 2404)
#
# IEC 104 parameter file: /home/104-gateway/wdx_iec104_config.ini on the controller
# (created from the image's default at the first deployment, then kept; edit it
# and run "docker restart 104-gateway").
set -euo pipefail

PFC=${1:?usage: $0 <pfc-ip> [--client] [--manager] [--104-gateway] [--no-gateway] | --native}
shift
GW=1; CL=0; MG=0; IEC=0; NATIVE=0
for a in "$@"; do
    case "$a" in
        --client)     CL=1 ;;
        --manager)    MG=1 ;;
        --104-gateway|--iec104) IEC=1 ;;
        --no-gateway) GW=0 ;;
        --native)     NATIVE=1 ;;
        *) echo "unknown option $a" >&2; exit 1 ;;
    esac
done
MG_PORT=${MG_PORT:-8090}
MG_BIND=${MG_BIND:-127.0.0.1}
MG_LIGHTTPD=${MG_LIGHTTPD:-1}
IEC_PORT=${IEC_PORT:-2404}
PFC_USER=${PFC_USER:-root}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION=$(tr -d ' \n' < "$ROOT/VERSION")
SSH="ssh -o BatchMode=yes $PFC_USER@$PFC"
OUT=$ROOT/build-docker

if [[ $NATIVE == 1 ]]; then
    echo ">>> back to the native service"
    $SSH 'docker rm -f kbus-gateway >/dev/null 2>&1 || true
          [ -x /etc/init.d/kbus-gateway ] || { echo "native package not installed (tools/deploy.sh)"; exit 1; }
          ln -sf ../init.d/kbus-gateway /etc/rc.d/S99_kbus-gateway
          /etc/init.d/kbus-gateway start'
    exit 0
fi

machine=$($SSH uname -m)
case "$machine" in
    aarch64) PLATFORM=linux/arm64; ARCH=arm64 ;;
    armv7*)  PLATFORM=linux/arm/v7; ARCH=armv7 ;;
    *) echo "unknown controller architecture '$machine'" >&2; exit 1 ;;
esac
GW_IMG=kbus-gateway:$VERSION-$ARCH
CL_IMG=kbus-example-client:$ARCH
MG_IMG=kbus-manager:$ARCH
IEC_IMG=104-gateway:$ARCH
GW_TAR=$OUT/kbus-gateway-$VERSION-$ARCH.tar.gz
CL_TAR=$OUT/kbus-example-client-$ARCH.tar.gz
MG_TAR=$OUT/kbus-manager-$ARCH.tar.gz
IEC_TAR=$OUT/104-gateway-$ARCH.tar.gz
echo ">>> controller $PFC is $machine -> $PLATFORM"

if [[ "${SKIP_BUILD:-0}" != 1 ]]; then
    echo ">>> 1/4 building images"
    mkdir -p "$OUT"
    # build <image> <archive> <docker/ folder> [context]: context = the folder, except
    # for the gateway (project root: src/, web/, sdk/) and the Python apps that share
    # docker/lib/wdx_client.py (context docker/)
    build() {
        docker buildx build --platform "$PLATFORM" -t "$1" -f "$ROOT/docker/$3/Dockerfile" --load "${4:-$ROOT/docker/$3}" &&
        docker save "$1" | gzip > "$2" && docker rmi "$1" >/dev/null     # keep only the archive on the PC
    }
    [[ $GW == 1 ]] && build "$GW_IMG" "$GW_TAR" kbus-gateway "$ROOT"
    [[ $CL == 1 ]] && build "$CL_IMG" "$CL_TAR" example-client "$ROOT/docker"
    [[ $MG == 1 ]] && build "$MG_IMG" "$MG_TAR" manager
    [[ $IEC == 1 ]] && build "$IEC_IMG" "$IEC_TAR" 104-gateway "$ROOT/docker"
else
    echo ">>> 1/4 SKIP_BUILD=1: using $OUT"
fi

echo ">>> 2/4 loading images on the controller"
[[ $GW == 1 ]] && $SSH docker load < "$GW_TAR"
[[ $CL == 1 ]] && $SSH docker load < "$CL_TAR"
[[ $MG == 1 ]] && $SSH docker load < "$MG_TAR"
[[ $IEC == 1 ]] && $SSH docker load < "$IEC_TAR"

if [[ $GW == 1 ]]; then
echo ">>> 3/4 stopping the native service, starting the gateway container"
# shellcheck disable=SC2087
$SSH sh -s <<EOF
set -e
if [ -x /etc/init.d/kbus-gateway ]; then
    /etc/init.d/kbus-gateway stop
    rm -f /etc/rc.d/S99_kbus-gateway      # not at boot while the container is used
fi
docker rm -f kbus-gateway >/dev/null 2>&1 || true
mkdir -p /run/kbus-gateway /home/kbus-gateway
mkdir -p -m 700 /run/kbus-gateway-control      # control socket: only gateway + manager
OPT=""
for f in /etc/config-tools/get_typelabel_value:ro /dev/eeprom: /dev/ttyKbus: /dev/serial: /dev/log: \
         /etc/localtime:ro /etc/kbus-gateway-registers.conf:ro; do
    p=\${f%%:*}; ro=\${f#*:}; [ -n "\$ro" ] && ro=":\$ro"
    if [ -e "\$p" ]; then OPT="\$OPT -v \$p:\$p\$ro"; else echo "note: \$p not present, not mounted"; fi
done
docker run -d --name kbus-gateway --restart unless-stopped \
    --privileged --network host --pid host \
    -v /usr/lib:/host/usr/lib:ro \
    -v /var/run/dbus:/var/run/dbus \
    -v /etc/specific:/etc/specific:ro \
    -v /run/kbus-gateway:/run/kbus-gateway \
    -v /run/kbus-gateway-control:/run/kbus-gateway-control \
    -v /home/kbus-gateway:/home/kbus-gateway \
    \$OPT -e LOG_TARGET=syslog ${GW_ENV:-} \
    $GW_IMG >/dev/null
EOF
else
    echo ">>> 3/4 gateway left as it is (--no-gateway)"
fi

if [[ $CL == 1 ]]; then
    echo ">>> starting the example client"
    $SSH "docker rm -f kbus-example-client >/dev/null 2>&1 || true
          docker run -d --name kbus-example-client --restart unless-stopped --network none ${CL_ENV:-} \
              -v /run/kbus-gateway:/run/kbus-gateway -v /etc/localtime:/etc/localtime:ro $CL_IMG >/dev/null"
fi

if [[ $MG == 1 ]]; then
    echo ">>> starting the manager on $MG_BIND:$MG_PORT"
    # shellcheck disable=SC2087
    $SSH sh -s <<EOF
set -e
mkdir -p /home/kbus-manager && chmod 700 /home/kbus-manager
mkdir -p -m 700 /run/kbus-gateway-control
[ -e /home/kbus-manager/manager.env ] || ( umask 077; : > /home/kbus-manager/manager.env )
docker rm -f kbus-manager >/dev/null 2>&1 || true
docker run -d --name kbus-manager --restart unless-stopped -p $MG_BIND:$MG_PORT:8080 \
    --env-file /home/kbus-manager/manager.env ${MG_ENV:-} -v /home/kbus-manager:/data \
    -v /run/kbus-gateway-control:/run/kbus-gateway-control \
    -v /run/kbus-gateway:/run/kbus-gateway -v /etc/localtime:/etc/localtime:ro $MG_IMG >/dev/null
EOF
    if [[ $MG_LIGHTTPD == 1 ]]; then
        # The controller's own web server forwards /kbus to the manager (port 80 / 443).
        # The wanted config is kept in /home (survives a firmware update); the gateway
        # container checks it at every start (kbus-dashboard-check.sh) and the same
        # check installs it now: lighttpd -tt first, a rejected config restores the old state.
        echo ">>> dashboard at /kbus: /etc/lighttpd/apps.confd/kbus.conf -> 127.0.0.1:$MG_PORT"
        sed "s/__MG_PORT__/$MG_PORT/" "$ROOT/docker/manager/kbus-lighttpd.conf" |
            $SSH 'cat > /home/kbus-manager/lighttpd-kbus.conf'
        $SSH sh -s < "$ROOT/docker/manager/kbus-dashboard-check.sh"
    else
        echo ">>> no dashboard at /kbus (MG_LIGHTTPD=0)"
        $SSH 'rm -f /home/kbus-manager/lighttpd-kbus.conf
              if [ -e /etc/lighttpd/apps.confd/kbus.conf ]; then
                  rm -f /etc/lighttpd/apps.confd/kbus.conf && /etc/init.d/lighttpd restart >/dev/null
              fi'
    fi
fi

if [[ $IEC == 1 ]]; then
    echo ">>> starting the IEC 104 server 104-gateway on port $IEC_PORT"
    # shellcheck disable=SC2087
    $SSH sh -s <<EOF
set -e
mkdir -p /home/104-gateway
C=/home/104-gateway/wdx_iec104_config.ini
if [ ! -s \$C ] && [ -s /home/kbus-iec104/wdx_iec104_config.ini ]; then   # old name: keep its parameters
    cp /home/kbus-iec104/wdx_iec104_config.ini \$C
    echo "parameter file taken over from /home/kbus-iec104"
fi
if [ ! -s \$C ]; then
    docker run --rm --entrypoint cat $IEC_IMG /app/wdx_iec104_config.ini > \$C
    echo "default parameter file created: \$C"
fi
docker rm -f kbus-iec104 104-gateway >/dev/null 2>&1 || true   # kbus-iec104 = old name (until 2.6.0)
docker run -d --name 104-gateway --restart unless-stopped -p $IEC_PORT:2404 \
    -v /run/kbus-gateway:/run/kbus-gateway -v \$C:/app/wdx_iec104_config.ini:ro \
    -v /etc/localtime:/etc/localtime:ro $IEC_IMG >/dev/null
EOF
fi

echo ">>> 4/4 status"
# shellcheck disable=SC2016
$SSH 'sleep 4; docker ps -a --filter name=kbus- --filter name=104-gateway --format "{{.Names}}: {{.Status}}";
      grep "kbus-gateway\[" /var/log/messages | tail -8'

[[ $IEC == 1 ]] && echo ">>> IEC 104: $PFC:$IEC_PORT, common address 1 (parameters: /home/104-gateway/wdx_iec104_config.ini)"
if [[ $MG == 1 ]]; then
    [[ $MG_LIGHTTPD == 1 ]] && echo ">>> dashboard: http://$PFC/kbus  (WBM in HTTPS-only mode: redirected to https://$PFC/kbus/)"
    [[ $MG_BIND != 127.0.0.1 ]] && echo ">>> dashboard directly: http://$PFC:$MG_PORT/"
    echo "    login: Settings tab of the dashboard"
fi
echo ">>> done. Back to the native service: $0 $PFC --native"

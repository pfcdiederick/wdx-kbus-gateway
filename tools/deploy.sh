#!/usr/bin/env bash
# Build the native package, copy it to the PFC, install it with opkg and show
# the result. One command for the whole development loop.
#
#   tools/deploy.sh <pfc-ip>
#
# The package architecture is taken from the controller (uname -m):
#   aarch64 (PFC300)          -> arm64
#   armv7l  (PFC100, PFC200)  -> armhf
#
# Environment:
#   PFC_USER    SSH user                      (default root)
#   SKIP_BUILD  1 = install the existing build-ipk/kbus-gateway_<version>_<arch>.ipk
#
# Before the first install: only one program may own the KBUS. Set WBM > PLC
# Runtime to "None", or run the CODESYS runtime without KBUS (README 2.2).
set -euo pipefail

PFC=${1:?usage: $0 <pfc-ip>}
PFC_USER=${PFC_USER:-root}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION=$(tr -d ' \n' < "$ROOT/VERSION")
SSH="ssh -o BatchMode=yes $PFC_USER@$PFC"

machine=$($SSH uname -m)
case "$machine" in
    aarch64) ARCH=arm64 ;;
    armv7*)  ARCH=armhf ;;
    *) echo "unknown controller architecture '$machine'" >&2; exit 1 ;;
esac
PKG="$ROOT/build-ipk/kbus-gateway_${VERSION}_${ARCH}.ipk"
echo ">>> controller $PFC is $machine -> $ARCH"

if [[ "${SKIP_BUILD:-0}" != 1 ]]; then
    echo ">>> 1/3 building $PKG"
    "$ROOT/tools/build-ipk.sh" "$ARCH"
else
    echo ">>> 1/3 SKIP_BUILD=1: using $PKG"
fi
[[ -f "$PKG" ]] || { echo "$PKG not found" >&2; exit 1; }

echo ">>> 2/3 copying and installing"
scp -q "$PKG" "$PFC_USER@$PFC:/tmp/"
$SSH "opkg install /tmp/${PKG##*/} && rm -f /tmp/${PKG##*/}"

echo ">>> 3/3 status"
# shellcheck disable=SC2016
$SSH 'sleep 3; /etc/init.d/kbus-gateway status; P=$(cat /var/run/kbus-gateway.pid 2>/dev/null);
      grep "kbus-gateway\[$P\]" /var/log/messages | tail -12'
echo ">>> done. Test: curl http://$PFC:8000/ALL   or   tools/test-rest.py $PFC node"

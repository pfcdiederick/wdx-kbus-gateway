#!/usr/bin/env bash
# Build the native opkg package kbus-gateway_<version>_<arch>.ipk for the
# PTXdist firmware of a WAGO PFC (no Docker needed on the controller).
#
#   tools/build-ipk.sh [arm64|armhf]        arm64 = PFC300 (default), armhf = PFC100/PFC200
#
# Compiles with the WAGO toolchain of the firmware SDK builder image (same
# GCC/glibc as the firmware) and packs the result like a PTXdist package:
#   ar archive: debian-binary, control.tar.gz, data.tar.gz
#
# Needs: the WAGO SDK headers in sdk/include (tools/import-sdk-packages.sh)
#        and the docker image of the firmware SDK (BUILDER_IMAGE, default
#        pfc300-builder:latest; created by "make builder" in pfc300-firmware-sdk).
# Result: build-ipk/kbus-gateway_<version>_<arch>.ipk
#
# Install on the PFC: WBM > Software Uploads, or
#   scp build-ipk/kbus-gateway_*_arm64.ipk root@<pfc>:/tmp/ && ssh root@<pfc> opkg install /tmp/kbus-gateway_*_arm64.ipk
set -euo pipefail

ARCH=${1:-arm64}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(tr -d ' \n' < "$ROOT/VERSION")
IMAGE=${BUILDER_IMAGE:-pfc300-builder:latest}
TCROOT=/opt/gcc-Toolchain-2022.08-wago.1/LINARO.Toolchain-2022.08-wago.1

case "$ARCH" in
    arm64) TRIPLET=aarch64-linux-gnu ;;
    armhf) TRIPLET=arm-linux-gnueabihf ;;
    *) echo "unknown arch '$ARCH' (arm64 | armhf)" >&2; exit 1 ;;
esac
[ -f "$ROOT/sdk/include/dal/adi_application_interface.h" ] || {
    echo "sdk/include/dal is missing: import the WAGO SDK headers first (sdk/README.md)" >&2; exit 1; }

OUT=$ROOT/build-ipk
WORK=$OUT/$ARCH
rm -rf "$WORK"
mkdir -p "$WORK/obj" "$WORK/data/usr/bin" "$WORK/control"

echo ">>> 1/3 compiling for $ARCH with $IMAGE"
docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT":/src -w /src --entrypoint sh "$IMAGE" -c "
    set -e
    export PATH=$TCROOT/$TRIPLET/bin:\$PATH
    make -s BUILD=build-ipk/$ARCH/obj CC=$TRIPLET-gcc REQUIRE_ADI=1 CFLAGS='-O2 -Werror'
    $TRIPLET-strip build-ipk/$ARCH/obj/kbus-gateway"

echo ">>> 2/3 package tree"
install -m 755 "$WORK/obj/kbus-gateway"                         "$WORK/data/usr/bin/kbus-gateway"
mkdir -p "$WORK/data/etc/init.d"
install -m 755 "$ROOT/packaging/ipk/etc/init.d/kbus-gateway"    "$WORK/data/etc/init.d/kbus-gateway"
install -m 644 "$ROOT/packaging/ipk/etc/kbus-gateway.conf"      "$WORK/data/etc/kbus-gateway.conf"
install -m 644 "$ROOT/packaging/ipk/etc/kbus-gateway-registers.conf" "$WORK/data/etc/kbus-gateway-registers.conf"
install -m 755 "$ROOT/packaging/ipk/control/postinst"           "$WORK/control/postinst"
install -m 755 "$ROOT/packaging/ipk/control/prerm"              "$WORK/control/prerm"
install -m 644 "$ROOT/packaging/ipk/control/conffiles"          "$WORK/control/conffiles"
SIZE=$(du -sb "$WORK/data" | cut -f1)
cat > "$WORK/control/control" <<EOF
Package: kbus-gateway
Priority: optional
Version: $VERSION
Section: base
Architecture: $ARCH
Maintainer: pfcdiederick
Installed-Size: $SIZE
Depends: dal, libdbuskbuscommon
Source: https://github.com/pfcdiederick/docker-kbus-gateway
Description: KBUS gateway: KBus DI/DO/AI/AO and shared registers as wdx data layer for containers (Unix socket, JSON:API)
EOF

echo ">>> 3/3 ipk"
# Directories as on the firmware (755): opkg must never change /usr or /etc
find "$WORK/data" "$WORK/control" -type d -exec chmod 755 {} +
PKG=$OUT/kbus-gateway_${VERSION}_${ARCH}.ipk
echo "2.0" > "$WORK/debian-binary"
# Archive the entries below the root (./usr, ./etc ...), NOT "./" itself: opkg
# would record "." as a package file and fail on it during upgrades.
pack() { ( cd "$1" && tar --owner=0 --group=0 --numeric-owner -czf "$2" $(find . -mindepth 1 -maxdepth 1 | sort) ); }
pack "$WORK/control" "$WORK/control.tar.gz"
pack "$WORK/data"    "$WORK/data.tar.gz"
rm -f "$PKG"
( cd "$WORK" && ar rc "$PKG" debian-binary control.tar.gz data.tar.gz )
ls -l "$PKG"
file "$WORK/data/usr/bin/kbus-gateway"

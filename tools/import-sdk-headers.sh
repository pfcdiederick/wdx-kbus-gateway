#!/usr/bin/env bash
# Copy the WAGO ADI/DAL + KBUS-info headers from a built WAGO PFC SDK (ptxproj)
# into ./sdk, so that tools/build-ipk.sh can compile the real KBUS backend.
#
#   tools/import-sdk-headers.sh <path-to-ptxproj> [platform-dir]
#
# Example: tools/import-sdk-headers.sh ~/wago/ptxproj
# The SDK must have been built (ptxdist go / images) for the firmware that runs on the PFC.
set -euo pipefail

PTXPROJ=${1:?usage: $0 <path-to-ptxproj> [platform-dir]}
PLATFORM=${2:-}
DEST="$(cd "$(dirname "$0")/.." && pwd)/sdk"

if [[ -z "$PLATFORM" ]]; then
    PLATFORM=$(ls -d "$PTXPROJ"/platform-wago-pfc* 2>/dev/null | head -n1 || true)
fi
SYSROOT="$PLATFORM/sysroot-target"
[[ -d "$SYSROOT/usr/include" ]] || { echo "no sysroot-target found under '$PLATFORM'" >&2; exit 1; }
[[ -f "$SYSROOT/usr/include/dal/adi_application_interface.h" ]] || {
    echo "dal/adi_application_interface.h not found in $SYSROOT/usr/include" >&2; exit 1; }

rm -rf "$DEST/include" "$DEST/lib"
mkdir -p "$DEST/include" "$DEST/lib"

# Copy the complete target include tree: the WAGO headers pull in OsLinux, glib
# and dbus headers. The Makefile uses -idirafter, so the libc headers of the
# cross compiler keep priority over the copies of the SDK.
cp -a "$SYSROOT/usr/include/." "$DEST/include/"
for d in glib-2.0 dbus-1.0; do
    [[ -d "$SYSROOT/usr/lib/$d/include" ]] && mkdir -p "$DEST/lib/$d" && cp -a "$SYSROOT/usr/lib/$d/include" "$DEST/lib/$d/"
done

echo "Imported SDK headers from $SYSROOT into $DEST"
ls "$DEST/include/dal/adi_application_interface.h"
find "$DEST/include" -maxdepth 2 -name 'ldkc_kbus_information.h' | head -n1

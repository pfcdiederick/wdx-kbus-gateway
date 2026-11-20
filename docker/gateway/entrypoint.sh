#!/bin/sh
# Make the PFC firmware libraries (ADI/DAL, KBUS-info, glib, dbus, ...) loadable
# WITHOUT the firmware's own C runtime.
#
# /usr/lib of the controller is mounted at $HOST_LIB_DIR. It also contains the
# firmware's glibc, which must not be mixed with the container's dynamic
# loader. All libraries are therefore linked into $LINK_DIR except glibc and
# the GCC runtime, and only $LINK_DIR is put on LD_LIBRARY_PATH.
set -e
HOST_LIB_DIR=${HOST_LIB_DIR:-/host/usr/lib}
LINK_DIR=/tmp/host-libs

if [ -d "$HOST_LIB_DIR" ]; then
    rm -rf "$LINK_DIR"
    mkdir -p "$LINK_DIR"
    for f in "$HOST_LIB_DIR"/*.so*; do
        [ -e "$f" ] || continue
        n=${f##*/}
        case "$n" in
            ld-linux*|libc.so*|libc-*|libm.so*|libm-*|libmvec*|libpthread*|libdl.so*|libdl-*|\
            librt.so*|librt-*|libresolv*|libutil*|libanl*|libnss_*|libnsl*|libthread_db*|\
            libBrokenLocale*|libmemusage*|libpcprofile*|libcrypt.so*|libgcc_s*|libstdc++*|libatomic*)
                continue ;;
        esac
        ln -s "$f" "$LINK_DIR/$n"
    done
    # Replace (not extend) the path, so the firmware's glibc is never found.
    export LD_LIBRARY_PATH="$LINK_DIR"

    # DAL device plugins. The firmware's /usr/lib/dal holds RELATIVE links
    # (libpackbus.so -> ../libpackbus.so), which must resolve inside
    # $HOST_LIB_DIR, not in the container's /usr/lib.
    if [ -d "$HOST_LIB_DIR/dal" ]; then
        if [ ! -e /usr/lib/dal ]; then
            # Normal case: link every plugin to its copy under $HOST_LIB_DIR/dal;
            # the kernel then resolves "../" relative to $HOST_LIB_DIR/dal.
            mkdir -p /usr/lib/dal
            for f in "$HOST_LIB_DIR"/dal/*; do
                [ -e "$f" ] && ln -s "$f" "/usr/lib/dal/${f##*/}"
            done
        else
            # /usr/lib/dal was bind-mounted (older run command): provide the
            # targets of its relative links in the container's /usr/lib.
            for f in /usr/lib/dal/*; do
                t=$(readlink "$f" 2>/dev/null) || continue
                case "$t" in
                    ../*) n=${t#../}
                          [ -e "/usr/lib/$n" ] || ln -s "$HOST_LIB_DIR/$n" "/usr/lib/$n" ;;
                esac
            done
        fi
    fi
else
    echo "[warn]  $HOST_LIB_DIR not mounted; WAGO libraries can only be found via LD_LIBRARY_PATH" >&2
fi

# Dashboard: before the gateway starts, check that http(s)://<controller>/kbus is
# set up in the controller's lighttpd, and repair it if not (a firmware update
# replaces /etc/lighttpd; /home with the wanted config and the containers stays).
# Runs in the controller's namespaces: needs --pid host (tools/deploy-docker.sh).
# KBUS_DASHBOARD_CHECK=0 switches it off.
DASHBOARD_CHECK=/usr/local/lib/kbus-gateway/kbus-dashboard-check.sh
if [ "${KBUS_DASHBOARD_CHECK:-1}" = 1 ] && [ -r "$DASHBOARD_CHECK" ]; then
    # with --pid host, PID 1 is the controller's init, not this script or docker-init
    if [ "$$" -ne 1 ] && ! tr '\0' ' ' </proc/1/cmdline | grep -q docker-init &&
       command -v nsenter >/dev/null 2>&1; then
        nsenter -t 1 -m -u -i -n -p sh -s <"$DASHBOARD_CHECK" || true
    else
        echo "[info]  dashboard check skipped (container without --pid host)"
    fi
fi

exec /usr/local/bin/kbus-gateway "$@"

#!/bin/sh
# kbus-dashboard-check: keep the dashboard at http(s)://<controller>/kbus working.
#
# Runs ON THE CONTROLLER (busybox sh):
#   - at every start of the gateway container, before the gateway starts
#     (docker/gateway/entrypoint.sh, through nsenter into the controller's namespaces)
#   - by tools/deploy-docker.sh --manager, right after it stored the wanted config
#
# Wanted config:    /home/kbus-manager/lighttpd-kbus.conf  (/home survives a firmware update)
# Installed config: /etc/lighttpd/apps.confd/kbus.conf     (a firmware update replaces /etc)
#
# Missing or different -> install, check the whole lighttpd config with lighttpd -tt,
# restart lighttpd. A rejected config restores the old state, so the WBM keeps working.
# No wanted config -> nothing to do (dashboard not deployed through lighttpd).
# Exit status: 0 = in order (unchanged or installed), 1 = rejected.
WANT=${KBUS_DASHBOARD_CONF:-/home/kbus-manager/lighttpd-kbus.conf}
DIR=/etc/lighttpd
CONF=$DIR/apps.confd/kbus.conf
CHECK_LOG=/tmp/kbus-lighttpd-check.log

log() {
    echo "kbus-dashboard: $*"
    logger -t kbus-dashboard "$*" 2>/dev/null || true
}

[ -f "$WANT" ] || exit 0
if [ ! -d "$DIR/apps.confd" ] || ! command -v lighttpd >/dev/null 2>&1; then
    log "no lighttpd with $DIR/apps.confd on this controller: /kbus not available"
    exit 0
fi
if [ -f "$CONF" ] && cmp -s "$WANT" "$CONF"; then
    exit 0                                              # in order: nothing to do
fi

if [ -e "$CONF" ]; then
    what="updated"
    cp "$CONF" "$CONF.old"                              # *.old / *.new: not matched by apps.confd/*.conf
else
    what="installed (missing, e.g. after a firmware update)"
    rm -f "$CONF.old"
fi
cp "$WANT" "$CONF.new" && mv "$CONF.new" "$CONF"

if ! (cd "$DIR" && lighttpd -tt -f lighttpd.conf) >"$CHECK_LOG" 2>&1; then
    if [ -e "$CONF.old" ]; then mv "$CONF.old" "$CONF"; else rm -f "$CONF"; fi
    log "lighttpd rejects $CONF, old state restored: $(head -n 3 "$CHECK_LOG" | tr '\n' ' ')"
    exit 1
fi
rm -f "$CONF.old"

# Join the control groups of process $1. Started from the gateway container
# (nsenter), this shell belongs to the container's cgroups; a lighttpd started
# from here would too, and docker would kill it with the next container stop.
# So the restart runs in the cgroups of the running lighttpd (cgroup v1 and v2).
join_cgroups() {
    [ -r "/proc/$1/cgroup" ] || return 0
    while IFS=: read -r _ ctrl path; do
        if [ -z "$ctrl" ]; then dir=/sys/fs/cgroup; else dir=/sys/fs/cgroup/${ctrl#name=}; fi
        [ -w "$dir$path/cgroup.procs" ] && echo $$ >"$dir$path/cgroup.procs" 2>/dev/null
    done <"/proc/$1/cgroup"
    return 0
}

pid=$(pidof lighttpd 2>/dev/null | cut -d' ' -f1)
if [ -n "$pid" ]; then
    join_cgroups "$pid"
    if /etc/init.d/lighttpd restart >/dev/null 2>&1; then
        log "$CONF $what, lighttpd restarted: /kbus available"
        pid=$(pidof lighttpd 2>/dev/null | cut -d' ' -f1)
        if [ -n "$pid" ] && grep -q docker "/proc/$pid/cgroup" 2>/dev/null; then
            log "WARNING: lighttpd runs in a docker cgroup; restart it on the controller: /etc/init.d/lighttpd restart"
        fi
    else
        log "$CONF $what, but the lighttpd restart failed: /etc/init.d/lighttpd restart"
    fi
else
    log "$CONF $what; lighttpd is not running yet and loads it when it starts"
fi
exit 0

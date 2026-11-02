#!/bin/bash

set -u

OMCPROXY="${1:?usage: $0 /path/to/omcproxy}"

UPLINK="omc-uplink"
DOWNLINK="omc-downlink"

LOG="$(mktemp)"
PID=""

wait_for_log()
{
    local pattern="$1"
    local timeout=100

    while [ "$timeout" -gt 0 ]; do
        if grep -Fq "$pattern" "$LOG"; then
            return 0
        fi

        if ! kill -0 "$PID" 2>/dev/null; then
            return 1
        fi

        timeout=$((timeout - 1))
        sleep 0.1
    done

    return 1
}

cleanup()
{
    echo
    echo "=== cleanup ==="

    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        kill "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
    fi

    ip link del "$UPLINK" 2>/dev/null || true
    ip link del "$DOWNLINK" 2>/dev/null || true

    echo
    echo "=== omcproxy log ==="
    cat "$LOG" 2>/dev/null || true

    rm -f "$LOG"
}

trap cleanup EXIT INT TERM

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

wait_for_interface()
{
    local dev="$1"
    local timeout=50

    while [ "$timeout" -gt 0 ]; do
        if ip link show dev "$dev" >/dev/null 2>&1; then
            return 0
        fi

        timeout=$((timeout - 1))
        sleep 0.1
    done

    return 1
}

wait_for_interface_gone()
{
    local dev="$1"
    local timeout=50

    while [ "$timeout" -gt 0 ]; do
        if ! ip link show dev "$dev" >/dev/null 2>&1; then
            return 0
        fi

        timeout=$((timeout - 1))
        sleep 0.1
    done

    return 1
}

get_ifindex()
{
    local dev="$1"

    ip -o link show dev "$dev" |
        awk -F': ' '{print $1}'
}

echo "=== omcproxy interface lifecycle test ==="

[ -x "$OMCPROXY" ] ||
    fail "omcproxy is not executable: $OMCPROXY"

#
# Initial state
#

echo "Cleaning up any stale test interfaces"

ip link del "$UPLINK" 2>/dev/null || true
ip link del "$DOWNLINK" 2>/dev/null || true

#
# Create only the uplink.
# The downlink deliberately does not exist.
#

echo "Creating uplink: $UPLINK"

ip link add "$UPLINK" type dummy
ip link set "$UPLINK" up

wait_for_interface "$UPLINK" ||
    fail "uplink was not created"

if ip link show dev "$DOWNLINK" >/dev/null 2>&1; then
    fail "downlink unexpectedly exists before daemon startup"
fi

echo "PASS: uplink exists"
echo "PASS: downlink does not exist"

#
# Start omcproxy with a missing interface.
#

echo "Starting omcproxy with missing downlink"

"$OMCPROXY" -v "$UPLINK,$DOWNLINK" >"$LOG" 2>&1 &
PID=$!

sleep 0.2

kill -0 "$PID" 2>/dev/null ||
    fail "omcproxy exited during startup with missing interface"

echo "PASS: omcproxy starts with missing downlink"

#
# Create the missing downlink.
#

echo "Creating downlink: $DOWNLINK"

ip link add "$DOWNLINK" type dummy
ip link set "$DOWNLINK" up

wait_for_interface "$DOWNLINK" ||
    fail "downlink was not created"

OLD_IFINDEX="$(get_ifindex "$DOWNLINK")"

[ -n "$OLD_IFINDEX" ] ||
    fail "could not determine initial downlink ifindex"

echo "Initial downlink ifindex: $OLD_IFINDEX"

wait_for_log "querier_iface_timer: sending generic IGMP-query on $OLD_IFINDEX" ||
    fail "omcproxy did not attach/use initial downlink ifindex $OLD_IFINDEX"

echo "PASS: omcproxy uses initial downlink ifindex $OLD_IFINDEX"

kill -0 "$PID" 2>/dev/null ||
    fail "omcproxy died after downlink creation"

echo "PASS: omcproxy survives downlink creation"

#
# Delete downlink.
#

echo "Removing downlink"

ip link del "$DOWNLINK"

wait_for_interface_gone "$DOWNLINK" ||
    fail "downlink still exists after deletion"

kill -0 "$PID" 2>/dev/null ||
    fail "omcproxy died after downlink deletion"

echo "PASS: omcproxy survives downlink deletion"

#
# Recreate downlink.
#

echo "Recreating downlink"

ip link add "$DOWNLINK" type dummy
ip link set "$DOWNLINK" up

wait_for_interface "$DOWNLINK" ||
    fail "recreated downlink was not created"

NEW_IFINDEX="$(get_ifindex "$DOWNLINK")"

[ -n "$NEW_IFINDEX" ] ||
    fail "could not determine new downlink ifindex"

echo "New downlink ifindex: $NEW_IFINDEX"

[ "$OLD_IFINDEX" != "$NEW_IFINDEX" ] ||
    fail "ifindex did not change: old=$OLD_IFINDEX new=$NEW_IFINDEX"

echo "PASS: recreated downlink has a new ifindex"

wait_for_log "querier_iface_timer: sending generic IGMP-query on $NEW_IFINDEX" ||
    fail "omcproxy did not attach/use new downlink ifindex $NEW_IFINDEX"

echo "PASS: omcproxy uses new downlink ifindex $NEW_IFINDEX"

#
# Final daemon check.
#

kill -0 "$PID" 2>/dev/null ||
    fail "omcproxy is no longer running"

echo "PASS: omcproxy is still running"

echo
echo "======================================"
echo "ALL TESTS PASSED"
echo "======================================"
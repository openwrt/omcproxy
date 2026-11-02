#!/bin/bash

set -u

OMCPROXY="${1:?usage: $0 /path/to/omcproxy}"

UPLINK="omc-uplink"
DOWNLINK="omc-downlink"
SRC="omc-src"
DST="omc-dst"

UPLINK_ADDR="192.0.2.1"
SRC_ADDR="192.0.2.2"
DOWNLINK_ADDR="198.51.100.1"
DST_ADDR="198.51.100.2"

GROUP="239.192.60.1"
PORT="5000"
MARKER_BEFORE="omcproxy-dataplane-before"
MARKER_AFTER="omcproxy-dataplane-after"

LOG="$(mktemp)"
RECEIVED="$(mktemp)"
RECEIVED_AFTER="$(mktemp)"
RECV_ERR="$(mktemp)"
PID=""
RECV_PID=""

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

log_size()
{
    stat -c %s "$LOG" 2>/dev/null || echo 0
}

wait_for_log_after()
{
    local offset="$1"
    local pattern="$2"
    local timeout=100

    while [ "$timeout" -gt 0 ]; do
        if tail -c "+$((offset + 1))" "$LOG" 2>/dev/null | grep -Fq "$pattern"; then
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

    if [ -n "$RECV_PID" ] && kill -0 "$RECV_PID" 2>/dev/null; then
        kill "$RECV_PID" 2>/dev/null || true
        wait "$RECV_PID" 2>/dev/null || true
    fi

    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        kill "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
    fi

    ip link del "$SRC" 2>/dev/null || true
    ip link del "$DST" 2>/dev/null || true
    ip link del "$UPLINK" 2>/dev/null || true
    ip link del "$DOWNLINK" 2>/dev/null || true

    echo
    echo "=== received data before link cycle ==="
    cat "$RECEIVED" 2>/dev/null || true

    echo
    echo "=== received data after link cycle ==="
    cat "$RECEIVED_AFTER" 2>/dev/null || true

    echo
    echo "=== socat receiver errors ==="
    cat "$RECV_ERR" 2>/dev/null || true

    echo
    echo "=== omcproxy log ==="
    cat "$LOG" 2>/dev/null || true

    rm -f "$LOG" "$RECEIVED" "$RECEIVED_AFTER" "$RECV_ERR"
}

trap cleanup EXIT INT TERM

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

get_ifindex()
{
    local dev="$1"

    ip -o link show dev "$dev" |
        awk -F': ' '{print $1}'
}

start_receiver()
{
    local output="$1"

    timeout 8 socat -u \
        "UDP4-RECVFROM:$PORT,reuseaddr,ip-add-membership=$GROUP:$DST_ADDR" \
        - >"$output" 2>>"$RECV_ERR" &
    RECV_PID=$!
}

wait_receiver()
{
    if [ -n "$RECV_PID" ]; then
        wait "$RECV_PID" 2>/dev/null || true
        RECV_PID=""
    fi
}

send_burst()
{
    local marker="$1"

    {
        for i in $(seq 1 20); do
            printf '%s-%02d\n' "$marker" "$i"
            sleep 0.1
        done
    } | socat -u STDIO \
        "UDP4-DATAGRAM:$GROUP:$PORT,ip-multicast-if=$SRC_ADDR,ip-multicast-ttl=16,ip-multicast-loop=0" ||
        fail "socat sender failed"
}

set_dataplane_sysctls()
{
    local key

    # The sender is in the same network namespace as the proxy, so its source
    # address is local to another interface. Allow that for this test.
    for key in \
        net.ipv4.conf.all.rp_filter \
        net.ipv4.conf.default.rp_filter \
        net.ipv4.conf."$UPLINK".rp_filter \
        net.ipv4.conf."$DOWNLINK".rp_filter \
        net.ipv4.conf."$SRC".rp_filter \
        net.ipv4.conf."$DST".rp_filter; do
        sysctl -w "$key=0" >/dev/null 2>&1 || true
    done

    for key in \
        net.ipv4.conf.all.accept_local \
        net.ipv4.conf.default.accept_local \
        net.ipv4.conf."$UPLINK".accept_local \
        net.ipv4.conf."$DOWNLINK".accept_local \
        net.ipv4.conf."$SRC".accept_local \
        net.ipv4.conf."$DST".accept_local; do
        sysctl -w "$key=1" >/dev/null 2>&1 || true
    done
}

echo "=== omcproxy multicast data-plane test ==="

[ -x "$OMCPROXY" ] ||
    fail "omcproxy is not executable: $OMCPROXY"

command -v socat >/dev/null 2>&1 ||
    fail "socat is required but not installed"

echo "Cleaning up any stale test interfaces"

ip link del "$SRC" 2>/dev/null || true
ip link del "$DST" 2>/dev/null || true
ip link del "$UPLINK" 2>/dev/null || true
ip link del "$DOWNLINK" 2>/dev/null || true

echo "Creating veth pairs"

ip link add "$SRC" type veth peer name "$UPLINK" ||
    fail "could not create source/uplink veth pair"

ip link add "$DST" type veth peer name "$DOWNLINK" ||
    fail "could not create receiver/downlink veth pair"

ip addr add "$UPLINK_ADDR/24" dev "$UPLINK" ||
    fail "could not assign uplink address"

ip addr add "$SRC_ADDR/24" dev "$SRC" ||
    fail "could not assign source address"

ip addr add "$DOWNLINK_ADDR/24" dev "$DOWNLINK" ||
    fail "could not assign downlink address"

ip addr add "$DST_ADDR/24" dev "$DST" ||
    fail "could not assign receiver address"

ip link set "$UPLINK" up
ip link set "$SRC" up
ip link set "$DOWNLINK" up
ip link set "$DST" up

set_dataplane_sysctls

UPLINK_RPF="$(cat "/proc/sys/net/ipv4/conf/$UPLINK/rp_filter" 2>/dev/null || echo unknown)"
UPLINK_ACCEPT_LOCAL="$(cat "/proc/sys/net/ipv4/conf/$UPLINK/accept_local" 2>/dev/null || echo unknown)"
echo "uplink rp_filter=$UPLINK_RPF accept_local=$UPLINK_ACCEPT_LOCAL"

ip route replace 239.0.0.0/8 dev "$SRC" ||
    fail "could not add multicast route via $SRC"

UPLINK_IFINDEX="$(get_ifindex "$UPLINK")"
[ -n "$UPLINK_IFINDEX" ] ||
    fail "could not determine uplink ifindex"

DOWNLINK_IFINDEX="$(get_ifindex "$DOWNLINK")"
[ -n "$DOWNLINK_IFINDEX" ] ||
    fail "could not determine downlink ifindex"

CLIENT_PATTERN="client_set: ::ffff:$GROUP on $UPLINK_IFINDEX => exclude"
QUERY_PATTERN="querier_iface_timer: sending generic IGMP-query on $DOWNLINK_IFINDEX"

echo "Starting omcproxy on $UPLINK -> $DOWNLINK (scope=organization)"

"$OMCPROXY" -v "$UPLINK,$DOWNLINK,scope=organization" >"$LOG" 2>&1 &
PID=$!

wait_for_log "$QUERY_PATTERN" ||
    fail "omcproxy did not attach the downlink"

echo "PASS: omcproxy attached downlink ifindex $DOWNLINK_IFINDEX"

echo "Starting multicast receiver on $DST ($GROUP)"

start_receiver "$RECEIVED"

wait_for_log "$CLIENT_PATTERN" ||
    fail "omcproxy did not learn the downlink multicast membership"

echo "PASS: omcproxy learned downlink membership"

echo "Sending multicast data before link cycle to $GROUP:$PORT via $SRC"

send_burst "$MARKER_BEFORE"
wait_receiver

grep -q "$MARKER_BEFORE-" "$RECEIVED" ||
    fail "no multicast data reached the receiver before link cycle (check rp_filter/accept_local; run via tests/run-docker-test.sh)"

BEFORE_COUNT="$(grep -c "$MARKER_BEFORE-" "$RECEIVED" || true)"
echo "PASS: receiver got $BEFORE_COUNT forwarded packet(s) before link cycle"

echo
echo "Bringing $UPLINK down and up"

LOG_OFFSET="$(log_size)"
ip link set "$UPLINK" down ||
    fail "could not bring $UPLINK down"

wait_for_log_after "$LOG_OFFSET" "proxy: detached uplink $UPLINK" ||
    fail "omcproxy did not detach $UPLINK after link down"

echo "PASS: omcproxy detached $UPLINK after link down"

LOG_OFFSET="$(log_size)"
ip link set "$UPLINK" up ||
    fail "could not bring $UPLINK up"

wait_for_log_after "$LOG_OFFSET" "proxy: attached uplink $UPLINK" ||
    fail "omcproxy did not reattach $UPLINK after link up"

wait_for_log_after "$LOG_OFFSET" "$QUERY_PATTERN" ||
    fail "omcproxy did not reattach the downlink after link cycle"

echo "PASS: omcproxy reattached $UPLINK and downlink ifindex $DOWNLINK_IFINDEX"

ip route replace 239.0.0.0/8 dev "$SRC" ||
    fail "could not restore multicast route via $SRC"

echo "Starting multicast receiver again on $DST ($GROUP)"

LOG_OFFSET="$(log_size)"
start_receiver "$RECEIVED_AFTER"

wait_for_log_after "$LOG_OFFSET" "$CLIENT_PATTERN" ||
    fail "omcproxy did not relearn the downlink multicast membership after link cycle"

echo "PASS: omcproxy relearned downlink membership after link cycle"

echo "Sending multicast data after link cycle to $GROUP:$PORT via $SRC"

send_burst "$MARKER_AFTER"
wait_receiver

grep -q "$MARKER_AFTER-" "$RECEIVED_AFTER" ||
    fail "no multicast data reached the receiver after link cycle"

AFTER_COUNT="$(grep -c "$MARKER_AFTER-" "$RECEIVED_AFTER" || true)"
echo "PASS: receiver got $AFTER_COUNT forwarded packet(s) after link cycle"

kill -0 "$PID" 2>/dev/null ||
    fail "omcproxy died during the data-plane test"

echo "PASS: omcproxy is still running"

echo
echo "======================================"
echo "DATA-PLANE TEST PASSED"
echo "======================================"

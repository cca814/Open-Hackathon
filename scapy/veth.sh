#!/usr/bin/env bash
# Create or remove the veth pairs for the single-host scapy test.
# BMv2 attaches to the sw-* ends; client.py, server.py and the CPU-port
# controller use the other ends.
set -euo pipefail

PAIRS=(
    "sw-p1 node-a"   # BMv2 port 1, client
    "sw-p2 node-b"   # BMv2 port 2, server
    "sw-p3 node-c"   # BMv2 port 3, optional multicast receiver
    "sw-cpu ctl-cpu" # BMv2 port 510, controller
)

up() {
    for pair in "${PAIRS[@]}"; do
        read -r sw host <<<"$pair"
        ip link show "$sw" >/dev/null 2>&1 || ip link add "$sw" type veth peer name "$host"
        for dev in "$sw" "$host"; do
            # No IPv6 router/neighbor chatter on the test links.
            sysctl -qw "net.ipv6.conf.$dev.disable_ipv6=1" 2>/dev/null || true
            ip link set dev "$dev" up
        done
    done
    echo "ready: simple_switch -i 1@sw-p1 -i 2@sw-p2 -i 3@sw-p3 -i 510@sw-cpu build/main.json"
}

down() {
    for pair in "${PAIRS[@]}"; do
        read -r sw _ <<<"$pair"
        ip link del "$sw" 2>/dev/null || true
    done
}

case "${1:-}" in
up) up ;;
down) down ;;
*)
    echo "usage: $0 up|down" >&2
    exit 2
    ;;
esac

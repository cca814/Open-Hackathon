#!/bin/bash
# Network setup for the DPU baseline test: source and sinks on DPU1, relay on DPU2.
# Run on each DPU ARM (needs sudo): ./dpu-net.sh dpu1|dpu2 up|down|status [flows]
#
# The IPs and the extra SFs do not survive a reboot, so run `up` again afterwards.
# The OVS ports do survive (they live in the OVS database), and `up` is safe to repeat.
#
#   10.0.1.0/24: DPU1 enp3s0f0s0 .1 <-> p0 ... p0 <-> DPU2 enp3s0f0s0 .2   (Port 1 to 3)
#   10.0.2.0/24: DPU1 enp3s0f1s0 .1 <-> p1 ... p1 <-> DPU2 enp3s0f1s0 .2   (Port 2 to 4)
#
# p0 and p1 have no RoCE GID table, so RDMA runs on the SF netdevs. DPU1 already
# bridges each SF representor with p0/p1 in OVS; DPU2 has to add p0/p1 itself.
#
# Once a process has a QP on an SF, other processes cannot create a CQ on it, so
# each RDMA process needs its own SF. DPU1 runs the source and three sinks, which
# takes four SFs: the two above plus two more on PF1 for the other sinks
# (10.0.2.3 and 10.0.2.4). DPU2 runs one relay process on its two SFs.
#
# `flows` adds what flows.sh needs for three parallel flows, each with its own
# source, relay and sink process (the sink serves all of a flow's receivers):
#   DPU1: two more SFs on PF0 for the sources (10.0.1.3, 10.0.1.4); the sinks use
#         the three on PF1.
#   DPU2: two more SFs on PF0 (10.0.1.5, 10.0.1.6) and two on PF1 (10.0.2.5,
#         10.0.2.6) for the relays.
# `down` and `status` always cover them, with or without `flows`.
set -euo pipefail

usage() { echo "usage: $0 dpu1|dpu2 up|down|status [flows]" >&2; exit 2; }
[ $# -eq 2 ] || [ $# -eq 3 ] || usage
role=$1
action=$2
flows=${3:-}
[ -z "$flows" ] || [ "$flows" = flows ] || usage

case $role in
    dpu1) ip0=10.0.1.1 ip1=10.0.2.1 ;;
    dpu2) ip0=10.0.1.2 ip1=10.0.2.2 ;;
    *) usage ;;
esac
sf0=enp3s0f0s0
sf1=enp3s0f1s0

# The extra SFs, one per line: PF, SF number, IP, MAC. Both DPUs are on the same
# L2 segment, so no two SFs of either may share a MAC.
base_extras() {
    if [ "$role" = dpu1 ]; then
        cat <<EOF
1 1 10.0.2.3 02:7e:6d:fd:79:01
1 2 10.0.2.4 02:7e:6d:fd:79:02
EOF
    fi
}
flow_extras() {
    if [ "$role" = dpu1 ]; then
        cat <<EOF
0 1 10.0.1.3 02:7e:6d:fd:7a:01
0 2 10.0.1.4 02:7e:6d:fd:7a:02
EOF
    else
        cat <<EOF
0 1 10.0.1.5 02:7e:6d:fd:7c:01
0 2 10.0.1.6 02:7e:6d:fd:7c:02
1 1 10.0.2.5 02:7e:6d:fd:7b:01
1 2 10.0.2.6 02:7e:6d:fd:7b:02
EOF
    fi
}
wanted_extras() { base_extras; if [ -n "$flows" ]; then flow_extras; fi; }
all_extras() { base_extras; flow_extras; }

sf_dev() { echo enp3s0f$1s$2; }          # SF netdev, from PF and SF number
sf_rep() { echo en3f$1pf$1sf$2; }        # its representor, bridged in ovsbr<PF+1>
sf_bridge() { echo ovsbr$(($1 + 1)); }
sf_pci() { echo 0000:03:00.$1; }
# mlnx-sf deletes by the full index, e.g. pci/0000:03:00.1/294945 (empty if absent).
sf_index() {
    sudo devlink port show | awk -v p=$1 -v n=$2 '$0 ~ "pfnum " p " sfnum " n " " { sub(/:$/, "", $1); print $1 }'
}

wait_for_netdev() {
    for _ in $(seq 20); do
        [ -e /sys/class/net/$1 ] && return 0
        sleep 0.5
    done
    echo "$1 did not appear" >&2
    exit 1
}

up() {
    for dev in $sf0 $sf1; do
        [ -e /sys/class/net/$dev ] || { echo "$dev not found" >&2; exit 1; }
    done
    if [ "$role" = dpu2 ]; then
        # Each IP lives on the SF only, so drop any copy left on the uplink.
        sudo ip addr del $ip0/24 dev p0 2>/dev/null || true
        sudo ip addr del $ip1/24 dev p1 2>/dev/null || true
        sudo ovs-vsctl --may-exist add-port ovsbr1 p0 -- --may-exist add-port ovsbr2 p1
    fi
    # A new SF netdev can come up administratively down, and then its IP is
    # unreachable (seen once on the second extra SF), so bring every one up.
    sudo ip link set $sf0 up
    sudo ip link set $sf1 up
    sudo ip addr replace $ip0/24 dev $sf0
    sudo ip addr replace $ip1/24 dev $sf1
    devs="$sf0 $sf1"
    # Descriptor 3 carries the list, so no command in the loop can eat it.
    while read -r pf n ip mac <&3; do
        dev=$(sf_dev $pf $n)
        rep=$(sf_rep $pf $n)
        if ! sudo devlink port show | grep -q "pfnum $pf sfnum $n "; then
            sudo mlnx-sf --action create --device $(sf_pci $pf) --sfnum $n --hwaddr $mac
        fi
        wait_for_netdev $dev
        wait_for_netdev $rep
        sudo ovs-vsctl --may-exist add-port $(sf_bridge $pf) $rep
        sudo ip link set $dev up
        sudo ip addr replace $ip/24 dev $dev
        devs="$devs $dev"
    done 3< <(wanted_extras)
    if [ "$role" = dpu1 ] || [ -n "$flows" ]; then
        # Several IPs share a subnet on this host. By default the first SF answers
        # ARP for all of them, so every target resolves to its MAC and RoCE reaches
        # the wrong SF (ping still works, because the host answers for any local IP).
        for dev in $devs; do
            sudo sysctl -qw net.ipv4.conf.$dev.arp_ignore=1 net.ipv4.conf.$dev.arp_announce=2
        done
    fi
    for dev in $devs; do
        sudo ip neigh flush dev $dev
    done
}

down() {
    while read -r pf n ip mac <&3; do
        sudo ip addr del $ip/24 dev $(sf_dev $pf $n) 2>/dev/null || true
        sudo ovs-vsctl --if-exists del-port $(sf_bridge $pf) $(sf_rep $pf $n)
        index=$(sf_index $pf $n)
        [ -z "$index" ] || sudo mlnx-sf --action delete --device $(sf_pci $pf) --sfindex "$index"
    done 3< <(all_extras)
    sudo ip addr del $ip0/24 dev $sf0 2>/dev/null || true
    sudo ip addr del $ip1/24 dev $sf1 2>/dev/null || true
    for dev in $sf0 $sf1; do
        sudo sysctl -qw net.ipv4.conf.$dev.arp_ignore=0 net.ipv4.conf.$dev.arp_announce=0
    done
    if [ "$role" = dpu2 ]; then
        sudo ovs-vsctl --if-exists del-port ovsbr1 p0 -- --if-exists del-port ovsbr2 p1
    fi
}

status() {
    devs="$sf0 $sf1"
    while read -r pf n ip mac <&3; do
        devs="$devs $(sf_dev $pf $n)"
    done 3< <(all_extras)
    for dev in $devs; do
        if [ -e /sys/class/net/$dev ]; then ip -br addr show dev $dev; else echo "$dev absent"; fi
    done
    echo "ovsbr1: $(sudo ovs-vsctl list-ports ovsbr1 | tr '\n' ' ')"
    echo "ovsbr2: $(sudo ovs-vsctl list-ports ovsbr2 | tr '\n' ' ')"
    for dev in $devs; do
        [ -e /sys/class/net/$dev ] || continue
        ibdev=$(rdma link show | awk -v n=$dev '$NF == n { split($2, a, "/"); print a[1]; exit }')
        echo "$dev -> ${ibdev:-?}"
        ibv_devinfo -d "$ibdev" -v | grep -E 'GID\[.*::ffff:.*RoCE v2' || echo "  no IPv4 RoCE v2 GID"
    done
}

case $action in
    up) up; status ;;
    down) down; status ;;
    status) status ;;
    *) usage ;;
esac

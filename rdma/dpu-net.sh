#!/bin/bash
# Network setup for the DPU baseline test: source and sinks on DPU1, relay on DPU2.
# Run on each DPU ARM (needs sudo): ./dpu-net.sh dpu1|dpu2 up|down|status
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
set -euo pipefail

usage() { echo "usage: $0 dpu1|dpu2 up|down|status" >&2; exit 2; }
[ $# -eq 2 ] || usage
role=$1
action=$2

case $role in
    dpu1) ip0=10.0.1.1 ip1=10.0.2.1 ;;
    dpu2) ip0=10.0.1.2 ip1=10.0.2.2 ;;
    *) usage ;;
esac
sf0=enp3s0f0s0
sf1=enp3s0f1s0

extra_sfs=
[ "$role" = dpu1 ] && extra_sfs="1 2"
pf1=0000:03:00.1
extra_dev() { echo enp3s0f1s$1; }     # SF netdev
extra_rep() { echo en3f1pf1sf$1; }    # its representor, bridged in ovsbr2
extra_ip() { echo 10.0.2.$(($1 + 2)); }
# mlnx-sf deletes by the full index, e.g. pci/0000:03:00.1/294945 (empty if absent).
extra_index() {
    sudo devlink port show | awk -v n=$1 '$0 ~ "pfnum 1 sfnum " n " " { sub(/:$/, "", $1); print $1 }'
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
    sudo ip addr replace $ip0/24 dev $sf0
    sudo ip addr replace $ip1/24 dev $sf1
    for n in $extra_sfs; do
        if ! sudo devlink port show | grep -q "pfnum 1 sfnum $n "; then
            sudo mlnx-sf --action create --device $pf1 --sfnum $n --hwaddr 02:7e:6d:fd:79:0$n
        fi
        wait_for_netdev $(extra_dev $n)
        wait_for_netdev $(extra_rep $n)
        sudo ovs-vsctl --may-exist add-port ovsbr2 $(extra_rep $n)
        sudo ip addr replace $(extra_ip $n)/24 dev $(extra_dev $n)
    done
    if [ "$role" = dpu1 ]; then
        # Several IPs share 10.0.2.0/24 on this host. By default the first SF answers
        # ARP for all of them, so every target resolves to its MAC and RoCE reaches
        # the wrong SF (ping still works, because the host answers for any local IP).
        for dev in $sf0 $sf1 $(for n in $extra_sfs; do extra_dev $n; done); do
            sudo sysctl -qw net.ipv4.conf.$dev.arp_ignore=1 net.ipv4.conf.$dev.arp_announce=2
        done
    fi
    sudo ip neigh flush dev $sf0
    sudo ip neigh flush dev $sf1
}

down() {
    for n in $extra_sfs; do
        sudo ip addr del $(extra_ip $n)/24 dev $(extra_dev $n) 2>/dev/null || true
        sudo ovs-vsctl --if-exists del-port ovsbr2 $(extra_rep $n)
        index=$(extra_index $n)
        [ -z "$index" ] || sudo mlnx-sf --action delete --device $pf1 --sfindex "$index"
    done
    sudo ip addr del $ip0/24 dev $sf0 2>/dev/null || true
    sudo ip addr del $ip1/24 dev $sf1 2>/dev/null || true
    if [ "$role" = dpu1 ]; then
        for dev in $sf0 $sf1; do
            sudo sysctl -qw net.ipv4.conf.$dev.arp_ignore=0 net.ipv4.conf.$dev.arp_announce=0
        done
    fi
    if [ "$role" = dpu2 ]; then
        sudo ovs-vsctl --if-exists del-port ovsbr1 p0 -- --if-exists del-port ovsbr2 p1
    fi
}

status() {
    devs="$sf0 $sf1"
    for n in $extra_sfs; do devs="$devs $(extra_dev $n)"; done
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

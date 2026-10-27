#!/bin/sh
set -eu

# Node A lab defaults. Override these variables when the SF allocation changes.
bridge_sender=${BRIDGE_SENDER:-ovsbr1}
bridge_receiver=${BRIDGE_RECEIVER:-ovsbr2}
sender_rep=${SENDER_REP:-en3f0pf0sf0}
receiver_rep0=${RECEIVER_REP0:-en3f1pf1sf0}
receiver_rep1=${RECEIVER_REP1:-en3f1pf1sf1}
sender_mac=${SENDER_MAC:-02:96:42:f8:5e:f2}
receiver_mac0=${RECEIVER_MAC0:-02:7e:6d:fd:79:7c}
receiver_mac1=${RECEIVER_MAC1:-02:7e:6d:fd:79:01}
physical_sender=${PHYSICAL_SENDER:-p0}
physical_receiver=${PHYSICAL_RECEIVER:-p1}
cookie=0x49435243

action=${1:-}
case "$action" in
    setup|recover|show) ;;
    *) echo "Usage: $0 {setup|recover|show}" >&2; exit 2 ;;
esac

show_flows()
{
    ovs-ofctl -O OpenFlow13 dump-flows "$bridge_sender" "cookie=$cookie/-1"
    ovs-ofctl -O OpenFlow13 dump-flows "$bridge_receiver" "cookie=$cookie/-1"
}

if [ "$action" = recover ]; then
    ovs-ofctl -O OpenFlow13 del-flows "$bridge_sender" "cookie=$cookie/-1"
    ovs-ofctl -O OpenFlow13 del-flows "$bridge_receiver" "cookie=$cookie/-1"
    show_flows
    exit 0
fi

if [ "$action" = show ]; then
    show_flows
    exit 0
fi

if [ "$(ovs-vsctl get Open_vSwitch . other_config:hw-offload)" != '"true"' ]; then
    echo "OVS hardware offload is not enabled" >&2
    exit 1
fi

ofport()
{
    value=$(ovs-vsctl get Interface "$1" ofport)
    case "$value" in
        ''|*[!0-9]*) echo "invalid OVS ofport for $1: $value" >&2; exit 1 ;;
    esac
    if [ "$value" -le 0 ]; then
        echo "inactive OVS ofport for $1: $value" >&2
        exit 1
    fi
    printf '%s' "$value"
}

sender_rep_port=$(ofport "$sender_rep")
receiver_rep0_port=$(ofport "$receiver_rep0")
receiver_rep1_port=$(ofport "$receiver_rep1")
p0_port=$(ofport "$physical_sender")
p1_port=$(ofport "$physical_receiver")

# Replace only this project's rules. The priority-0 NORMAL rules remain intact.
ovs-ofctl -O OpenFlow13 del-flows "$bridge_sender" "cookie=$cookie/-1"
ovs-ofctl -O OpenFlow13 del-flows "$bridge_receiver" "cookie=$cookie/-1"

# Forward SEND: the dedicated sender SF -> p0, then p1 -> the selected
# receiver SF. Keep this layer-2-only: OVS need not parse RoCE or IP.
ovs-ofctl -O OpenFlow13 add-flow "$bridge_sender" \
    "cookie=$cookie,priority=200,in_port=$sender_rep_port,actions=output:$p0_port"
ovs-ofctl -O OpenFlow13 add-flow "$bridge_receiver" \
    "cookie=$cookie,priority=200,in_port=$p1_port,dl_dst=$receiver_mac0,actions=output:$receiver_rep0_port"
ovs-ofctl -O OpenFlow13 add-flow "$bridge_receiver" \
    "cookie=$cookie,priority=200,in_port=$p1_port,dl_dst=$receiver_mac1,actions=output:$receiver_rep1_port"

# RC ACK return: either receiver SF -> p1, then p0 -> the sender SF.
ovs-ofctl -O OpenFlow13 add-flow "$bridge_receiver" \
    "cookie=$cookie,priority=200,in_port=$receiver_rep0_port,actions=output:$p1_port"
ovs-ofctl -O OpenFlow13 add-flow "$bridge_receiver" \
    "cookie=$cookie,priority=200,in_port=$receiver_rep1_port,actions=output:$p1_port"
ovs-ofctl -O OpenFlow13 add-flow "$bridge_sender" \
    "cookie=$cookie,priority=200,in_port=$p0_port,dl_dst=$sender_mac,actions=output:$sender_rep_port"

show_flows

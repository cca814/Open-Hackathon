#!/bin/bash
# Message-size sweep for the DPU baseline: three sinks and the source on DPU1,
# the relay on DPU2 (see CLAUDE.md for the layout and RELAY.md for the options).
#
# Run it from any machine that can `ssh DPU1` and `ssh DPU2` without a prompt,
# with the programs built in ~/baseline/rdma on both and dpu-net.sh already `up`.
#
#   ./bench.sh                         default sweep, 3 runs per size
#   ./bench.sh -r 1 1024:20000         one run of 20000 x 1 KB messages
#   ./bench.sh --no-verify 16384:100000
#   ./bench.sh -o results.txt          also append each line, with a timestamp, to a file
#
# Each argument is SIZE[:COUNT]. One line is printed per run.
set -uo pipefail

SRC_HOST=${SRC_HOST:-DPU1}
RELAY_HOST=${RELAY_HOST:-DPU2}
dir=baseline      # directory under ~ on both hosts that holds the built programs
reps=3
pool=64           # pool == window makes the sender wait on RNR retries, so keep it larger
window=16
verify=--verify
warmup=1
out=              # file to append the result lines to; empty means screen only

# Devices and IPs set up by dpu-net.sh. Each RDMA process needs its own SF.
SRC_DEV=mlx5_2          SRC_IP_TO_RELAY=10.0.1.2
RELAY_IN_DEV=mlx5_2     RELAY_OUT_DEV=mlx5_3
SINK_DEVS=(mlx5_3 mlx5_4 mlx5_5)
SINK_IPS=(10.0.2.1 10.0.2.3 10.0.2.4)
SINK_PORTS=(10011 10012 10013)
GID=1

DEFAULT_SIZES="64:500000 256:500000 1024:300000 4096:200000 16384:100000 65536:30000"

usage() {
    echo "usage: $0 [-r REPS] [-p POOL] [-w WINDOW] [-d DIR] [-o FILE] [--no-verify] [--no-warmup] [SIZE[:COUNT]...]" >&2
    exit 2
}

while [ $# -gt 0 ]; do
    case $1 in -r|-p|-w|-d|-o) [ $# -ge 2 ] || usage ;; esac
    case $1 in
        -r) reps=$2; shift 2 ;;
        -p) pool=$2; shift 2 ;;
        -w) window=$2; shift 2 ;;
        -d) dir=$2; shift 2 ;;
        -o) out=$2; shift 2 ;;
        --no-verify) verify=; shift ;;
        --no-warmup) warmup=0; shift ;;
        -h|--help) usage ;;
        -*) usage ;;
        *) break ;;
    esac
done
sizes=${*:-$DEFAULT_SIZES}

# stdin must not reach ssh, or it eats the rest of any loop that is reading it.
remote() { local host=$1; shift; ssh -o BatchMode=yes "$host" "$@" < /dev/null; }

stop_all() {
    remote $SRC_HOST 'pkill -u $USER -x sink; pkill -u $USER -x source; true'
    remote $RELAY_HOST 'pkill -u $USER -x relay; true'
}
trap stop_all EXIT

# $1 size, $2 count; prints one summary line.
run_once() {
    local size=$1 count=$2 msg i
    msg=$((size + 64)); [ $msg -lt 4096 ] && msg=4096

    stop_all
    # A background command ends with `&`, so the commands are joined with spaces.
    local sinks="cd ~/$dir/rdma; rm -f sink*.log;"
    local targets=""
    for i in 0 1 2; do
        sinks+=" nohup ./sink --device ${SINK_DEVS[$i]} --gid-index $GID --port ${SINK_PORTS[$i]} --count $count $verify --msg-size $msg --pool $pool > sink${SINK_PORTS[$i]}.log 2>&1 < /dev/null &"
        targets+=" --target ${SINK_IPS[$i]}:${SINK_PORTS[$i]}"
    done
    remote $SRC_HOST "$sinks sleep 1"
    remote $RELAY_HOST "cd ~/$dir/rdma; rm -f relay.log; nohup ./relay --in-device $RELAY_IN_DEV --in-gid $GID --out-device $RELAY_OUT_DEV --out-gid $GID --msg-size $msg --pool $pool $targets --count $count > relay.log 2>&1 < /dev/null & sleep 1"

    local src rel snk
    src=$(remote $SRC_HOST "cd ~/$dir/rdma; timeout 300 ./source --device $SRC_DEV --gid-index $GID --server-ip $SRC_IP_TO_RELAY --count $count --size $size --window $window 2>&1 | tail -2")
    sleep 2
    rel=$(remote $RELAY_HOST "cd ~/$dir/rdma; grep Forwarded relay.log")
    snk=$(remote $SRC_HOST "cd ~/$dir/rdma; cat sink*.log")

    local s_time s_gbps r_time r_in r_out rates ok
    read -r s_time s_gbps <<< "$(sed -n 's/.* in \([0-9.]*\) s: \([0-9.]*\) Gbps.*/\1 \2/p' <<< "$src" | tail -1)"
    read -r r_time r_in r_out <<< "$(sed -n 's/.*target(s) in \([0-9.]*\) s: \([0-9.]*\) Gbps in, \([0-9.]*\) Gbps out.*/\1 \2 \3/p' <<< "$rel" | tail -1)"
    rates=$(sed -n 's/^Received [0-9]* messages, [0-9]* bytes in [0-9.]* s: \([0-9.]*\) Gbps.*/\1/p' <<< "$snk" | paste -sd, -)
    if [ -n "$verify" ]; then
        ok="$(grep -c 'Verify: 0 corrupted, 0 out of order' <<< "$snk")/3"
    else
        ok=off
    fi
    local mps=NA
    [ -n "$s_time" ] && mps=$(awk -v c="$count" -v t="$s_time" 'BEGIN { printf "%.3f", c / t / 1e6 }')

    echo "size=$size count=$count pool=$pool window=$window dir=$dir | source ${s_gbps:-NA} Gbps, $mps Mmsg/s, ${s_time:-NA} s | relay in ${r_in:-NA} out ${r_out:-NA} Gbps | sinks ${rates:-NA} Gbps | verify ${ok}"
}

if [ "$warmup" = 1 ]; then
    # The first traffic over a new flow is slow while OVS offloads it.
    run_once 1024 5000 > /dev/null
fi

for spec in $sizes; do
    size=${spec%%:*}
    count=${spec#*:}
    [ "$count" = "$spec" ] && count=100000
    for ((rep = 1; rep <= reps; rep++)); do
        line=$(run_once "$size" "$count")
        echo "$line"
        # The warm-up above is not logged, only the measured runs.
        [ -z "$out" ] || echo "$(date -Iseconds) | $line" >> "$out"
    done
done

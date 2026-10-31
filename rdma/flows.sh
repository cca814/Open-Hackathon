#!/bin/bash
# Several flows at once, each with several receivers, for the DPU baseline (see
# CLAUDE.md for the layout and RELAY.md for the options).
#
# Every flow has its own source, relay and sink process. A source on DPU1 sends to
# its relay on DPU2, the relay sends every message to R receivers, and one sink
# process on DPU1 serves all R of them, one QP each. The flows run at the same
# time. Needs `dpu-net.sh dpu1 up flows` and `dpu-net.sh dpu2 up flows`, and the
# programs built in ~/baseline_flows/rdma on both DPUs.
#
# Run it from any machine that can `ssh DPU1` and `ssh DPU2` without a prompt.
#
#   ./flows.sh -f 3 -R 5 64:10000000        3 flows of 5 receivers, 10 million 64-byte messages each
#   ./flows.sh -f 1 -R 5 -r 3 -o results.txt 1024:10000000 4096:10000000
#   ./flows.sh -f 3 -R 5 16384:100000       SIZE[:COUNT], one line is printed per run
set -uo pipefail

SRC_HOST=${SRC_HOST:-DPU1}
RELAY_HOST=${RELAY_HOST:-DPU2}
dir=baseline_flows    # directory under ~ on both hosts that holds the built programs
flows=1
receivers=5
reps=1
pool=64               # keep it larger than the window, see bench.sh
window=1
verify=--verify
warmup=1
out=                  # file to append the result lines to; empty means screen only
GID=1

# Per flow, the SF netdev that each process uses (every process needs its own SF)
# and the IPs dpu-net.sh gives them. The ibdev is looked up from the netdev.
SRC_NETDEV=(enp3s0f0s0 enp3s0f0s1 enp3s0f0s2);       SRC_IP=(10.0.1.1 10.0.1.3 10.0.1.4)
SINK_NETDEV=(enp3s0f1s0 enp3s0f1s1 enp3s0f1s2);      SINK_IP=(10.0.2.1 10.0.2.3 10.0.2.4)
RELAY_IN_NETDEV=(enp3s0f0s0 enp3s0f0s1 enp3s0f0s2);  RELAY_IN_IP=(10.0.1.2 10.0.1.5 10.0.1.6)
RELAY_OUT_NETDEV=(enp3s0f1s0 enp3s0f1s1 enp3s0f1s2); RELAY_OUT_IP=(10.0.2.2 10.0.2.5 10.0.2.6)
RELAY_PORT=10001      # the relay of flow f listens on RELAY_PORT + f
SINK_PORT=10100       # the sink of flow f listens on SINK_PORT + 10 f, one port per receiver

usage() {
    echo "usage: $0 [-f FLOWS] [-R RECEIVERS] [-r REPS] [-p POOL] [-w WINDOW] [-d DIR] [-o FILE] [--no-verify] [--no-warmup] [SIZE[:COUNT]...]" >&2
    exit 2
}

while [ $# -gt 0 ]; do
    case $1 in -f|-R|-r|-p|-w|-d|-o) [ $# -ge 2 ] || usage ;; esac
    case $1 in
        -f) flows=$2; shift 2 ;;
        -R) receivers=$2; shift 2 ;;
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
sizes=${*:-"64:10000000 256:10000000 1024:10000000 4096:10000000 16384:100000 65536:100000"}
case $flows in 1|2|3) ;; *) echo "-f must be 1, 2 or 3" >&2; exit 2 ;; esac
case $receivers in [1-9]|10) ;; *) echo "-R must be 1 to 10" >&2; exit 2 ;; esac

# stdin must not reach ssh, or it eats the rest of any loop that is reading it.
remote() { local host=$1; shift; ssh -o BatchMode=yes -o ServerAliveInterval=30 "$host" "$@" < /dev/null; }

ibdev_of() { remote "$1" "rdma link show" | awk -v n="$2" '$NF == n { split($2, a, "/"); print a[1]; exit }'; }
# GID index $GID has to hold the IPv4-mapped address: that is what the programs use.
want_gid() { local IFS=.; set -- $1; printf '0000:0000:0000:0000:0000:ffff:%02x%02x:%02x%02x' "$1" "$2" "$3" "$4"; }

# $1 label, $2 host, $3 netdev, $4 IP; sets REPLY to the ibdev.
resolve() {
    local dev gid
    dev=$(ibdev_of "$2" "$3")
    if [ -z "$dev" ]; then
        echo "$1: no ibdev for $3 on $2 (is dpu-net.sh up flows done?)" >&2; return 1
    fi
    gid=$(remote "$2" "cat /sys/class/infiniband/$dev/ports/1/gids/$GID" 2>&1)
    if [ "$gid" != "$(want_gid "$4")" ]; then
        echo "$1: GID $GID of $dev ($3) is '$gid', expected $(want_gid "$4") for $4" >&2; return 1
    fi
    REPLY=$dev
}

SRC_DEV=(); SINK_DEV=(); RELAY_IN_DEV=(); RELAY_OUT_DEV=()
for ((f = 0; f < flows; f++)); do
    resolve "source $f" "$SRC_HOST" "${SRC_NETDEV[$f]}" "${SRC_IP[$f]}" || exit 1;            SRC_DEV[$f]=$REPLY
    resolve "sink $f" "$SRC_HOST" "${SINK_NETDEV[$f]}" "${SINK_IP[$f]}" || exit 1;            SINK_DEV[$f]=$REPLY
    resolve "relay $f in" "$RELAY_HOST" "${RELAY_IN_NETDEV[$f]}" "${RELAY_IN_IP[$f]}" || exit 1;   RELAY_IN_DEV[$f]=$REPLY
    resolve "relay $f out" "$RELAY_HOST" "${RELAY_OUT_NETDEV[$f]}" "${RELAY_OUT_IP[$f]}" || exit 1; RELAY_OUT_DEV[$f]=$REPLY
done

stop_all() {
    remote $SRC_HOST 'pkill -u $USER -x sink; pkill -u $USER -x source; true'
    remote $RELAY_HOST 'pkill -u $USER -x relay; true'
}
trap stop_all EXIT

# "lo..hi" of the numbers on stdin, or just the number if they are all equal.
range() { awk 'NR == 1 { lo = hi = $1 } { if ($1 < lo) lo = $1; if ($1 > hi) hi = $1 } END { if (NR == 0) print "NA"; else if (lo == hi) print lo; else print lo ".." hi }'; }
# Values on stdin joined with commas, or NA.
commas() { paste -sd, - | grep . || echo NA; }

# $1 size, $2 count; prints one summary line.
run_once() {
    local size=$1 count=$2 msg f r cmd targets
    msg=$((size + 64)); [ $msg -lt 4096 ] && msg=4096

    stop_all
    # A background command ends with `&`, so the commands are joined with spaces.
    cmd="cd ~/$dir/rdma; rm -f flow*.log;"
    for ((f = 0; f < flows; f++)); do
        cmd+=" nohup ./sink --device ${SINK_DEV[$f]} --gid-index $GID --port $((SINK_PORT + 10 * f)) --receivers $receivers --count $count $verify --latency --msg-size $msg --pool $pool > flow${f}_sink.log 2>&1 < /dev/null &"
    done
    remote $SRC_HOST "$cmd sleep 1"

    cmd="cd ~/$dir/rdma; rm -f flow*.log;"
    for ((f = 0; f < flows; f++)); do
        targets=""
        for ((r = 0; r < receivers; r++)); do
            targets+=" --target ${SINK_IP[$f]}:$((SINK_PORT + 10 * f + r))"
        done
        cmd+=" nohup ./relay --in-device ${RELAY_IN_DEV[$f]} --in-gid $GID --out-device ${RELAY_OUT_DEV[$f]} --out-gid $GID --msg-size $msg --pool $pool $targets --listen-port $((RELAY_PORT + f)) --count $count --busy > flow${f}_relay.log 2>&1 < /dev/null &"
    done
    remote $RELAY_HOST "$cmd sleep 1"

    # All the sources start within a few milliseconds of each other, from one ssh.
    cmd="cd ~/$dir/rdma;"
    for ((f = 0; f < flows; f++)); do
        cmd+=" (timeout 3600 ./source --device ${SRC_DEV[$f]} --gid-index $GID --server-ip ${RELAY_IN_IP[$f]} --port $((RELAY_PORT + f)) --count $count --size $size --window $window > flow${f}_source.log 2>&1 &);"
    done
    remote $SRC_HOST "$cmd sleep 1; while pgrep -u \$USER -x source > /dev/null; do sleep 0.5; done"

    # The sinks and the relays print their result after the sources are done, and a
    # sink sorts its latency samples first, so wait for them to exit.
    remote $SRC_HOST 'for _ in $(seq 480); do pgrep -u $USER -x sink > /dev/null || break; sleep 0.5; done'
    remote $RELAY_HOST 'for _ in $(seq 480); do pgrep -u $USER -x relay > /dev/null || break; sleep 0.5; done'

    # Every line of every log is tagged with its flow number.
    local ids src rel snk
    ids=$(seq -s ' ' 0 $((flows - 1)))
    src=$(remote $SRC_HOST "cd ~/$dir/rdma; for f in $ids; do sed \"s/^/\$f /\" flow\${f}_source.log; done")
    snk=$(remote $SRC_HOST "cd ~/$dir/rdma; for f in $ids; do sed \"s/^/\$f /\" flow\${f}_sink.log; done")
    rel=$(remote $RELAY_HOST "cd ~/$dir/rdma; for f in $ids; do sed \"s/^/\$f /\" flow\${f}_relay.log; done")

    local s_times s_gbps s_max s_sum mps r_in r_out r_busy k_gbps ok p50 p99
    s_times=$(sed -n 's/^[0-9]* SEND completed: .* in \([0-9.]*\) s: .*/\1/p' <<< "$src")
    s_gbps=$(sed -n 's/^[0-9]* SEND completed: .* s: \([0-9.]*\) Gbps.*/\1/p' <<< "$src")
    s_max=$(sort -g <<< "$s_times" | tail -1)
    s_sum=$(awk '{ s += $1 } END { if (NR) printf "%.2f", s }' <<< "$s_gbps")
    mps=NA
    [ -n "$s_max" ] && [ "$(wc -l <<< "$s_times")" -eq "$flows" ] && mps=$(awk -v n="$flows" -v c="$count" -v t="$s_max" 'BEGIN { printf "%.3f", n * c / t / 1e6 }')
    r_in=$(sed -n 's/^[0-9]* Forwarded .* s: \([0-9.]*\) Gbps in, .*/\1/p' <<< "$rel" | commas)
    r_out=$(sed -n 's/^[0-9]* Forwarded .* Gbps in, \([0-9.]*\) Gbps out.*/\1/p' <<< "$rel" | commas)
    r_busy=$(sed -n 's/^[0-9]* Busy: \([0-9.]*\)%.*/\1/p' <<< "$rel" | commas)
    k_gbps=$(sed -n 's/^[0-9]* \(\[r[0-9]*\] \)*Received [0-9]* messages, [0-9]* bytes in [0-9.]* s: \([0-9.]*\) Gbps.*/\2/p' <<< "$snk" | range)
    if [ -n "$verify" ]; then
        ok="$(grep -c 'Verify: 0 corrupted, 0 out of order' <<< "$snk")/$((flows * receivers))"
    else
        ok=off
    fi
    p50=$(sed -n 's/^[0-9]* \(\[r[0-9]*\] \)*Latency: .* p50=\([0-9.]*\) .*/\2/p' <<< "$snk" | range)
    p99=$(sed -n 's/^[0-9]* \(\[r[0-9]*\] \)*Latency: .* p99=\([0-9.]*\) .*/\2/p' <<< "$snk" | range)

    echo "flows=$flows receivers=$receivers size=$size count=$count pool=$pool window=$window dir=$dir | source $mps Mmsg/s in all, ${s_max:-NA} s longest, ${s_sum:-NA} Gbps in all | relay in $r_in out $r_out Gbps | relay busy $r_busy % | sinks $k_gbps Gbps | verify $ok | latency p50 $p50 p99 $p99 us"
}

if [ "$warmup" = 1 ]; then
    # The first traffic over a new SF is slow while OVS offloads it.
    run_once 1024 20000 > /dev/null
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

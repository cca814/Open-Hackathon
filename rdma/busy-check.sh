#!/bin/bash
# Is anyone else using the DPUs? Read-only. `who` only lists logins with a terminal,
# so this looks at what the machines are doing instead: CPU, the physical ports and
# the RDMA devices. Run it from any machine that can `ssh DPU1` and `ssh DPU2`.
#
#   ./busy-check.sh                 sample both DPUs for 5 s; exit 0 if clear, 1 if busy
#   ./busy-check.sh -t 10 --cpu 30 --gbps 0.5
#   ./busy-check.sh counters        print the byte counters of p0 and p1 on both DPUs
#   ./busy-check.sh wire "$before" "$after" FLOWS RECEIVERS SIZE COUNT [MTU]
#                                   compare the counters' growth with what a run should send
#
# Busy means any of: a process that is not root's uses at least --cpu percent of a
# core (default 20), a physical port carries at least --gbps in either direction
# (default 0.1), or a process has an RDMA device open. A root process above --cpu is
# only a warning: it is a system job (apt, a service), not another user, though it
# disturbs a measurement just the same. The port counters see traffic from the
# hosts too, which never touches the ARM cores. A program that sleeps while idle,
# or one that runs only between two samples, is not seen.
set -uo pipefail

SRC_HOST=${SRC_HOST:-DPU1}
RELAY_HOST=${RELAY_HOST:-DPU2}

# The byte counters of the two uplinks, one line each: "p0 <rx> <tx>".
read -r -d '' PORTS_FN <<'EOS'
ports() { for p in p0 p1; do ethtool -S $p 2>/dev/null | awk -v p=$p '/ rx_bytes_phy:/ { r = $2 } / tx_bytes_phy:/ { t = $2 } END { print p, r + 0, t + 0 }'; done; }
EOS

# Runs on a DPU as `bash -s SECONDS`. ps shows a process's CPU over its whole life,
# so the CPU of each one is the growth of its time between two samples.
read -r -d '' SAMPLER <<'EOS'
secs=${1:-5}
tck=$(getconf CLK_TCK)
procs() { cat /proc/[0-9]*/stat 2>/dev/null | awk '{ s = $0; sub(/^.*\) /, "", s); split(s, f, " "); print $1, f[12] + f[13] }'; }
cores() { grep '^cpu[0-9]' /proc/stat; }
t0=$(date +%s.%N); P1=$(procs); C1=$(cores); N1=$(ports)
sleep "$secs"
t1=$(date +%s.%N); P2=$(procs); C2=$(cores); N2=$(ports)
dt=$(awk -v a="$t0" -v b="$t1" 'BEGIN { print b - a }')

# CORE <n> <busy percent>: everything except idle and iowait.
paste <(echo "$C1") <(echo "$C2") | awk '{
  t1 = 0; for (i = 2; i <= 9; i++) t1 += $i
  t2 = 0; for (i = 13; i <= 20; i++) t2 += $i
  d = t2 - t1; idle = ($16 + $17) - ($5 + $6)
  n = $1; sub(/^cpu/, "", n)
  printf "CORE %s %d\n", n, (d > 0) ? 100 * (d - idle) / d : 0 }'

# PROC <user> <pid> <percent of a core> <command>, for those at 5% or more.
tmp=$(mktemp)
awk -v dt="$dt" -v tck="$tck" 'FNR == NR { a[$1] = $2; next }
  ($1 in a) && $2 > a[$1] { printf "%s %.1f\n", $1, ($2 - a[$1]) * 100 / tck / dt }' <(echo "$P1") <(echo "$P2") > "$tmp"
ps -eo pid=,user:32=,comm= | awk 'FNR == NR { p[$1] = $2; next }
  ($1 in p) && p[$1] >= 5 { printf "PROC %s %s %s %s\n", $2, $1, p[$1], $3 }' "$tmp" -
rm -f "$tmp"

# PORT <name> <rx Gbps> <tx Gbps>
paste <(echo "$N1") <(echo "$N2") | awk -v dt="$dt" '{
  printf "PORT %s %.2f %.2f\n", $1, ($5 - $2) * 8 / dt / 1e9, ($6 - $3) * 8 / dt / 1e9 }'

# RDMA <user> <pid> <command>: whoever has a verbs device open.
if ls /dev/infiniband/uverbs* > /dev/null 2>&1; then
  if sudo -n true 2> /dev/null; then
    for pid in $(sudo -n fuser /dev/infiniband/uverbs* 2> /dev/null | tr ' ' '\n' | grep -E '^[0-9]+$' | sort -u); do
      ps -o user=,comm= -p "$pid" | awk -v p="$pid" '{ print "RDMA", $1, p, $2 }'
    done
  else
    echo RDMA_UNKNOWN
  fi
fi
EOS

ge() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a >= b) }'; }

# $1 host, $2 file with that host's samples; prints the verdict, returns 1 if busy.
report() {
    local host=$1 file=$2 kind a b c d core_max=0 ports="" reasons="" warns=""
    while read -r kind a b c d; do
        case $kind in
            PROC)
                if ge "$c" "$cpu"; then
                    if [ "$a" = root ]; then
                        warns+="  warning: root's $d (pid $b) uses $c% of a core (a system job; it still disturbs a run)"$'\n'
                    else
                        reasons+="  $a's $d (pid $b) uses $c% of a core"$'\n'
                    fi
                fi ;;
            CORE) ge "$b" "$core_max" && core_max=$b ;;
            PORT)
                ports+=" $a $b/$c"
                if ge "$b" "$gbps" || ge "$c" "$gbps"; then
                    reasons+="  $a carries $b Gbps in and $c Gbps out"$'\n'
                fi ;;
            RDMA) reasons+="  $a's $c (pid $b) has an RDMA device open"$'\n' ;;
            RDMA_UNKNOWN) warns+="  warning: no passwordless sudo, so the RDMA users were not checked"$'\n' ;;
        esac
    done < "$file"
    if [ -z "$reasons" ]; then
        echo "$host: clear (busiest core ${core_max}%,${ports:- no port counters} Gbps in/out)"
    else
        echo "$host: BUSY"
        printf '%s' "$reasons"
    fi
    printf '%s' "$warns"
    [ -z "$reasons" ]
}

check() {
    local secs=5 dir h rc=0
    cpu=20 gbps=0.1
    while [ $# -gt 0 ]; do
        case $1 in -t|--cpu|--gbps) [ $# -ge 2 ] || usage ;; esac
        case $1 in
            -t) secs=$2; shift 2 ;;
            --cpu) cpu=$2; shift 2 ;;
            --gbps) gbps=$2; shift 2 ;;
            *) usage ;;
        esac
    done
    dir=$(mktemp -d)
    trap "rm -rf '$dir'" EXIT   # the path now: $dir is local and gone when the trap runs
    # Both DPUs are sampled at the same time. The script goes in on stdin.
    for h in "$SRC_HOST" "$RELAY_HOST"; do
        ssh -o BatchMode=yes "$h" "bash -s $secs" <<< "$PORTS_FN"$'\n'"$SAMPLER" > "$dir/$h" 2> "$dir/$h.err" &
    done
    wait
    for h in "$SRC_HOST" "$RELAY_HOST"; do
        if ! grep -q '^CORE ' "$dir/$h"; then
            echo "$h: could not sample ($(head -1 "$dir/$h.err"))" >&2
            return 2
        fi
        report "$h" "$dir/$h" || rc=1
    done
    return $rc
}

# "<host> p0 <rx> <tx> p1 <rx> <tx>" for each DPU.
counters() {
    local h
    for h in "$SRC_HOST" "$RELAY_HOST"; do
        echo "$h $(ssh -o BatchMode=yes "$h" "$PORTS_FN; ports" < /dev/null | tr '\n' ' ')"
    done
}

# What a run should put on the links, from the message size, the count and the path
# MTU: per packet an Ethernet, IP, UDP and base transport header and the ICRC (58
# bytes, the Ethernet checksum not counted), and 62 bytes for each ACK. p0 carries
# the source to the relay; p1 carries the relay to every receiver. The ratios are
# measured over expected, for the sending side of each link. This is an estimate
# and the counters' exact meaning is not confirmed, so a ratio a little off 1 is
# normal; from 1.2 up in the data direction there is more on the link than we sent.
wire() {
    [ $# -ge 6 ] || usage
    B=$1 A=$2 SRC_HOST=$SRC_HOST RELAY_HOST=$RELAY_HOST awk -v flows="$3" -v rcv="$4" -v size="$5" -v count="$6" -v mtu="${7:-1024}" '
        function parse(text, arr,    n, lines, i, f) {
            n = split(text, lines, "\n")
            for (i = 1; i <= n; i++) {
                split(lines[i], f, " ")
                if (f[1] != "") { arr[f[1], "p0tx"] = f[4]; arr[f[1], "p1tx"] = f[7] }
            }
        }
        BEGIN {
            parse(ENVIRON["B"], b); parse(ENVIRON["A"], a)
            s = ENVIRON["SRC_HOST"]; r = ENVIRON["RELAY_HOST"]
            pk = int((size + mtu - 1) / mtu)
            data = size + 58 * pk; ack = 62
            p0d = (a[s, "p0tx"] - b[s, "p0tx"]) / (flows * count * data)
            p0a = (a[r, "p0tx"] - b[r, "p0tx"]) / (flows * count * ack)
            p1d = (a[r, "p1tx"] - b[r, "p1tx"]) / (flows * rcv * count * data)
            p1a = (a[s, "p1tx"] - b[s, "p1tx"]) / (flows * rcv * count * ack)
            printf "p0 data %.2fx ack %.2fx, p1 data %.2fx ack %.2fx", p0d, p0a, p1d, p1a
            if (p0d >= 1.2 || p1d >= 1.2) printf " (more than ours: someone else may be using the links)"
            printf "\n"
        }'
}

usage() {
    echo "usage: $0 [-t SECONDS] [--cpu PERCENT] [--gbps GBPS] | counters | wire BEFORE AFTER FLOWS RECEIVERS SIZE COUNT [MTU]" >&2
    exit 2
}

case ${1:-} in
    counters) shift; [ $# -eq 0 ] || usage; counters ;;
    wire) shift; wire "$@" ;;
    -h|--help) usage ;;
    *) check "$@" ;;
esac

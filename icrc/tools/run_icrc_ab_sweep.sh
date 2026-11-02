#!/usr/bin/env bash
set -euo pipefail

if (($# == 0)); then
    set -- dpdk-neon incremental
fi
for mode in "$@"; do
    case "$mode" in
        dpdk-neon|incremental) ;;
        *) echo "unsupported A/B mode: $mode" >&2; exit 2 ;;
    esac
done

node_a_dir=/home/mtchang/Open-Hackathon/icrc
node_b_dir=/home/mtchang/Open-Hackathon/icrc
runtime_rel=config/groups.m4.5qp.ipv4.runtime.json
wire_packets=10000000
log_dir=${AB_LOG_DIR:-reports/icrc-ab-raw-2026-11-02}
only_payload=${AB_ONLY_PAYLOAD:-}
mkdir -p "$log_dir"

payloads=(64 256 1024 4096 16384 65536)
windows=(32 32 8 2 1 1)

harness_pid=
node_b_started=0

cleanup_point()
{
    if [[ -n "$harness_pid" ]] && kill -0 "$harness_pid" 2>/dev/null; then
        kill -TERM "$harness_pid" 2>/dev/null || true
        wait "$harness_pid" 2>/dev/null || true
    fi
    harness_pid=
    if ((node_b_started)); then
        ssh nvaitc-2 "ssh DPU 'cd $node_b_dir && GROUPS_FILE=./$runtime_rel docker compose stop -t 60 >/dev/null 2>&1 || true; GROUPS_FILE=./$runtime_rel docker compose down >/dev/null 2>&1 || true'" || true
    fi
    node_b_started=0
    ssh nvaitc-1 "ssh DPU 'rm -f $node_a_dir/$runtime_rel'" 2>/dev/null || true
    ssh nvaitc-2 "ssh DPU 'rm -f $node_b_dir/$runtime_rel'" 2>/dev/null || true
}
trap 'cleanup_point' EXIT INT TERM

run_point()
{
    local mode=$1 payload=$2 window=$3
    local label=${mode}-${payload}B
    local harness_log=$log_dir/${label}-harness.log
    local node_b_log=$log_dir/${label}-node-b.log
    local cpu_log=$log_dir/${label}-cpu.log
    local line ready=0 workers=0 cpu_pid= harness_out harness_in

    : >"$harness_log"
    : >"$node_b_log"
    : >"$cpu_log"
    echo "BEGIN mode=$mode payload=$payload window=$window wire_packets=$wire_packets"

    coproc HARNESS {
        ssh nvaitc-1 "ssh DPU 'cd $node_a_dir && stdbuf -oL -eL ./build-local/m4_rc_ramp $runtime_rel 1 1 $payload $window $wire_packets'" 2>&1
    }
    harness_pid=$HARNESS_PID
    # Bash unsets the named coprocess array as soon as its process exits. Keep
    # duplicated descriptors so the final buffered PASS lines remain readable.
    exec {harness_out}<&"${HARNESS[0]}"
    exec {harness_in}>&"${HARNESS[1]}"
    while IFS= read -r -u "$harness_out" line; do
        printf '%s\n' "$line" | tee -a "$harness_log"
        if [[ "$line" == READY\ * ]]; then
            ready=1
            break
        fi
    done
    if ((ready == 0)); then
        wait "$harness_pid" || true
        harness_pid=
        echo "harness failed before READY: $label" >&2
        return 1
    fi

    ssh nvaitc-1 "ssh DPU 'cat $node_a_dir/$runtime_rel'" |
        ssh nvaitc-2 "ssh DPU 'tee $node_b_dir/$runtime_rel >/dev/null && cd $node_b_dir && ./build-local/groups_check $runtime_rel'"

    ssh nvaitc-2 "ssh DPU 'cd $node_b_dir && GROUPS_FILE=./$runtime_rel NODE_B_ICRC_BACKEND=$mode NODE_B_IPV4_UDP_CHECKSUM=0 docker compose up -d'"
    node_b_started=1
    for _ in {1..30}; do
        workers=$(ssh nvaitc-2 "ssh DPU 'docker exec icrc-node-b-1 ps -eLo comm= 2>/dev/null | grep -c ^dpdk-worker || true'")
        [[ "$workers" == 8 ]] && break
        sleep 1
    done
    if [[ "$workers" != 8 ]]; then
        echo "Node B did not reach eight workers: $label" >&2
        return 1
    fi

    printf '\n' >&"$harness_in"
    exec {harness_in}>&-
    {
        sleep 2
        ssh nvaitc-2 "ssh DPU 'docker stats --no-stream --format \"cpu={{.CPUPerc}} mem={{.MemUsage}}\" icrc-node-b-1; docker exec icrc-node-b-1 ps -eLo pcpu=,comm= | grep dpdk-worker'"
    } >"$cpu_log" 2>&1 &
    cpu_pid=$!

    while IFS= read -r -u "$harness_out" line; do
        printf '%s\n' "$line" | tee -a "$harness_log"
    done
    exec {harness_out}<&-
    if ! wait "$harness_pid"; then
        harness_pid=
        wait "$cpu_pid" || true
        echo "harness failed during traffic: $label" >&2
        return 1
    fi
    harness_pid=
    wait "$cpu_pid" || true
    if ! grep -q '^PASS ' "$harness_log"; then
        echo "missing PASS record: $label" >&2
        return 1
    fi

    ssh nvaitc-2 "ssh DPU 'cd $node_b_dir && GROUPS_FILE=./$runtime_rel NODE_B_ICRC_BACKEND=$mode NODE_B_IPV4_UDP_CHECKSUM=0 docker compose stop -t 60'"
    ssh nvaitc-2 "ssh DPU 'cd $node_b_dir && GROUPS_FILE=./$runtime_rel docker compose logs --no-color node-b 2>&1'" >"$node_b_log"
    if ! grep -q 'IPv4 UDP checksum rewrite=disabled' "$node_b_log"; then
        echo "checksum-disable startup record missing: $label" >&2
        return 1
    fi
    ssh nvaitc-2 "ssh DPU 'cd $node_b_dir && GROUPS_FILE=./$runtime_rel docker compose down && rm -f $node_b_dir/$runtime_rel'"
    node_b_started=0
    ssh nvaitc-1 "ssh DPU 'rm -f $node_a_dir/$runtime_rel'"
    echo "PASS_POINT mode=$mode payload=$payload"
}

for mode in "$@"; do
    for i in "${!payloads[@]}"; do
        if [[ -n "$only_payload" && "${payloads[$i]}" != "$only_payload" ]]; then
            continue
        fi
        run_point "$mode" "${payloads[$i]}" "${windows[$i]}"
    done
done

trap - EXIT INT TERM
cleanup_point
echo "PASS_SWEEP modes=$* log_dir=$log_dir"

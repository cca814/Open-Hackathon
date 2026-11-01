#!/bin/sh
set -eu

# Run the Milestone 3 wire experiment directly on the Node A DPU. Raw packet
# sockets require root; do not run this script through a privileged container.
if [ "$(id -u)" -ne 0 ]; then
    echo "Run directly as root: sudo tools/run_m3_experiment.sh [OUTPUT_DIR]" >&2
    exit 2
fi

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
oracle=$project_dir/tools/roce_wire.py

sender_interface=${SENDER_INTERFACE:-enp3s0f0s0}
capture_interface=${CAPTURE_INTERFACE:-p1}
source_mac=${SOURCE_MAC:-02:96:42:f8:5e:f2}
source_ip=${SOURCE_IP:-2001:db8::10}
group_ip=${GROUP_IP:-2001:db8::100}
group_qpn=${GROUP_QPN:-9000}
flows=${FLOWS:-64}
payload_size=${PAYLOAD_SIZE:-512}
# Set VERIFY_COUNT to a small positive value only when the capture interface is
# known to expose every packet. Hardware-offloaded OVS forwarding can bypass
# AF_PACKET on p1, so the default sustained experiment does not gate on it.
verify_count=${VERIFY_COUNT:-0}
load_count=${LOAD_COUNT:-20000}
load_interval=${LOAD_INTERVAL:-0}
unknown_count=${UNKNOWN_COUNT:-256}
tag=${TAG:-m3-$(date -u +%Y%m%dT%H%M%SZ)}
output_dir=${1:-$project_dir/reports/milestone-3-artifacts/$tag}

member0=02:7e:6d:fd:79:7c,2001:db8::1,101
member1=02:7e:6d:fd:79:01,2001:db8::2,102

mkdir -p "$output_dir"
metadata=$output_dir/workload.env
{
    echo "tag=$tag"
    echo "sender_interface=$sender_interface"
    echo "capture_interface=$capture_interface"
    echo "source_mac=$source_mac"
    echo "source_ip=$source_ip"
    echo "group_ip=$group_ip"
    echo "group_qpn=$group_qpn"
    echo "flows=$flows"
    echo "payload_size=$payload_size"
    echo "verify_count=$verify_count"
    echo "load_count=$load_count"
    echo "load_interval=$load_interval"
    echo "unknown_count=$unknown_count"
    echo "member0=$member0"
    echo "member1=$member1"
} >"$metadata"

capture_log=$output_dir/verify-capture.jsonl
verify_send_log=$output_dir/verify-send.jsonl
send_log=$output_dir/load-send.jsonl
timing_log=$output_dir/timing.env
unknown_log=$output_dir/unknown-send.jsonl
capture_pid=
cleanup()
{
    if [ -n "$capture_pid" ] && kill -0 "$capture_pid" 2>/dev/null; then
        kill "$capture_pid" 2>/dev/null || true
        wait "$capture_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

if [ "$verify_count" -gt 0 ]; then
    python3 "$oracle" capture \
        --interface "$capture_interface" --src-mac "$source_mac" \
        --src-ip "$source_ip" --dst-ip "$group_ip" --dst-qpn "$group_qpn" \
        --tag "$tag-verify" --count "$verify_count" --flows "$verify_count" \
        --payload-size "$payload_size" --member "$member0" --member "$member1" \
        --timeout 120 >"$capture_log" 2>&1 &
    capture_pid=$!

    # Do not send until the independent oracle has finished constructing every
    # expected member frame and started its receive loop.
    ready=0
    for _ in $(seq 1 600); do
        if grep -q '"event": "capture_ready"' "$capture_log"; then
            ready=1
            break
        fi
        if ! kill -0 "$capture_pid" 2>/dev/null; then
            wait "$capture_pid" || true
            echo "capture exited before traffic; see $capture_log" >&2
            exit 1
        fi
        sleep 0.2
    done
    if [ "$ready" -ne 1 ]; then
        echo "capture was not ready within 120 seconds; see $capture_log" >&2
        exit 1
    fi

    python3 "$oracle" send \
        --interface "$sender_interface" --src-mac "$source_mac" \
        --src-ip "$source_ip" --dst-ip "$group_ip" --dst-qpn "$group_qpn" \
        --tag "$tag-verify" --count "$verify_count" --flows "$verify_count" \
        --payload-size "$payload_size" --interval 0.001 >"$verify_send_log" 2>&1

    if ! wait "$capture_pid"; then
        capture_pid=
        echo "independent capture did not receive every expected replica; see $capture_log" >&2
        exit 1
    fi
    capture_pid=
else
    echo '{"event": "capture_skipped", "reason": "hardware-offloaded OVS bypasses AF_PACKET"}' >"$capture_log"
    echo '{"event": "verify_send_skipped", "count": 0}' >"$verify_send_log"
fi

# Sustained traffic is measured by Node B's per-queue/member completion
# counters and Node A's OVS counters. Hardware-offloaded OVS packets are not
# generally visible to an AF_PACKET socket on the physical representor.
start_ns=$(date +%s%N)
python3 "$oracle" send \
    --interface "$sender_interface" --src-mac "$source_mac" \
    --src-ip "$source_ip" --dst-ip "$group_ip" --dst-qpn "$group_qpn" \
    --tag "$tag-load" --count "$load_count" --flows "$flows" \
    --payload-size "$payload_size" --interval "$load_interval" >"$send_log" 2>&1
end_ns=$(date +%s%N)
{
    echo "start_ns=$start_ns"
    echo "end_ns=$end_ns"
    echo "elapsed_ns=$((end_ns - start_ns))"
} >"$timing_log"

# These valid RC SEND frames deliberately miss the immutable group table.
# Node B's shutdown counters must report exactly unknown_count unknown groups,
# without adding member submissions or completions.
python3 "$oracle" send \
    --interface "$sender_interface" --src-mac "$source_mac" \
    --src-ip "$source_ip" --dst-ip "$group_ip" --dst-qpn "$((group_qpn + 1))" \
    --tag "$tag-unknown" --count "$unknown_count" --flows "$flows" \
    --payload-size "$payload_size" --interval 0 >"$unknown_log" 2>&1

echo "Milestone 3 Node A workload complete"
echo "Evidence: $output_dir"
sed -n '$p' "$send_log"
sed -n '$p' "$capture_log"
sed -n '$p' "$unknown_log"

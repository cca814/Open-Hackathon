# Milestone 4 IPv4 hardware validation — 2026-10-29

## Hardware and software

- Node B: BlueField Arm, DOCA `3.5.0098`, DPDK `26.03.0.5`.
- Node B ports: `03:00.0` (DPDK port 2) and `03:00.1` (DPDK port 3).
- Node A sender: `mlx5_2`, GID index 1, IPv4 `10.0.1.1`.
- Node A receiver: `mlx5_3`, GID index 1, IPv4 `10.0.2.1`.
- Perftest: `ib_send_bw` 6.30, RC, one QP, 4 KiB messages, 10 seconds.
- Metrics enabled at a measured DPU TSC frequency of 299,981,760 Hz.

Stock perftest chooses QPNs and PSNs at runtime. `perftest_qp_gate.so`
pauses its single sender QP immediately before RTS, and
`m4_perftest_config.py` generates the matching one-receiver IPv4 group before
Node B starts. The gate does not generate traffic or replace perftest.

## Stage A: `ib_send_bw` IPv4 results

### Unconstrained sender

- Sender/receiver result: 0.067108 / 0.067157 Gbit/s.
- Node B forward arrivals: 443,783.
- Submitted/completed: 92,164 / 92,164.
- Backpressure drops: 351,619.
- TX errors/pending at shutdown: 0 / 0.
- ICRC samples: 92,164.
- ICRC average/min/p50-upper/p99-upper/max:
  13,457 / 13,384 / 13,650 / 27,304 / 36,032 ns.

This is a saturation failure, not a throughput pass. RC retries allowed the
benchmark to complete but amplified offered packets after Node B dropped
bursts.

### 50 Mbit/s, perftest default burst of 128

- Sender/receiver result: 0.049982 / 0.050331 Gbit/s.
- Node B forward arrivals: 165,838.
- Submitted/completed: 63,149 / 63,149.
- Backpressure drops: 102,689.
- TX errors/pending at shutdown: 0 / 0.
- ICRC samples: 63,149; average 13,447 ns; p99 upper 27,304 ns.

The rate limit alone was insufficient because perftest selected a 128-WQE
burst. RC retry amplification again made completed bandwidth hide loss.

### 50 Mbit/s, burst size 1

The sender used `--burst_size=1`, TX depth 16, and CQ moderation 1.

- Sender/receiver result: 0.050135 / 0.050134 Gbit/s.
- Matched SEND arrivals: 59,864.
- Submitted/completed: 59,864 / 59,864.
- Drops/TX errors/pending: 0 / 0 / 0.
- Bad ICRC/unknown group: 0 / 0.
- ICRC samples: 59,864.
- ICRC average/min/p50-upper/p99-upper/max:
  13,447 / 13,380 / 13,650 / 27,304 / 66,910 ns.

This is the loss-free IPv4 Stage A pass. It also isolates burst pressure as
the cause of the first two failures. It is not a saturation ceiling.

## Two-receiver IPv4 RTT metric gate

Stock `ib_send_bw` creates one remote RC connection and cannot produce the two
distinct receiver IP/QP ACK streams required by Milestone 4 aggregation. The
bounded wire probe therefore generated 32 valid IPv4 SENDs and one translated
positive ACK from each of two receiver domains, with a nominal 2 ms
SEND-to-first-ACK delay and 1 ms ACK gap.

- Forward SENDs: 32.
- Member submissions/completions: 32/32 for each member (64 total).
- ACKs received: 64.
- ACKs suppressed/forwarded: 32 / 32.
- Stale/malformed ACKs: 0 / 0.
- RTT samples/missed/overwritten: 32 / 0 / 0.
- Aggregate ACK frontier: sender PSN 100031.
- RTT average/min/p50-upper/p99-upper/max:
  3,123,253 / 2,367,577 / 3,495,462 / 3,495,462 / 3,159,148 ns.
- Forward-replica ICRC samples: 64; average 1,393 ns.
- Aggregate-ACK ICRC samples: 32; average 633 ns.
- Total ICRC samples: 96; average 1,140 ns.
- Group drops, TX errors, bad ICRC, pending buffers: all zero.

The logarithmic percentile value is a bucket upper bound, so it may exceed the
observed maximum. The metric definition ends when Node B processes the ACK that
advances the aggregate frontier; it excludes final transmission to the sender.

## Conclusions and next load gate

IPv4 parsing, address/QPN/PSN rewrite, checksums, ICRC timing, ACK aggregation,
and Node-B-visible RTT timing passed on hardware. The first two perftest runs
also demonstrate that benchmark completion is not sufficient evidence of a
healthy datapath: Node B counters must be checked for drops and retry
amplification.

For larger tests, keep burst size and outstanding depth bounded while stepping
offered load upward. Stock `ib_send_bw` remains the single-receiver RNIC/path
baseline. The two-receiver saturation test requires the duration-based
multi-QP libibverbs generator described in
`reports/milestone-4-metrics-and-load-plan.md`.

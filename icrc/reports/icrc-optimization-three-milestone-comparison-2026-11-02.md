# ICRC optimization: three-milestone combined report — 2026-11-02

## Executive summary

Three successive ICRC implementations were exercised on the same BlueField-3
five-member IPv4 RoCEv2 RC fan-out path:

1. portable bitwise full recomputation;
2. DPDK Arm NEON full recomputation;
3. constant-work incremental ICRC correction.

An intermediate `incremental-verify` sweep calculated both the incremental and
NEON results for every rewritten data packet and aggregate ACK. Its 336,313,133
comparisons produced zero mismatches and established the correctness gate before
pure incremental mode was measured.

For the clean NEON-versus-incremental A/B, pure incremental improved application
payload throughput by 1.12x–3.94x and reduced measured output ICRC time from
386–3,780 ns to 26 ns. Relative to the original bitwise experiment, final
throughput was 5.41x–12.29x higher and output ICRC time was 53.69x–517.69x lower.

These are application-path results, not an Ethernet line-rate claim. The
original 64 B and 256 B bitwise points were retry-amplified overload passes, so
their cross-milestone throughput gains must not be interpreted as clean capacity
ratios. The clean same-day NEON-versus-incremental A/B is the primary performance
comparison.

## Common topology and workload

All experiments used one ConnectX sender RC QP, five live receiver QPs, Node B
DOCA Flow UDP/4791 RSS steering, DPDK worker parsing and five-way fan-out, DOCA
ETH TX, aggregate ACK rewriting, and ConnectX receiver-side ICRC acceptance.
The RC path MTU was 1,024 bytes.

| Payload | Segments/SEND | SENDs | Window | Forward wire packets | Member transmissions |
|---:|---:|---:|---:|---:|---:|
| 64 B | 1 | 10,000,000 | 32 | 10,000,000 | 50,000,000 |
| 256 B | 1 | 10,000,000 | 32 | 10,000,000 | 50,000,000 |
| 1 KiB | 1 | 10,000,000 | 8 | 10,000,000 | 50,000,000 |
| 4 KiB | 4 | 2,500,000 | 2 | 10,000,000 | 50,000,000 |
| 16 KiB | 16 | 625,000 | 1 | 10,000,000 | 50,000,000 |
| 64 KiB | 64 | 156,250 | 1 | 10,000,000 | 50,000,000 |

The sender was unpaced. Every terminal pass required exact sender CQE parity,
five receiver CQEs per SEND, and exact payload bytes.

## Milestone 1 — portable bitwise full recomputation

The 2026-10-31 baseline performed a protocol-correct bitwise CRC-32 over every
rewritten member packet. It established functional five-way RC fan-out and
exposed two datapath limitations:

- full 1 KiB output ICRC cost approximately 13.44 microseconds per member;
- TX completion service was too infrequent for sustained fan-out bursts.

Completion-driven backpressure was added: progress after every input packet,
continued progress above the half-full TX watermark, and safe release of an
unprocessed RX burst on progress failure. The final 1 KiB–64 KiB baseline
points completed loss-free. The 64 B and 256 B results eventually completed
but remained retry-amplified overload measurements with fan-out drops, so they
are qualified throughout this report.

## Milestone 2 — DPDK NEON full recomputation and verification gate

The full backend was replaced with DPDK `rte_net_crc` using the Arm NEON CRC-32
implementation. Packet parsing was reused and ICRC invariant normalization was
performed temporarily in worker-owned memory, with every modified byte restored
before transmission.

Before enabling pure incremental correction, `incremental-verify` calculated
the table-based correction and independently performed a NEON full recomputation
on every output. The six payload points completed:

- 300,000,000 forward member comparisons;
- 36,313,133 aggregate-ACK comparisons;
- zero incremental/full mismatches;
- zero controlled-flow drops, TX errors, or pending buffers;
- exact sender/receiver CQ and payload parity.

The subsequent clean A/B measured NEON alone with IPv4 UDP checksum disabled
equally in both arms. Full input ICRC validation remained enabled.

## Milestone 3 — pure incremental correction

Pure incremental mode starts from the received packet's valid ICRC and XORs
precomputed reflected CRC contributions for only the changed destination IP,
destination QPN, and translated PSN bytes. Work is therefore independent of
payload length for the supported equal-length rewrite.

The clean six-point run retained full NEON validation of every input ICRC but
used incremental correction for all member outputs and aggregate ACKs. Every
point completed exactly 10,000,000 transmissions per member with no member
drops/errors, TX errors, pending buffers, bad input ICRCs, unknown groups, or
reliability errors.

## End-to-end throughput progression

Throughput is application payload throughput. `Verify` intentionally computes
both incremental and full NEON output ICRC and is shown as a correctness-stage
measurement, not a standalone performance milestone.

| Payload | Bitwise Mbit/s | Verify Mbit/s | NEON Mbit/s | Incremental Mbit/s |
|---:|---:|---:|---:|---:|
| 64 B | 20.351934* | 102.599684 | 98.435786 | 110.046163 |
| 256 B | 50.769036* | 195.042221 | 201.819260 | 406.418275 |
| 1 KiB | 94.697752 | 319.020783 | 295.058605 | 1,163.686409 |
| 4 KiB | 95.285704 | 291.808205 | 308.873437 | 845.858076 |
| 16 KiB | 96.006309 | 295.626764 | 296.941766 | 909.857464 |
| 64 KiB | 97.182017 | 304.894516 | 309.109182 | 1,023.152999 |

`*` Retry-amplified overload completion pass, not a loss-free capacity point.

### Throughput gains

| Payload | NEON / bitwise | Incremental / NEON | Incremental / bitwise |
|---:|---:|---:|---:|
| 64 B | 4.84x* | 1.12x | 5.41x* |
| 256 B | 3.98x* | 2.01x | 8.01x* |
| 1 KiB | 3.12x | 3.94x | 12.29x |
| 4 KiB | 3.24x | 2.74x | 8.88x |
| 16 KiB | 3.09x | 3.06x | 9.48x |
| 64 KiB | 3.18x | 3.31x | 10.53x |

The NEON/incremental column is the strongest causal comparison: identical
software image, workload, checksum setting, input validation, topology, and
ten-million-packet target, with only the output ICRC strategy changed.

## Output ICRC cost progression

Average time is for one output member's ICRC operation. Verify mode includes
both the incremental correction and full NEON comparison.

| Payload | Bitwise avg ns | Verify avg ns | NEON avg ns | Incremental avg ns |
|---:|---:|---:|---:|---:|
| 64 B | 1,396 | 403 | 386 | 26 |
| 256 B | 3,806 | 1,086 | 1,066 | 26 |
| 1 KiB | 13,440 | 3,800 | 3,780 | 26 |
| 4 KiB | 13,460 | 3,800 | 3,780 | 26 |
| 16 KiB | 13,444 | 3,796 | 3,776 | 26 |
| 64 KiB | 13,447 | 3,796 | 3,776 | 26 |

| Payload | NEON timing gain vs bitwise | Incremental timing gain vs bitwise | Incremental timing gain vs NEON |
|---:|---:|---:|---:|
| 64 B | 3.62x | 53.69x | 14.85x |
| 256 B | 3.57x | 146.38x | 41.00x |
| 1 KiB | 3.56x | 516.92x | 145.38x |
| 4 KiB | 3.56x | 517.69x | 145.38x |
| 16 KiB | 3.56x | 517.08x | 145.23x |
| 64 KiB | 3.56x | 517.19x | 145.23x |

NEON cost plateaus for 1 KiB and larger SENDs because the RNIC segments them
into 1 KiB RC packets. Incremental cost remains 26 ns because it depends on the
number and position of changed bytes rather than payload size.

## Sender RTT progression

Sender RTT spans `ibv_post_send` through the successful ACK-backed CQE and
therefore includes segmentation, all five copies, receiver ACK generation,
Node B aggregation, and sender completion.

| Payload | Bitwise avg us | Verify avg us | NEON avg us | Incremental avg us |
|---:|---:|---:|---:|---:|
| 64 B | 804.662* | 159.321 | 166.074 | 148.481 |
| 256 B | 1,290.295* | 335.285 | 323.842 | 160.446 |
| 1 KiB | 690.345 | 204.098 | 220.159 | 54.981 |
| 4 KiB | 683.334 | 220.099 | 207.756 | 73.060 |
| 16 KiB | 1,348.438 | 426.606 | 424.658 | 127.332 |
| 64 KiB | 5,328.461 | 1,653.493 | 1,629.913 | 446.297 |

Pure incremental reduced average sender RTT by 1.12x–4.00x relative to NEON.
Rare maximum outliers occurred on the shared system in both A/B arms; average,
p50, p99, and full-run duration are more representative than maximum RTT.

## Correctness and stability progression

| Gate | Bitwise baseline | Incremental verify | Clean NEON/incremental A/B |
|---|---|---|---|
| ConnectX receiver acceptance | PASS | PASS | PASS |
| Sender ACK-backed CQEs | PASS | PASS | PASS |
| Exact receiver CQ/payload parity | PASS | PASS | PASS |
| Incremental/full comparisons | N/A | 336,313,133; zero mismatches | Previously gated |
| Per-member final transmissions | Exact for clean 1 KiB–64 KiB points | 10,000,000 each | 10,000,000 each |
| TX errors/pending buffers | Zero in final clean points | Zero | Zero |
| Fan-out drops | 64 B/256 B overload only | Zero | Zero |
| Reliability errors | Zero in final clean points | Zero | Zero |

One initial 4 KiB NEON A/B sample contained one retry and five invalid ACK
observations. It was rejected rather than averaged into the result. The isolated
replacement sample completed with exact counts and zero datapath or reliability
anomalies.

## Interpretation

The optimization sequence moved the bottleneck in three clear steps:

1. Bitwise CRC dominated the one-worker five-member path at roughly 13.44
   microseconds per 1 KiB member copy.
2. NEON cut full recomputation by about 3.56x and raised clean large-payload
   throughput from roughly 95–97 Mbit/s to 295–309 Mbit/s.
3. Incremental correction reduced output ICRC to 26 ns and raised clean
   large-payload throughput to 846–1,164 Mbit/s.

The gap between the 145x ICRC-operation gain and the 2.74x–3.94x end-to-end
gain shows that output ICRC is no longer the only dominant cost. The next likely
bottlenecks are full input ICRC validation, five member-frame copies, DOCA buffer
lookup/task allocation, TX completion cadence, ACK processing, and the fact that
one RC flow maps to one RSS worker while all eight workers continue polling.

At 64 B, incremental improves throughput only 1.12x despite a 14.85x output
ICRC improvement, confirming that fixed per-packet submission/completion and
copy costs dominate short packets.

## Comparability and limitations

- The clean same-day NEON/incremental A/B disabled IPv4 UDP checksum equally in
  both arms with `NODE_B_IPV4_UDP_CHECKSUM=0`. IPv4 legally represents no UDP
  checksum as zero. The normal default remains enabled, and IPv6 always retains
  its mandatory checksum.
- The bitwise and verification sweeps did not use this explicit checksum-off
  experiment setting. Cross-experiment total gains therefore include image,
  backpressure, shared-system, and configuration differences; use the A/B for
  attribution specifically to incremental output ICRC.
- The original bitwise 64 B and 256 B points were retry-amplified overload
  completions and are marked with `*`.
- Runs were sequential rather than randomized. Thermal state and unrelated
  shared-system activity may contribute small differences.
- Throughput is application payload throughput through one sender RC flow, not
  Ethernet line rate or eight-flow aggregate capacity.
- Output ICRC timing excludes full input ICRC validation, copying, header/IP
  checksum work, task allocation, completion processing, and ACK aggregation.

## Source reports and artifacts

- `reports/milestone-4-10m-wire-packet-size-sweep-2026-10-31.md`
- `reports/icrc-optimization-10m-payload-sweep-2026-11-02.md`
- `reports/icrc-incremental-ab-2026-11-02.md`
- Raw A/B logs: `reports/icrc-ab-raw-2026-11-02/`
- Reproduction runner: `tools/run_icrc_ab_sweep.sh`

All final experiment containers and live-QP runtime files were removed after
measurement. Project OVS flows and the temporary permanent neighbor require the
operator's interactive `sudo` cleanup when hardware testing is complete.

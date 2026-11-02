# ICRC optimization ten-million-packet payload sweep — 2026-11-02

## Outcome

PASS at every payload size in `incremental-verify` mode. Each point processed
exactly 10,000,000 forward RoCEv2 wire packets and produced five member copies.
All sender and receiver CQEs completed with exact payload parity. Node B
reported zero controlled-flow drops, TX errors, pending buffers, bad input
ICRCs, unknown groups, malformed/stale ACKs, RTT misses/overwrites, and
incremental-versus-full ICRC mismatches.

The backend was `dpdk-neon-full`; the strategy calculated the incremental
correction and independently recalculated with DPDK NEON for every rewritten
data packet and aggregate ACK. Across the sweep, 336,313,133 verification-mode
ICRC operations agreed exactly: 300,000,000 forward member copies plus
36,313,133 aggregate ACKs.

## Workload

| Payload | Segments/SEND | SENDs | Window | Wire packets |
|---:|---:|---:|---:|---:|
| 64 B | 1 | 10,000,000 | 32 | 10,000,000 |
| 256 B | 1 | 10,000,000 | 32 | 10,000,000 |
| 1 KiB | 1 | 10,000,000 | 8 | 10,000,000 |
| 4 KiB | 4 | 2,500,000 | 2 | 10,000,000 |
| 16 KiB | 16 | 625,000 | 1 | 10,000,000 |
| 64 KiB | 64 | 156,250 | 1 | 10,000,000 |

## End-to-end results

Payload throughput is application payload throughput. Sender RTT runs from
`ibv_post_send` through the ACK-backed sender CQE and includes all five member
paths.

| Payload | Result | Mbit/s | Duration ms | RTT avg us | p50 us | p99 us | max us |
|---:|:---:|---:|---:|---:|---:|---:|---:|
| 64 B | PASS | 102.599684 | 49,902.688 | 159.321 | 158.662 | 175.017 | 4,464.380 |
| 256 B | PASS | 195.042221 | 105,002.906 | 335.285 | 343.636 | 442.162 | 4,317.188 |
| 1 KiB | PASS | 319.020783 | 256,785.778 | 204.098 | 202.988 | 212.902 | 5,694.067 |
| 4 KiB | PASS | 291.808205 | 280,732.340 | 220.099 | 218.716 | 229.900 | 5,583.893 |
| 16 KiB | PASS | 295.626764 | 277,106.169 | 426.606 | 426.388 | 433.918 | 3,509.177 |
| 64 KiB | PASS | 304.894516 | 268,683.087 | 1,653.493 | 1,650.377 | 1,669.751 | 6,945.278 |

Every row had sender CQEs equal to SENDs, five receiver CQEs per SEND, and
`receiver_parity=true sender_parity=true`.

## Node B ICRC and ACK RTT

Forward ICRC timing includes both the incremental result and the independent
DPDK NEON full recomputation used to verify it. Percentiles are histogram
bucket upper bounds.

| Payload | Forward samples | ICRC avg ns | min ns | p50 upper ns | p99 upper ns | max ns | Node B RTT samples | RTT avg ns | min ns | p50 upper ns | p99 upper ns | max ns |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 B | 50,000,000 | 403 | 383 | 423 | 423 | 152,385 | 10,000,000 | 8,850 | 4,353 | 13,650 | 27,304 | 3,419,664 |
| 256 B | 50,000,000 | 1,086 | 1,056 | 1,703 | 1,703 | 27,728 | 10,000,000 | 100,236 | 8,743 | 109,229 | 436,929 | 1,001,754 |
| 1 KiB | 50,000,000 | 3,800 | 3,750 | 6,823 | 6,823 | 42,865 | 10,000,000 | 28,375 | 15,290 | 54,613 | 54,613 | 2,933,948 |
| 4 KiB | 50,000,000 | 3,800 | 3,753 | 6,823 | 6,823 | 5,044,509 | 4,750,633 | 43,349 | 15,874 | 54,613 | 218,463 | 2,471,493 |
| 16 KiB | 50,000,000 | 3,796 | 3,753 | 6,823 | 6,823 | 27,485 | 1,250,000 | 28,535 | 15,057 | 54,613 | 54,613 | 1,078,258 |
| 64 KiB | 50,000,000 | 3,796 | 3,753 | 6,823 | 6,823 | 3,617,426 | 312,500 | 28,741 | 23,108 | 54,613 | 54,613 | 2,453,262 |

Each point completed exactly 10,000,000 submissions/completions per member,
50,000,000 total member transmissions, and had `icrc_verify_fail=0` on every
queue. Additional malformed frames were unrelated ambient UDP/4791 traffic and
were rejected before group lookup.

## CPU utilization

| Payload | Container CPU | Worker CPU range |
|---:|---:|---:|
| 64 B | 802.16% | 94.3–94.4% |
| 256 B | 802.46% | 94.4–94.5% |
| 1 KiB | 802.46% | 94.2–94.3% |
| 4 KiB | 802.47% | 93.5–93.6% |
| 16 KiB | 802.98% | 93.6–93.7% |
| 64 KiB | 802.19% | 95.8% |

These are point samples during each run. All eight polling workers are expected
to remain busy; one RSS queue carries the controlled sender flow while every
worker continues polling its assigned queues in both directions.

## Comparison with 2026-10-31 full-bitwise sweep

| Payload | Old Mbit/s | New Mbit/s | Throughput gain | Old ICRC avg ns | New verify avg ns | ICRC-time gain |
|---:|---:|---:|---:|---:|---:|---:|
| 64 B | 20.351934 | 102.599684 | 5.04x | 1,396 | 403 | 3.46x |
| 256 B | 50.769036 | 195.042221 | 3.84x | 3,806 | 1,086 | 3.50x |
| 1 KiB | 94.697752 | 319.020783 | 3.37x | 13,440 | 3,800 | 3.54x |
| 4 KiB | 95.285704 | 291.808205 | 3.06x | 13,460 | 3,800 | 3.54x |
| 16 KiB | 96.006309 | 295.626764 | 3.08x | 13,444 | 3,796 | 3.54x |
| 64 KiB | 97.182017 | 304.894516 | 3.14x | 13,447 | 3,796 | 3.54x |

The small-packet points are now clean rather than retry-amplified: each saw
exactly 10,000,000 controlled forward packets and zero fan-out drops. The
comparison strongly validates the optimization, but it is not a pure
incremental-backend capacity measurement because verification mode intentionally
computes both algorithms. Isolated `dpdk-neon` and `incremental` A/B sweeps are
the remaining performance characterization.

## Cleanup

Node B was stopped gracefully after every point, final counters were collected,
and Compose removed the container. No harness, Node B process/container, or
temporary live-QP JSON remained after the experiment. Project OVS flows and the
temporary permanent neighbor require the operator's interactive `sudo` cleanup.

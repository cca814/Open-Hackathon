# Milestone 4 metrics and high-volume traffic plan

## Implemented measurements

### ICRC recomputation time

The timed interval surrounds only `roce_icrc_ipv4()` or
`roce_icrc_ipv6()` while constructing each rewritten SEND replica or aggregate
ACK. Parsing, address/QPN/PSN writes, IP checksum, UDP checksum, TX submission,
and input-ICRC validation are outside the interval.

Hardware builds use `rte_rdtsc()` and convert ticks with `rte_get_tsc_hz()`.
Each worker keeps count, total, minimum, maximum, and a 64-bucket logarithmic
histogram. Shutdown output includes per-direction/per-queue and aggregate
`avg_ns`, `min_ns`, `p50_upper_ns`, `p99_upper_ns`, and `max_ns`.

### Traffic round-trip time

The Node B RTT definition is:

```text
reliable SEND arrival at Node B p0
    -> two receiver replicas
    -> both receiver ACK frontiers advance
    -> aggregate ACK arrival/processing at Node B p1
```

It therefore measures the multicast network/receiver feedback round trip seen
by Node B. It excludes the final Node B-to-sender transmission and sender CQ
processing. The high-volume sender will separately measure post-send-to-CQE
latency as the end-to-end RDMA operation RTT.

SEND timestamps are indexed by the sender-domain 24-bit PSN in a bounded
16,384-slot ring per reliable group. No allocation occurs in the packet path.
ACK aggregation samples RTT only when `AggAckPSN` advances. `rtt_overwrites`
reports reuse of a still-live ring slot, such as an outstanding window larger
than the ring or a retransmission recorded before cumulative ACK retirement;
`rtt_missed` reports an aggregate ACK without a matching timestamp.
Both must be interpreted alongside the RTT sample count.

Metrics are enabled by default. `NODE_B_METRICS=0` disables TSC reads for an
A/B overhead run. There is no per-packet logging.

## Validation plan

1. Portable deterministic-clock tests verify exact tick counts for one ICRC
   rewrite and one SEND-to-aggregate-ACK RTT.
2. Run AddressSanitizer and UndefinedBehaviorSanitizer over the worker,
   reliability manager, metrics, parser, and checksum code.
3. Build inside the DOCA 3.5.0098 development image and run all Meson tests.
4. On BlueField, send a small controlled workload and require:
   - ICRC samples equal successful rewritten replicas plus forwarded aggregate
     ACKs;
   - RTT samples are nonzero and do not exceed forwarded aggregate ACKs;
   - `rtt_missed=0` and `rtt_overwrites=0` for a bounded outstanding window;
   - captured packet checksums and ICRC remain correct.
5. Repeat the same workload with `NODE_B_METRICS=0`; compare packet rate and CPU
   to quantify instrumentation overhead.

## Preparing voluminous RDMA traffic

Use two stages rather than treating one generator as proof of everything.

### Stage A: RNIC and path baseline

Run `ib_send_bw` with a normal single-receiver RC connection, without fan-out,
to establish the sender/receiver RNIC ceiling. Sweep message sizes such as 64,
256, 1024, 4096, and 65536 bytes, queue depths, CQ moderation, and inline
thresholds. Record line rate, message rate, CQ errors, retries, and CPU. This is
the baseline only; stock `ib_send_bw` does not create the two-receiver virtual
RC connection required by Milestone 4.

### Stage B: multicast reliability load generator

Extend the libibverbs Milestone 4 harness into a duration-based generator:

- one sender QP plus two receiver QPs per reliable group;
- 8–64 groups/QPs to populate all eight RSS queues;
- pre-posted receive rings and replenishment in batches;
- bounded outstanding SEND window below 16,384 PSNs per group;
- mostly unsignaled SENDs with periodic signaled completions;
- separate pinned sender and receiver CQ polling threads;
- warm-up, fixed measurement interval, and cooldown phases;
- sender-side TSC timestamps for post-to-CQE average/p50/p99/max;
- exact sent, receiver-CQE, sender-CQE, retry, error, and timeout counts;
- runtime QPN/PSN configuration generated fresh for every run.

Start with loss-free traffic and `M4_ACK_DELAY` disabled. Increase offered load
in steps (for example 10%, 25%, 50%, 75%, 90%, then saturation) rather than
jumping directly to maximum. At every step collect:

- offered and completed messages/s and Gbit/s;
- Node B input packets, output replicas, ACK received/forwarded/suppressed;
- per-queue and per-member submissions, completions, drops, and TX errors;
- ICRC recomputation and Node B RTT summaries;
- sender post-to-CQE RTT;
- Flow counters, RSS distribution, DPU CPU utilization, and pending buffers;
- receiver and sender CQ status, QP state, and hardware retry counters.

A valid high-volume run requires receiver completion parity, sender completion
parity, zero unexpected drops/errors, no persistent pending buffers, monotonic
ACK frontier progress, and consistent Flow/worker/capture counts. Throughput
must be reported together with message size and instrumentation state.

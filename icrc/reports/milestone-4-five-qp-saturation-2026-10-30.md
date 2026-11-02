# Milestone 4 five-QP saturation attempt — 2026-10-30

## Method

`tools/m4_rc_ramp.c` was extended with a configurable registered-buffer ring
and outstanding SEND window. The original correctness mode remains available
with window 1. Saturation mode used:

- one sender RC QP and five destination RC QPs on one receiver SF;
- IPv4 RoCE v2, 4 KiB messages, 1024-byte path MTU;
- a 4,096-message outstanding window;
- a separate registered buffer slot for every in-flight message on every QP;
- busy rate pacing, batched CQ polling, and signaled sender completions;
- slot reuse only after the sender CQE and all five receiver CQEs;
- payload verification at every receiver CQE;
- a 30-second bounded drain deadline after each one-second offered-load stage.

The requested sequence was 5, 10, 25, 50, and 100 Gbit/s. The run was required
to stop on the first CQ error, timeout, parity failure, or loss/backpressure
indication rather than advancing into a known-failed state.

Fresh live QPs were sender `3510`, virtual destination `985`, and receivers
`980,981,982,983,984`. The runtime group validated as one group and five
members. Node B started normally with eight bidirectional workers.

## Result: 5 Gbit/s failed the loss-free gate

The generator filled its outstanding window during the first stage:

- Messages posted: 17,888.
- Messages completing sender plus all five receiver CQEs: 13,792.
- Messages still outstanding after the drain deadline: 4,096.
- Result: stage timeout and immediate ramp termination.

The 10, 25, 50, and 100 Gbit/s stages were deliberately not attempted. Once
5 Gbit/s had failed with a full outstanding window, higher rates could only add
retry amplification and would not be meaningful loss-free measurements.

## Node B evidence

The single sender flow hashed to forward RSS queue 1:

- Forward packets received on queue 1: 1,333,186.
- Total replica submissions/completions: 335,217 / 335,217.
- Fan-out slot drops: 6,330,713.
- TX completion errors/pending at shutdown: 0 / 0.
- Flow matched forward packets: 1,599,811; bytes: 1,730,993,992.

The drop and submission totals account exactly for all five attempted replicas:

```text
1,333,186 input packets * 5 members
    = 6,665,930 replica attempts
335,217 submissions + 6,330,713 drops
    = 6,665,930 replica attempts
```

Per-member submissions were no longer equal under slot exhaustion:

- Member 0: 99,654.
- Member 1: 59,513.
- Member 2: 59,310.
- Member 3: 58,370.
- Member 4: 58,370.

This partial-fan-out state is outside Milestone 4's controlled loss-free scope.
It caused RC retry amplification and reliability degradation:

- ACKs received/forwarded/suppressed: 72,384 / 13,868 / 58,516.
- Stale/invalid ACKs: 3,107 / 1,223.
- RTT timestamp overwrites/misses: 3,268 / 49.
- Node B RTT average: 1,123,891 ns, versus about 87,000 ns in the loss-free
  100 Mbit/s confirmation run.

ICRC work remained error-free for submitted packets, but was the dominant CPU
cost:

- Forward replica ICRC samples: 335,217.
- Average per-replica ICRC time: 13,464 ns.
- p50/p99 bucket upper bounds: 13,650 / 27,304 ns.

## Capacity interpretation

One input packet requires five full ICRC recomputations on the one worker that
owns its RSS flow. At the measured average, ICRC alone costs approximately:

```text
5 * 13,464 ns = 67,320 ns per input packet
```

That is an optimistic one-worker ceiling of roughly 14,854 input packets/s.
Using the observed 1,082-byte matched frame size gives about 129 Mbit/s before
parsing, copies, checksums, DOCA ETH submission, completion work, and ACK
processing. Perfectly balancing independent groups across all eight workers
would raise that ICRC-only upper bound to roughly 1.0 Gbit/s, still far below
5 Gbit/s and far below 100 Gbit/s.

Therefore the requested 5–100 Gbit/s loss-free ramp is not achievable with the
current Arm full-recompute, five-replica datapath. Reaching those rates requires
moving or reducing ICRC work (incremental correction, DPA, or hardware assist),
distributing multiple independent source groups across RSS queues, and making
fan-out admission atomic so overload drops a whole multicast packet rather
than producing unequal partial replication.

## Verification

- The windowed harness compiled on Node A against the actual libibverbs
  provider with `-Wall -Wextra -Wpedantic -Werror`.
- A 4,096-entry queue/CQ and registered-buffer ring reached RTS on all six QPs.
- The full portable test suite passed after the harness change.
- Node B stopped cleanly with exit status 0 and no pending TX buffers.

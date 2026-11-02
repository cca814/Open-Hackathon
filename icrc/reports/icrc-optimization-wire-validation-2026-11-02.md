# ICRC optimization bounded wire validation — 2026-11-02

## Outcome

PASS for a bounded five-member reliable RC run using DPDK NEON full ICRC plus
incremental correction verification on every rewritten SEND and aggregate ACK.

## Configuration

- DOCA `3.5.0098`; DPDK `26.03.0.5`; Cortex-A78AE Arm runtime.
- Backend: `dpdk-neon-full`; strategy: `incremental-verify`.
- One sender QP, one virtual destination QPN, five live receiver QPs.
- IPv4 RoCEv2 RC SEND; path MTU 1,024 bytes.
- 10,000 messages, 1,024-byte payload, window 1, 10,000 wire packets.
- Live QPNs: sender 3679, virtual 2179, receivers 2174–2178.

## End-to-end result

```text
messages=10000 wire_packets=10000 achieved_mbps=211.771759
duration_ms=386.832 rtt_avg_us=37.347 rtt_p50_us=36.629
rtt_p99_us=42.886 rtt_max_us=3894.697
per_receiver_cqes=10000 total_receiver_cqes=50000 sender_cqes=10000
receiver_parity=true sender_parity=true
```

Every receiver payload matched exactly.

## Node B result

- Forward controlled packets: 10,000 on queue 3.
- Replica submissions/completions: 50,000 / 50,000.
- Each of five members: 10,000 submitted and 10,000 completed.
- Member errors/drops, TX errors, pending buffers: zero.
- Bad input ICRC, unknown group, ACK invalid/stale/malformed: zero.
- Incremental-versus-full mismatches: zero on every queue.
- ACKs received/forwarded/suppressed: 50,000 / 10,000 / 40,000.
- RTT overwrites/misses: zero / zero.
- Flow matches: 10,001 forward and 50,001 reverse; one unrelated forward
  frame was rejected as malformed outside the controlled flow.

Forward ICRC verification timing (incremental correction plus DPDK NEON full
recomputation for every member):

```text
samples=50000 avg_ns=3796 min_ns=3756
p50_upper_ns=6823 p99_upper_ns=6823 max_ns=11500
```

Aggregate-ACK ICRC verification produced 10,000 samples. Across forward and
reverse paths the total was 60,000 samples, average 3,196 ns, with zero
verification failures.

The earlier bitwise full-recompute 1 KiB measurement was approximately 13,440
ns per forward replica. The bounded verification-mode result is about 3.54x
faster despite calculating both the incremental result and a DPDK NEON full
result. This comparison is encouraging but is not a capacity claim; the full
payload sweep and isolated `dpdk-neon`/`incremental` A/B runs remain required.

## Cleanup

- Node B stopped cleanly with exit status 0 and was removed with Compose.
- No Node B or harness process remained.
- Ephemeral live-QP JSON copies were removed from all relay/target checkouts.
- The temporary OVS cookie flows and reciprocal permanent neighbor require
  interactive `sudo` cleanup by the operator.

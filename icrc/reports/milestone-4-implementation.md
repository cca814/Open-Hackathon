# Milestone 4 implementation and validation

## Scope implemented

- Parse positive RoCEv2 RC ACK packets (`0x11`) in addition to RC SEND.
- Strict reliable-group configuration with sender, virtual ACK source, sender
  initial PSN, and per-receiver initial PSNs.
- Translate forward SEND PSNs into each receiver's PSN space.
- Translate receiver ACK PSNs back into the sender's PSN space.
- Track `AckPSN[i]`, `AggAckPSN`, and Cepheus `triPort` per group.
- Forward only a strictly advancing group-wide minimum ACK frontier.
- Suppress duplicate, stale, reordered, and non-frontier ACKs.
- Compare 24-bit PSNs safely across wraparound within the RC half-range window.
- Rewrite aggregate ACK source/destination MAC/IP, destination QPN, and PSN,
  then recompute IPv4 checksum (when applicable), UDP checksum, and full ICRC.
- Share aggregation state safely across the eight reverse workers.
- Report received, forwarded, suppressed, stale, malformed, delayed, frontier,
  and `triPort` counters.
- Provide a disabled-by-default one-shot `M4_ACK_DELAY=member:microseconds`
  validation hook.

NACK aggregation, loss recovery, and retransmission filtering remain Milestone
5 work. Best-effort Milestone 2/3 group files retain their old behavior.

## Portable evidence

`./tests/run_local.sh` passes. It includes all earlier tests plus:

- reliable schema acceptance and incomplete-schema rejection;
- member-specific forward PSN translation with valid ICRC;
- delayed receiver behavior and no premature aggregate ACK;
- duplicate and reordered ACK suppression;
- sender-facing address/QPN/PSN rewrite with valid ICRC;
- 24-bit PSN wrap from `0xffffff` to `0`;
- reverse-worker suppression, transmission, ownership, and completion;
- one-shot delayed-ACK injection.

The focused test also passes under AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
cc -std=c11 -Wall -Wextra -Werror -pedantic -g \
  -fsanitize=address,undefined -Iinclude \
  src/worker.c src/roce.c src/icrc.c src/groups.c src/reliability.c src/metrics.c \
  tests/test_reliability.c -o /tmp/icrc-test-reliability-sanitized
/tmp/icrc-test-reliability-sanitized
```

Meson was not available on the local development host, so the clean Meson and
hardware-enabled build must run in the existing DOCA development image on Node
B before live traffic.

## Live hardware acceptance plan

1. Build in the DOCA 3.5.0098 image and run every Meson test.
2. Create one sender QP, two receiver QPs, and a distinct virtual destination
   QP/GID on Node A. Keep them alive while generating the runtime group file.
3. Put both receiver QPs in RTR/RTS with the configured member initial PSNs and
   post identical receive buffers. Put the sender in RTR/RTS with the virtual
   peer and configured sender initial PSN.
4. Start Node B with the generated reliable group and
   `M4_ACK_DELAY=1:500000`.
5. Post one signaled SEND. Confirm both receiver CQEs and payloads complete,
   while the sender CQE does not complete before 500 ms and does complete after
   the delayed member ACK is processed.
6. Stop Node B cleanly and verify one delayed ACK, frontier advancement, member
   submission/completion parity, zero TX errors, zero unexpected drops, and zero
   pending buffers. Verify packet bytes/checksums/ICRC with the independent
   oracle or capture path.
7. Repeat without `M4_ACK_DELAY` for a sustained loss-free workload. Require all
   sender/receiver CQEs, no QP errors or stalls, monotonic frontier progress, and
   no buffer leaks.

This report records implementation and portable validation only. Milestone 4's
hardware done-condition is satisfied only after the two-receiver live run.

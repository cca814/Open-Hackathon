# Milestone 4 five-QP IPv4 hardware validation — 2026-10-30

## Topology and implementation

- Node A sender: `mlx5_2`, `enp3s0f0s0`, IPv4 GID `10.0.1.1`.
- Node A receiver: `mlx5_3`, `enp3s0f1s0`, IPv4 GID `10.0.2.1`.
- All five destination RC QPs share the one receiver SF, interface, IPv4
  address, and MAC address. Distinct QPNs and separated initial PSN domains
  identify the five reliable members.
- `tools/configure_node_a_ovs.sh setup` configures only the sender SF and the
  single receiver SF. It assigns both IPv4 addresses before installing the
  project-cookie OVS flows. It has no `enp3s0f1s1` path.
- `tools/m4_rc_ramp.c` creates one sender QP, five receiver QPs, and the virtual
  destination QPN. It writes the live group JSON after every QP reaches RTS,
  preposts a receive on every destination, and verifies every receive CQE,
  sender CQE, and payload before proceeding.
- Same-IP ACK member selection translates each receiver PSN into the sender
  domain and chooses the candidate nearest that member's ACK frontier.
- RTT timestamps now retire every PSN covered by a cumulative ACK. This avoids
  false ring-overwrite counts when the RNIC coalesces ACKs and advances the
  aggregate frontier by more than one packet.

The provider-accepted RC transition matches `ib_send_bw`: 1024-byte path MTU,
IPv4 RoCE v2 GID index 1, hop limit 255, `max_dest_rd_atomic=1`, and
`max_rd_atomic=1`.

## Validation procedure

1. Run `sudo ./tools/configure_node_a_ovs.sh setup` on the Node A DPU.
2. Verify GID index 1 is `::ffff:10.0.1.1` on `mlx5_2` and
   `::ffff:10.0.2.1` on `mlx5_3`.
3. Allocate eight 512 MiB hugepages on Node B.
4. Start the five-QP harness and leave it paused at `READY`.
5. Copy its runtime-generated JSON to Node B and run `groups_check`; the result
   must be `1 groups, 5 members`.
6. Start Node B with that exact JSON, confirm the eight workers are polling,
   then release the harness.
7. Gracefully stop Node B and inspect every queue, member, reliability, ICRC,
   RTT, drop, error, and pending counter.

The rebuilt Arm image used DOCA `3.5.0098` and DPDK `26.03.0.5`. Its Docker
build compiled the hardware adapters, ran all eight Meson tests successfully,
and verified that `node_b` resolves `libdoca_flow` and `libdoca_eth`.

## Confirmation run

Live QPs were sender `3509`, virtual destination `979`, and receivers
`974,975,976,977,978`. The harness sent 4 KiB RC SEND messages for two seconds
at each requested rate.

| Requested | Achieved | Messages | Median RTT | p99 RTT | Max RTT |
|---:|---:|---:|---:|---:|---:|
| 5 Mbit/s | 5.015 Mbit/s | 305 | 360.865 us | 368.079 us | 3852.254 us |
| 10 Mbit/s | 10.014 Mbit/s | 610 | 360.591 us | 367.372 us | 380.970 us |
| 25 Mbit/s | 25.011 Mbit/s | 1,525 | 359.788 us | 366.795 us | 583.762 us |
| 50 Mbit/s | 50.006 Mbit/s | 3,051 | 359.712 us | 366.249 us | 392.551 us |
| 100 Mbit/s | 88.415 Mbit/s | 6,103 | 359.155 us | 365.916 us | 387.850 us |

The harness passed 11,594 messages with exact parity:

- Sender CQEs: 11,594.
- Receiver CQEs: 11,594 for each of five receivers, 57,970 total.
- `receiver_parity=true`, `sender_parity=true`; every payload matched.

Each 4 KiB message produced four MTU-sized forward packets. Node B therefore
processed 46,376 valid sender packets and completed 46,376 transmissions for
each member, 231,880 member transmissions total. All member errors and drops,
TX errors, pending buffers, bad ICRC packets, unknown groups, invalid ACKs,
stale ACKs, RTT misses, and RTT overwrites were zero.

Reliability counters:

- ACKs received: 115,940.
- Aggregate ACKs forwarded: 23,188.
- ACKs suppressed: 92,752.
- Final aggregate sender PSN: 1,239,421.
- RTT samples: 23,188; average 86,998 ns, minimum 84,721 ns,
  p50/p99 bucket upper bound 109,229 ns, maximum 732,711 ns.

ICRC counters:

- Forward replica samples: 231,880; average 13,444 ns, minimum 13,380 ns,
  p50 upper bound 13,650 ns, p99 upper bound 27,304 ns, maximum 34,392 ns.
- Aggregate-ACK samples: 23,188.
- Total ICRC samples: 255,068; average 12,280 ns.

Two additional malformed forward UDP/4791 frames hashed to another RSS queue;
they were rejected before group lookup and were not part of the test QP flow.
The valid test-flow count, per-member transmissions, and all CQEs have exact
expected parity.

## Interpretation

The 5–50 Mbit/s stages track their requested rates. The 100 Mbit/s request
drains at 88.415 Mbit/s because this correctness harness permits one outstanding
message and waits for the sender plus all five receiver CQEs before posting the
next message. This is a harness serialization ceiling, not a Node B loss or TX
backpressure ceiling: Node B reported zero drops and exact completion parity.
A saturation test should add a bounded window of outstanding messages while
retaining per-QP CQE and payload accounting.

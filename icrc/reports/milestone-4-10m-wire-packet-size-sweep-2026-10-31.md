# Milestone 4 IPv4 ten-million-packet payload sweep

## Scope and pass criteria

This hardware experiment used one ConnectX sender RC QP, five destination RC
QPs on one receiver SF/interface, IPv4 RoCEv2, Node B DOCA Flow steering, one
RSS worker for the sender flow, five-way DPDK fan-out with full ICRC
recomputation, and DOCA ETH TX. The sender was unpaced. There was no configured
bit-rate limiter or inter-message delay.

The target for every payload point was exactly 10,000,000 forward RoCE packets
after RNIC path-MTU segmentation. The configured RC path MTU was 1,024 bytes.
For payloads below the path MTU, one SEND still produces one on-wire RoCE data
packet; those packets are naturally shorter than 1,024 bytes.

The harness required all five receivers to return a successful CQE with exact
payload bytes and the sender to return a successful ACK-backed CQE for every
message. Node B was then stopped cleanly so its final per-member, ICRC, RTT,
drop, error and Flow counters could be recorded.

## Workload construction

| Payload | Segments per SEND | SEND messages | Outstanding window |
|---:|---:|---:|---:|
| 64 B | 1 | 10,000,000 | 32 |
| 256 B | 1 | 10,000,000 | 32 |
| 1 KiB | 1 | 10,000,000 | 8 |
| 4 KiB | 4 | 2,500,000 | 2 |
| 16 KiB | 16 | 625,000 | 1 |
| 64 KiB | 64 | 156,250 | 1 |

The window controls registered-buffer ownership, not rate. A slot is reused
only after its sender CQE and all five receiver CQEs. The smaller windows for
multi-segment messages avoid queuing several RNIC segment trains at once while
remaining unpaced.

## End-to-end results

Throughput below is application payload throughput, not Ethernet line rate.
Sender RTT is measured from `ibv_post_send` to the successful sender CQE and
therefore includes segmentation, all five forward copies, the slowest receiver
ACK, Node B aggregation, and the sender RNIC completion path.

| Payload | Result | Payload Mbit/s | Duration ms | Sender RTT avg us | p50 us | p99 us | max us |
|---:|:---:|---:|---:|---:|---:|---:|---:|
| 64 B | PASS with overload/retries | 20.351934 | 251,573.141 | 804.662 | 1,084.890 | 1,147.064 | 134,044.216 |
| 256 B | PASS with overload/retries | 50.769036 | 403,395.491 | 1,290.295 | 1,581.313 | 1,639.023 | 33,097.552 |
| 1 KiB | PASS, loss-free | 94.697752 | 865,068.054 | 690.345 | 773.434 | 782.652 | 5,985.594 |
| 4 KiB | PASS, loss-free | 95.285704 | 859,730.230 | 683.334 | 686.019 | 1,005.504 | 4,723.401 |
| 16 KiB | PASS, loss-free | 96.006309 | 853,277.257 | 1,348.438 | 1,347.777 | 1,357.191 | 6,457.793 |
| 64 KiB | PASS, loss-free after backpressure fix | 97.182017 | 842,954.311 | 5,328.461 | 5,324.609 | 5,355.020 | 10,500.452 |

Every terminal PASS had exact CQ parity:

- sender CQEs equaled the SEND-message count;
- each of five receiver QP CQEs equaled the SEND-message count;
- total receiver CQEs equaled five times the SEND-message count;
- every received payload matched exactly.

## Node B metrics

ICRC time is one full recomputation for one transmitted member copy. Node B RTT
is measured from a forward packet's Node B ingress timestamp to the newly
aggregated ACK frontier emitted toward the sender. It is not the same interval
as sender CQE RTT. Percentiles are histogram bucket upper bounds.

| Payload | Forward ICRC samples | ICRC avg ns | min ns | p50 upper ns | p99 upper ns | max ns | Node B RTT samples | RTT avg ns | min ns | p50 upper ns | p99 upper ns | max ns |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 B | 50,772,823 | 1,396 | 1,380 | 1,703 | 1,703 | 38,422 | 10,000,000 | 12,757 | 5,640 | 13,650 | 27,304 | 900,271 |
| 256 B | 50,942,611 | 3,806 | 3,780 | 6,823 | 6,823 | 90,325 | 10,000,000 | 27,465 | 14,844 | 27,304 | 54,613 | 2,455,892 |
| 1 KiB | 50,000,000 | 13,440 | 13,380 | 13,650 | 27,304 | 59,196 | 10,000,000 | 256,228 | 84,605 | 436,929 | 873,863 | 2,585,320 |
| 4 KiB | 50,000,000 | 13,460 | 13,380 | 13,650 | 27,304 | 97,639 | 3,750,008 | 162,179 | 72,114 | 109,229 | 436,929 | 1,920,096 |
| 16 KiB | 50,000,000 | 13,444 | 13,380 | 13,650 | 27,304 | 117,617 | 1,250,000 | 86,715 | 57,490 | 109,229 | 109,229 | 2,505,245 |
| 64 KiB | 50,000,000 | 13,447 | 13,380 | 13,650 | 27,304 | 59,033 | 312,501 | 86,961 | 82,748 | 109,229 | 109,229 | 2,516,682 |

The number of Node B RTT samples follows aggregated ACK-frontier advances, not
SEND-message count. ACK coalescing and RC ACK cadence therefore make it vary
with segmentation.

### Clean-run forwarding evidence

The 1 KiB, 4 KiB, 16 KiB and final 64 KiB runs each recorded:

- 10,000,000 controlled forward data packets (plus at most a few unrelated or
  startup packets in the Flow/RX counters);
- exactly 10,000,000 submissions and 10,000,000 completions for each of five
  members;
- 50,000,000 forward ICRC recomputations;
- zero member drops, TX errors, pending buffers, malformed ACKs, RTT overwrites
  and RTT misses.

The final 64 KiB run specifically recorded 10,000,001 packets on its dominant
forward queue, 10,000,000 completed copies per member, 10,000,039 packets in
the forward Flow counter, and no datapath drops or pending buffers.

### 64 B and 256 B overload qualification

The 64 B and 256 B harnesses eventually completed every SEND, but window 32
overran the original TX completion-service cadence. They are completion passes,
not loss-free datapath passes:

| Payload | Forward Flow matches | Replica submissions | Fan-out drops |
|---:|---:|---:|---:|
| 64 B | 14,259,178 | 50,772,823 | 20,522,927 |
| 256 B | 14,307,685 | 50,942,611 | 20,595,714 |

The excess input matches and ICRC samples are RC retry amplification. Their
throughput and RTT values describe the requested unpaced overloaded behavior;
they must not be presented as loss-free capacity.

## Conclusion

The clean 1 KiB through 64 KiB points converge near 95–97 Mbit/s application
payload throughput because every run performs the same ten million input
packets and fifty million full member-copy ICRC recomputations on one RSS
worker. Full 1,024-byte packet ICRC costs about 13.44 microseconds per member
copy. The experiment therefore confirms that full software ICRC and five-way
fan-out, not RNIC segmentation or sender pacing, are the present single-flow
capacity limit.

The 64 KiB test also exposed and validated a necessary sustained-burst
backpressure correction. The 64 B and 256 B results remain useful overload
measurements, but a clean small-packet capacity claim requires repeating them
with the final completion-driven backpressure image and a window selected to
avoid retry amplification.

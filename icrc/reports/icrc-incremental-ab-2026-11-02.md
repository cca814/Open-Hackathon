# Incremental ICRC A/B experiment — 2026-11-02

## Outcome

PASS for the complete six-point `dpdk-neon` versus pure `incremental` A/B
sweep. Every point processed exactly 10,000,000 forward wire packets and five
member copies per packet. Sender and all five receiver CQEs had exact parity
and payload validation passed.

The controlled IPv4 benchmark set `NODE_B_IPV4_UDP_CHECKSUM=0` in both arms.
Node B logged `IPv4 UDP checksum rewrite=disabled` at every startup, explicitly
emitted the legal IPv4 UDP no-checksum value zero, and skipped output UDP
checksum recomputation. IPv6 remains checksummed and ignores this option. The
normal default remains enabled.

Across the final 12 samples Node B recorded zero member drops/errors, TX
errors, pending buffers, bad input ICRCs, unknown groups, invalid/stale/malformed
ACKs, RTT misses/overwrites, or incremental verification failures. One initial
4 KiB NEON sample had a single retry and was rejected; the isolated replacement
sample below had exact counts and no anomaly.

## Workload

Both arms used the 2026-10-31 workload construction:

| Payload | Segments/SEND | SENDs | Window | Wire packets |
|---:|---:|---:|---:|---:|
| 64 B | 1 | 10,000,000 | 32 | 10,000,000 |
| 256 B | 1 | 10,000,000 | 32 | 10,000,000 |
| 1 KiB | 1 | 10,000,000 | 8 | 10,000,000 |
| 4 KiB | 4 | 2,500,000 | 2 | 10,000,000 |
| 16 KiB | 16 | 625,000 | 1 | 10,000,000 |
| 64 KiB | 64 | 156,250 | 1 | 10,000,000 |

`dpdk-neon` fully recomputed every output ICRC with DPDK's Arm NEON backend.
`incremental` corrected the received ICRC from the changed destination IP,
QPN, and translated PSN. Both arms retained full NEON validation of every input
ICRC, so the experiment isolates output recomputation rather than disabling
input integrity checking.

## End-to-end A/B result

| Payload | NEON Mbit/s | Incremental Mbit/s | Gain | NEON duration ms | Incremental duration ms |
|---:|---:|---:|---:|---:|---:|
| 64 B | 98.435786 | 110.046163 | 1.12x | 52,013.604 | 46,525.929 |
| 256 B | 201.819260 | 406.418275 | 2.01x | 101,476.935 | 50,391.435 |
| 1 KiB | 295.058605 | 1,163.686409 | 3.94x | 277,639.759 | 70,396.972 |
| 4 KiB | 308.873437 | 845.858076 | 2.74x | 265,221.900 | 96,848.398 |
| 16 KiB | 296.941766 | 909.857464 | 3.06x | 275,879.009 | 90,036.081 |
| 64 KiB | 309.109182 | 1,023.152999 | 3.31x | 265,019.626 | 80,066.227 |

Throughput is application payload throughput, not Ethernet line rate. Every row
reported `receiver_parity=true sender_parity=true` and exact payload bytes.

## Output ICRC timing

| Payload | NEON avg ns | Incremental avg ns | Timing gain | NEON p50/p99 upper ns | Incremental p50/p99 upper ns |
|---:|---:|---:|---:|---:|---:|
| 64 B | 386 | 26 | 14.85x | 423 / 423 | 50 / 50 |
| 256 B | 1,066 | 26 | 41.00x | 1,703 / 1,703 | 50 / 50 |
| 1 KiB | 3,780 | 26 | 145.38x | 6,823 / 6,823 | 50 / 50 |
| 4 KiB | 3,780 | 26 | 145.38x | 6,823 / 6,823 | 50 / 50 |
| 16 KiB | 3,776 | 26 | 145.23x | 6,823 / 6,823 | 50 / 50 |
| 64 KiB | 3,776 | 26 | 145.23x | 6,823 / 6,823 | 50 / 50 |

Each row contains 50,000,000 forward ICRC samples. Incremental timing is nearly
payload-size independent because it touches only changed bytes and a fixed
number of correction-table entries. NEON full recomputation plateaus once the
RC path MTU segments large SENDs into 1 KiB packets.

## Sender RTT

| Payload | NEON avg us | Incremental avg us | Improvement | Incremental p50 us | Incremental p99 us |
|---:|---:|---:|---:|---:|---:|
| 64 B | 166.074 | 148.481 | 1.12x | 148.608 | 188.954 |
| 256 B | 323.842 | 160.446 | 2.02x | 158.349 | 212.896 |
| 1 KiB | 220.159 | 54.981 | 4.00x | 54.313 | 64.600 |
| 4 KiB | 207.756 | 73.060 | 2.84x | 72.374 | 84.031 |
| 16 KiB | 424.658 | 127.332 | 3.34x | 126.017 | 135.498 |
| 64 KiB | 1,629.913 | 446.297 | 3.65x | 441.531 | 453.073 |

Rare maximum RTT outliers of hundreds of milliseconds occurred in both arms;
the p50/p99 and ten-million-packet duration are more representative of steady
behavior on this shared system.

## Interpretation

Pure incremental output ICRC is validated for this controlled IPv4 RC fan-out
path. It improves application throughput by 1.12x–3.94x and reduces the measured
output ICRC operation from 386–3,780 ns to 26 ns. The smaller end-to-end gain at
64 B shows that task allocation, copies, TX completion service, ACK processing,
and full input ICRC validation now dominate short-packet performance.

The result does not justify disabling IPv4 UDP checksums generally. The switch
is explicit, startup-audited, defaults on, and was used only to remove UDP
checksum work as a confounder. A separate A/B with checksum enabled is required
before attributing any additional gain to that switch. IPv6 was not tested in
this experiment and always retains its mandatory UDP checksum.

## Artifacts and cleanup

- Raw per-point harness, Node B and CPU logs:
  `reports/icrc-ab-raw-2026-11-02/`
- Reusable failure-cleaning runner: `tools/run_icrc_ab_sweep.sh`
- IPv4 checksum control: `NODE_B_IPV4_UDP_CHECKSUM` (default `1`)

Node B was stopped gracefully after every point. The final runner cleanup
removed the container and temporary live-QP JSON from both target checkouts.
Project OVS flows and the temporary permanent neighbor require interactive
operator cleanup.

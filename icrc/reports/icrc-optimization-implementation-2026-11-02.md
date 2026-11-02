# ICRC optimization implementation — 2026-11-02

## Implemented

- A portable canonical CRC-32 backend remains the reference implementation.
- Hardware builds contain a DPDK `RTE_NET_CRC_NEON`/`RTE_NET_CRC32_ETH`
  backend with startup differential self-tests from 1 through 1,536 bytes.
- The worker owns a const backend reference; the DPDK context is created after
  EAL and destroyed after workers/TX contexts drain.
- The full backend temporarily uses the eight bytes before the parsed IP header
  as the virtual LRH, normalizes invariant fields in worker-owned memory, and
  restores all bytes before TX.
- Forward and ACK paths reuse their existing validated parse result.
- Incremental correction uses precomputed reflected CRC-32 contributions indexed
  by byte value and distance from the ICRC trailer. It covers changed source and
  destination IP bytes, destination QPN, and PSN. Ethernet and invariant-masked
  checksum bytes are excluded.
- `incremental-verify` compares each correction with the selected full backend;
  `incremental` is available for later gated measurements but is not the default.

## Portable verification

`./tests/run_local.sh` passed after integration. This includes:

- the existing IPv4/IPv6 Python wire oracle;
- existing worker ownership, bounds, group, fan-out, ACK aggregation, PSN wrap,
  metric, and checksum tests;
- full-backend differential and byte-restoration tests across packet lengths;
- incremental correction differential tests across IPv4/IPv6 lengths;
- `incremental-verify` exercised through IPv4 fan-out and IPv6 aggregate-ACK
  worker paths.

A clean local Meson build was not available because this WSL environment has no
`meson` executable. The shell test build uses strict C11 warnings with `-Werror`.

## Hardware validation

The corrected nested-SSH audit found no active packet, RDMA-test, or Node B
workload on either Arm DPU. Dormant desktop/session helpers on the jump host
were not DPU workloads.

The Node B image built natively against DOCA `3.5.0098` and DPDK `26.03.0.5`.
All ten in-image Meson tests passed. A bounded no-traffic launch selected:

```text
ICRC backend=dpdk-neon-full strategy=incremental-verify
port0 PCI=03:00.0 DPDK-port=6
port1 PCI=03:00.1 DPDK-port=7
Eight bidirectional workers running
```

The dynamic DPDK IDs differed from the earlier 2/3 mapping, confirming that the
application derives rather than assumes them. Shutdown exited 0. Every queue
had zero TX errors, zero pending buffers, and zero `icrc_verify_fail`. Two
unrelated forward frames were rejected as malformed and two reverse frames
were forwarded unchanged; no valid group rewrite occurred during this smoke
run.

The first wire attempts could not start. Two five-QP attempts failed before
`READY` with `receiver0 RTR failed: Connection timed out`. The independently
compiled single-receiver harness failed at the same QP-setup stage. Both RDMA
ports and their SF links reported ACTIVE with MTU 1024 and the expected IPv4
addresses. The documented OVS setup was attempted first, but `sudo -n` required
an interactive password and therefore changed nothing. Missing project OVS
routes and one missing reciprocal neighbor were prerequisite failures, not ICRC
defects. After the operator installed the project OVS routes and the permanent
`10.0.1.1 -> 02:96:42:f8:5e:f2` neighbor on the receiver SF, the same harness
reached `READY` and the bounded wire run passed.

The successful run sent 10,000 one-segment 1 KiB messages through five live
receiver QPs. Every receiver produced 10,000 CQEs, the sender produced 10,000
CQEs, and every payload matched. Node B submitted and completed 50,000 replicas
with no errors or drops. All 50,000 forward and 10,000 aggregate-ACK
`incremental-verify` computations agreed with DPDK NEON full recomputation;
`icrc_verify_fail` remained zero.

Cleanup removed the stopped Compose container, the temporary diagnostic binary,
and every ephemeral live-QP JSON copy. No Node B or QP-harness process remained.
The operator must still run the documented cookie recovery and delete the
temporary permanent neighbor because noninteractive `sudo` cannot authenticate.

Use the prior incremental payload-size sequence (64 B, 256 B, 1 KiB, 4 KiB, 16 KiB, and
64 KiB) for controlled A/B runs of
`portable`, `dpdk-neon`, `incremental-verify`, and finally `incremental`.

# ICRC optimization plan

This plan keeps protocol correctness and independent wire verification as hard
gates.  A faster checksum is not accepted merely because it compiles or agrees
with itself.

## Status — 2026-11-02

- Stages 1 and 2 are implemented. Portable tests cover backend equivalence,
  temporary-byte restoration, parser reuse, fan-out, and ACK aggregation.
- The Stage 3 correction table and explicit `incremental-verify`/`incremental`
  strategies are implemented. Portable fan-out and ACK tests run in verification
  mode and compare every correction to full recomputation.
- The hardware default remains `dpdk-neon`; `incremental` is not accepted for
  target use until Stage 4 passes.
- Stage 4 target build and no-traffic startup passed on Node B: DOCA 3.5.0098,
  DPDK 26.03.0.5, all ten image tests, the DPDK NEON startup self-test, Flow
  initialization, eight bidirectional workers, and clean teardown. Bounded
  wire validation passed after the Node A OVS routes and reciprocal
  neighbor were installed interactively. A 10,000-packet 1 KiB five-member RC
  run completed with exact sender/receiver CQE parity and zero incremental/full
  verification mismatches. The full 64 B through 64 KiB verification sweep then
  passed at ten million wire packets per point: 300 million forward replica
  comparisons plus 36,313,133 aggregate-ACK comparisons produced zero
  mismatches, drops, TX errors, or pending buffers. The isolated six-point
  `dpdk-neon` versus pure `incremental` A/B sweep also passed at ten million
  wire packets per point with IPv4 UDP checksum disabled equally in both arms.
  Pure incremental reduced measured output ICRC time from 386–3,780 ns to 26 ns
  and improved application throughput by 1.12x–3.94x. A checksum-enabled A/B
  remains optional for separating the checksum switch from the ICRC result.

## Stage 1 — swappable full-recompute backend

- Keep the existing bitwise implementation as the portable reference backend.
- Add a narrow canonical CRC-32 backend interface, owned outside packet parsing.
- In hardware builds, create a DPDK `rte_net_crc` context with
  `RTE_NET_CRC_NEON` and `RTE_NET_CRC32_ETH` after EAL initialization.
- Verify the DPDK result against the portable backend during startup.  Fall back
  visibly to the portable backend if NEON is unavailable or fails verification.
- Share only an immutable/thread-safe backend context across the eight workers;
  destroy it after workers and TX contexts have drained.

## Stage 2 — remove redundant hot-path work

- Parse each incoming packet once and pass the validated offsets to ICRC
  validation and rewrite helpers.
- Build the virtual eight-byte all-ones LRH prefix in the eight bytes preceding
  the IP header, temporarily normalize invariant fields in worker-owned packet
  memory, calculate one contiguous CRC, and restore every byte before TX.
- Retain a portable path that does not depend on DPDK, EAL, or target hardware.
- Measure input validation separately from output recomputation; keep validation
  enabled by default.

## Stage 3 — incremental correction backend

- Treat CRC correction as a separate interchangeable backend, not as a change
  to packet parsing or TX ownership.
- Correct only fields covered by ICRC that Node B changes: IPv4/IPv6 addresses,
  BTH destination QPN, and translated PSN.  Ethernet addresses are outside the
  ICRC and the IP/UDP checksum fields are invariant-masked.
- First validate a hybrid mode that fully recomputes one member and checks every
  incremental member result.  Do not enable the incremental backend by default
  until independent Python vectors, captures, and ConnectX acceptance agree.
- Use wraparound-safe packet-length/offset tables and explicit bounds; never
  infer that equal payloads imply equal CRC corrections at different lengths.

## Stage 4 — target benchmarking and acceptance

- Reuse the previous incremental payload-size experiment and its exact counters:
  64 B, 256 B, 1 KiB, 4 KiB, 16 KiB, and 64 KiB messages, including the existing
  multi-member fan-out and ACK path.
- Record backend name, ICRC samples, average/min/p50/p99/max, packet rate, drops,
  TX completions/errors, pending buffers, and worker utilization.
- Run differential portable-versus-NEON tests before traffic, then the Python
  oracle, controlled captures, receiver CQEs, and sender completion checks.
- Before using either shared DPU, inspect logged-in users and running workload.
  If another team appears active, postpone hardware testing.  For every started
  run, stop the service gracefully, drain TX, remove only project-owned OVS
  flows if installed, and confirm no project process/container remains.

## Deferred experiments

- Compare the in-place unaligned canonical span against an aligned per-worker
  scratch copy on the Cortex-A78AE; retain whichever wins on measured hardware.
- Consider sampled input-ICRC validation only after the always-validate path is
  characterized.  The safe default remains validation of every input.
- Do not use global `-mcpu=native`; isolate any custom Arm intrinsic translation
  unit and retain runtime capability/fallback checks.

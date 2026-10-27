# Agent Instructions — BlueField-3 RoCEv2 Node B MVP

## 1. Mission

Implement **Node B**, a BlueField-3 DPU packet-processing middlebox for a minimal RoCEv2 proof of concept.

Use **DOCA Flow for ingress steering into RSS queues**, **eight DPDK workers on the DPU Arm cores** for packet parsing, rewriting and full ICRC recomputation, and **DOCA ETH TX** for transmission.

The first success target is:

> A ConnectX-generated RoCEv2 RC SEND packet enters Node B on physical port `p0`, is steered to DPDK, has its BTH Destination QPN rewritten, receives a correctly recalculated ICRC, leaves on `p1`, and is accepted by a correctly configured ConnectX receiver.

**Do not attempt P4 integration, hardware QPN rewrite, DPA, or incremental ICRC in the initial MVP.** These are future extensions.

## 2. Reference topology and responsibilities

```text
Node A — BlueField-3 (RDMA endpoints, no DOCA application)

  RDMA sender SF / ConnectX
      -> OVS ovsbr1 (hardware-offloaded eSwitch)
      -> A.p0 (100 GbE)
         |
         | RoCEv2 RC SEND traffic
         v
Node B — BlueField-3 (DOCA application on Arm)

  B.p0 / mlx5_0 (RX)
      -> DOCA Flow: match/steer UDP destination port 4791 into RSS
      -> DPDK RX queues / Arm worker pool
      -> parse -> optionally validate input ICRC -> rewrite BTH QPN
         -> recompute output ICRC -> DOCA ETH TX
      -> B.p1 / mlx5_1 (TX)
         |
         v
  A.p1 (100 GbE)
      -> OVS ovsbr2 (hardware-offloaded eSwitch)
      -> RDMA receiver SF / ConnectX

  RC ACK return: A.p1 -> B.p1 -> unchanged UDP/4791 reverse worker
                         -> B.p0 -> A.p0 -> sender SF / ConnectX
```

Node A establishes and manages the RDMA RC endpoints and QPs. Node B is a **packet processing network function**, not an RDMA endpoint and does not establish QPs. Node B should not rely on a DOCA RDMA API for this MVP.

Keep the following distinction explicit:

- **DOCA Flow:** ingress classification, flow steering, RSS, and supported forwarding rules.
- **DPDK workers:** receive packet buffers (`rte_mbuf`) from RSS queues, create per-member copies when needed, rewrite headers and recompute ICRC.
- **DOCA ETH TX:** submits modified packets and reports completion; workers retain TX buffers until completion.
- **ConnectX RDMA transport:** creates original ICRC on send and validates incoming RoCEv2 packets on receive.

## 3. Technical stack

- Hardware: NVIDIA BlueField-3 DPU with two 100 GbE ports.
- SDK target: NVIDIA DOCA **3.5.0098**, verified on the Arm runtime (2026-10-26/27; Ubuntu 24.04.4 LTS). Native WSL SSH aliases `nvaitc-1` and `nvaitc-2` both reach their jump hosts, and `ssh DPU` from either jump host reaches its associated Arm DPU. Check installed headers, library versions, device mode, and firmware before coding; never infer APIs from another DOCA release.
- DOCA Flow API for steering/RSS.
- DPDK **26.03.0.5** (installed SDK metadata) + mlx5 PMD, EAL and eight RSS RX queues.
- DOCA ETH TX queues and completion handling, owned by individual workers.
- C, with Meson/Ninja build setup unless the existing repository specifies otherwise.
- Eight Linux Arm lcores, each polling the same queue ID on both ports and
  servicing one TX context per direction. Forward traffic may rewrite/fan out;
  reverse UDP/4791 traffic is forwarded unchanged for ACK return.
- libibverbs/perftest or an existing RC SEND test harness on Node A, separately from the Node B application.

**Privilege policy:** Never use a privileged Docker container, host PID/mount
namespace entry, or a similar container workaround to execute privileged host
commands. Ask the user before every required `sudo`, root, privileged-container,
or equivalent operation. Run `tools/configure_node_a_ovs.sh` directly under an
explicitly approved `sudo`; its `recover` action removes only cookie
`0x49435243` flows. Starting the Node B Compose service is itself privileged and
also requires explicit approval.

**Startup invariant:** Verify compatible DOCA Flow, DPDK RX and DOCA ETH TX device ownership against installed SDK samples on the actual BlueField. Define the mbuf-to-TX-buffer handoff explicitly; start with a copy into DOCA-compatible TX memory if needed, rather than assuming zero-copy interoperability. This development machine is not the runtime target.

## 4. Scope

### In scope

1. Receive RoCEv2 traffic on B.p0 and steer it to DPDK RX queues.
2. Parse Ethernet, optional VLAN, IPv4 (including options) or IPv6 where supported, UDP, BTH, transport extension headers, payload, pad, and ICRC.
3. Identify RoCEv2 packets (typically UDP destination port 4791) and safely reject malformed/truncated packets.
4. Rewrite the **24-bit BTH Destination QPN** based on an explicit test mapping.
5. Recompute RoCEv2 **ICRC** according to the protocol's invariant-field rules; do not substitute Ethernet FCS or a naive CRC over the entire Ethernet frame.
6. TX modified packets on B.p1 and track counters/errors.
7. Prove end-to-end acceptance by Node A's hardware RDMA receiver.
8. Load multicast groups from a local file and replicate to their configured members.
9. Record throughput, packet rate, drops and worker CPU usage for the eight-worker datapath.

### Out of scope for the first MVP

- P4 integration and RC ACK aggregation/reliability coordination. File-configured packet fan-out is in scope; reliable RC multicast is not implied by replication.
- QP establishment/management within Node B.
- DPA, CUDA/GPU, and direct hardware BTH rewrite.
- Incremental ICRC correction (implement as a later interchangeable backend).
- Payload rewriting (introduce only after QPN-only rewrite works).
- Claiming 100 Gb/s throughput without a measurement.

## 5. Protocol correctness constraints

### BTH field layout (BTH-relative offsets)

| Bytes | Field |
|---|---|
| `0` | Opcode |
| `1` | Flags (including solicited/migration/padding/version bits) |
| `2–3` | P_Key |
| `4` | Reserved |
| `5–7` | **Destination QPN (24-bit, network byte order)** |
| `8` | ACK request / reserved bits |
| `9–11` | Packet Sequence Number (PSN, 24-bit) |

The BTH is 12 bytes. For the restricted Ethernet II + IPv4 without options + UDP case, BTH begins at frame offset `14 + 20 + 8 = 42`, and the QPN occupies frame offsets **47–49**. **Never hard-code 47 for general traffic.** Derive offsets from validated headers; account for VLAN, IPv4 IHL, and IPv6 extension headers as appropriate. Reject unsupported encapsulation explicitly.

Example byte rewrite after bounds-checking and locating `bth`:

```c
static void rewrite_dest_qpn(uint8_t *bth, uint32_t qpn)
{
    qpn &= 0x00ffffffU;
    bth[5] = (uint8_t)(qpn >> 16);
    bth[6] = (uint8_t)(qpn >> 8);
    bth[7] = (uint8_t)qpn;
}
```

**Do not silently truncate an invalid configured QPN in production logic.** Validate `qpn <= 0xFFFFFF` at configuration time. The above helper only illustrates field encoding.

### RDMA transport and connection state

A rewritten destination QPN must identify an **existing, correctly configured receiver QP**. A change in QPN does not establish a new RC connection. The receiver also expects compatible PSN, opcode, P_Key, and any required extended transport headers and connected-peer state. For the first valid end-to-end test, arrange the sender and receiver QPs explicitly so the only necessary on-wire rewrite is Destination QPN. If this cannot be achieved, document the additional fields and control-plane work before implementing them.

Do not confuse a successfully captured packet with an RDMA SEND completion or with the destination QP accepting it.

### RoCEv2 ICRC

- ICRC is **not** Ethernet FCS or UDP checksum.
- Use a protocol-correct implementation of RoCEv2 ICRC, including invariant normalization of specified IP/UDP and transport fields. Verify behavior with independent golden vectors/captures before packet modification.
- Do not include Ethernet headers/FCS indiscriminately in the ICRC input.
- Recompute the last four ICRC bytes after BTH rewriting, respecting wire byte order and padding.
- Avoid unintended TX checksum offloads or metadata that changes the bytes used in ICRC validation.
- If changing IP/UDP headers later, separately determine and update relevant IP/UDP checksums, as required by the chosen IP version/protocol.
- Input ICRC validation is a useful diagnostic but may be made configurable if it is too expensive for the final datapath.

**Engineering gate:** Do not claim an ICRC implementation is correct without comparing it against an independent oracle (preferably ConnectX-generated packets and receiver-side hardware acceptance).

## 6. Hardware/API uncertainty — do not invent capabilities

### DOCA Flow and Destination QPN

DOCA Flow documents actions for a set of parsed fields (including usual L2/L3/L4 fields), but this project has **not established a supported native action** for `RoCEv2 BTH Destination QPN`.

Do **not** write imaginary API members such as:

```c
/* Not a verified API. Do not use. */
// actions.outer.roce.bth.dest_qp = new_qpn;
```

DPDK `rte_flow` Flex Parser/FLEX ITEM with `MODIFY_FIELD` may offer a future hardware route on some mlx5 configurations; **this is unverified for this exact BF3 firmware, DPDK/DOCA combination and parser profile**. It is not a prerequisite for the MVP. Never assume a standalone `rte_flow` rule can be mixed safely with DOCA Flow's port/flow ownership.

### Critical RX steering risk

RoCEv2 packets can be consumed by an RDMA hardware transport pipeline rather than ordinary Ethernet RX queues. Creating a DOCA Flow rule successfully is **not proof** that RoCEv2 packets arrive in DPDK. Test actual wire-origin UDP/4791 reception and counters on B.p0 before implementing the ICRC engine.

### Physical port / eSwitch setup

Confirm `p0`/`p1`, mlx5 port indices, DPDK port IDs, SF/VF/representor ownership, switch mode, and any `isolated`/default-forwarding settings on the real BF3. Do not assume `mlx5_0` and `mlx5_1` are DPDK port 0 and port 1. Verify that B's RX and TX paths are feasible with the selected steering mode, and that Node A's OVS hardware offload remains functional.

## 7. Implementation milestones and acceptance criteria

### Milestone 1 — Flow → eight workers → ETH TX

- On the target BlueField, confirm SDK versions and physical RX/TX mapping as part of bring-up.
- DOCA Flow matches RoCEv2 UDP/4791 and steers into eight DPDK RSS queues.
- Each worker polls one RX queue and submits unchanged packets through its own DOCA ETH TX context, including completion processing and buffer reclamation.
- Count RX, TX submissions, TX completions, errors and drops; define unmatched traffic policy.

**Done when:** ConnectX-originated packets enter p0 and leave p1 unchanged. Wire captures and counters agree, and sustained traffic does not leak buffers or stall on TX backpressure. Exercise all eight queues with multiple flows; one RC flow need not spread across workers. Record the working target build/run command and port mapping.

### Milestone 2 — Header rewrite and full ICRC

- Parse a restricted initial format: Ethernet II, IPv4 without options, UDP and RC SEND. Explicitly reject unsupported or truncated packets.
- Rewrite the configured destination QPN and recompute the full ICRC before ETH TX.
- Test header offsets, QPN bounds, SEND extension headers, padding and ICRC against an independent oracle.

**Done when:** Captures show only the intended rewrite and new ICRC; independent verification agrees. A correctly configured single receiver accepts the packet and the RC SEND completes with a documented ACK return path. QP setup remains Node A's responsibility.

**Status: COMPLETE (2026-10-27).** Independent IPv4 and IPv6 wire oracles agree,
and `tools/rc_qpn_rewrite_test.c` proved transport acceptance with live
ConnectX RC QPs. The sender targeted virtual QPN `1006`; Node B rewrote it to
receiver QPN `1005`. The receiver CQE reported success with the expected
40-byte payload, and the sender CQE completed only after the ACK traversed the
unchanged reverse path. Runtime QPNs are ephemeral and must be regenerated.

### Milestone 3 — File-configured groups and fan-out

- Load a group file at startup, mapping an incoming destination IPv4/QPN pair to a list of destination members.
- Create an independent writable packet per member; apply its destination MAC/IP/QPN, update affected checksums and recompute ICRC, then submit through the worker's ETH TX context.
- Validate the entire file before starting workers. Keep the table immutable while running; edit the file and restart to apply changes.
- Track per-member TX completions/errors and replication drops. Measure packet rate, goodput and drops with eight workers under a recorded workload.

**Done when:** A two-member group produces exactly two correctly addressed, independently checksum-verified wire packets per input under a no-loss test load. Unknown groups are counted and dropped; invalid configuration prevents startup; TX pressure is handled without shared-buffer corruption or leaks. Record receiver observations separately: packet fan-out alone does not demonstrate reliable multicast RC completion.

## 8. Proposed source layout and group control

Keep modules small and aligned with the packet path; the same worker handles RX, processing and TX completions.

```text
icrc/
├── AGENTS.md
├── README.md
├── meson.build
├── config/
│   └── groups.example.json
├── include/               # module headers: flow, worker, eth_tx, dpdk_rx, roce, icrc, groups
├── src/
│   ├── main.c              # startup, eight workers, shutdown
│   ├── flow.c      # ingress match and RSS steering
│   ├── worker.c  # DPDK RX, replication, rewrite, ICRC, TX loop
│   ├── eth_tx.c  # DOCA ETH TX buffers, submissions, completions
│   ├── roce.c      # bounds-checked parsing and header rewriting
│   ├── icrc.c      # full ICRC calculation, independent of I/O APIs
│   └── groups.c  # file parsing, validation, immutable lookup table
├── tests/                  # parser, ICRC vectors and group-file tests
└── tools/                  # target discovery and traffic/capture checks
```

Proposed startup interface: `--groups config/groups.json`. All members initially use the configured p1 egress. A single-member group also provides the mapping for Milestone 2.

```json
{
  "groups": [
    {
      "id": "demo",
      "match": {"dst_ipv4": "192.0.2.100", "dst_qpn": 9000},
      "members": [
        {"dst_mac": "02:00:00:00:00:01", "dst_ipv4": "192.0.2.1", "dst_qpn": 101},
        {"dst_mac": "02:00:00:00:00:02", "dst_ipv4": "192.0.2.2", "dst_qpn": 102}
      ]
    }
  ]
}
```

These are illustrative addresses and QPNs; use the receiver QPs actually created on Node A. Reject duplicate group IDs/match keys, duplicate members, empty groups, malformed addresses and QPNs outside 24 bits. Set explicit group/member limits. No watcher, RPC service or hot reload is needed initially. Preserve per-flow ordering; this table does not manage PSNs, QP state, ACK aggregation or retransmissions.

## 9. Instructions to coding agents

1. **Inspect before editing.** Read the existing repository, build files, current hardware configuration, SDK headers, and available examples. Do not assume an empty project.
2. **Work in milestones.** Implement one milestone at a time; report its verification result and remaining blockers.
3. **Prioritize compilable and measurable code.** No fake success paths, fabricated throughput numbers, or undocumented DOCA APIs.
4. **Clearly label assumptions.** Especially DOCA version differences, mlx5 steering domains, RoCEv2 packet layout, ICRC normalization, and hardware offload behavior.
5. **Keep packet correctness separate from transport acceptance.** Captures, checksum unit tests, and actual RDMA SEND success are distinct gates.
6. **Use safe mbuf access.** Handle non-contiguous packets, truncation and ownership. Release RX mbufs after processing/copying; release DOCA TX buffers only after completion, or after a failed submission that transferred no ownership.
7. **Avoid undefined concurrency.** Explicitly assign queues to workers; minimize shared mutable state and use safe counters/control-plane updates.
8. **Make ICRC swappable.** Keep a narrow API for current full recomputation and future incremental correction without tying it to DPDK-specific packet descriptors unnecessarily.
9. **Preserve Node B's responsibility boundary.** It receives and forwards full packets in this MVP. A future P4 architecture may send metadata only, but do not prematurely implement a metadata-only protocol.
10. **Ask or document blockers** if real firmware/API capabilities or a Node A RDMA test arrangement are required for the next step.

## 10. Recommended first task

Before implementing QPN rewriting, build the **DOCA Flow → eight DPDK workers → DOCA ETH TX baseline** that receives actual RoCEv2 UDP/4791 frames on B.p0 and sends them unchanged through B.p1. Record the verified SDK initialization, RX-to-TX buffer handoff, port mapping, build/run commands and RX/TX completion counters. If steering fails, investigate NIC RDMA consumption, representor mapping, and isolated-switch configuration before adding more features.

### Runtime SSH paths

From WSL, use the native SSH client and the configured aliases. Both paths were
verified non-interactively on 2026-10-26:

```sh
ssh nvaitc-1
ssh DPU
```

```sh
ssh nvaitc-2
ssh DPU
```

`nvaitc-1` resolves to jump host `NVAITC-TPWS-1`, while `nvaitc-2` resolves to
`NVAITC-TPWS-2`. Each jump host's `DPU` alias reaches a distinct `aarch64` DPU
running DOCA `3.5.0098`. Prefer these native WSL aliases over invoking the
Windows OpenSSH executable. Authentication remains in the user's SSH
configuration; do not copy credentials or private key material into this
repository.

### Verified Node B runtime

Node B is the DPU reached through `ssh nvaitc-2`, then `ssh DPU`. Its validated
checkout is `/home/mtchang/Open-Hackathon/icrc`. On 2026-10-27:

- `03:00.0` (RX) and `03:00.1` (TX) mapped to DPDK ports 2 and 3.
- `/dev/hugepages` was mounted as hugetlbfs with eight free 512 MiB pages before
  launch. Use `HUGEPAGES_DIR=/dev/hugepages`; the container target remains
  `/mnt/huge`.
- The DOCA image build completed and all six Meson tests passed.
- A bounded no-group launch initialized eight RX queues and eight DOCA ETH TX
  contexts per port, one UDP/4791 RSS Flow pipe/counter per port, then reached
  `Eight bidirectional workers running` and shut down cleanly.
- DPDK 26.03 on BF3 requires mlx5 probe argument `txq_mem_algn=0` to use its
  legacy per-queue allocation path. DOCA Flow 3.5 requires port-scoped RSS and
  counter reservations. In this HWS path, fix IPv4/UDP using parser metadata;
  do not redundantly set `outer.l3_type` or `outer.l4_type_ext`, which causes
  pipe construction to fail with a protocol-only opcode error.
- `tools/raw_udp4791.py` provides an icrc-local physical-path diagnostic. Three
  individually logged frames were captured byte-for-byte from Node A p0 through
  Node B to Node A p1. A 256-frame/64-flow run captured all 256 and assigned 32
  controlled packets to each of the eight RX queues, with matching TX
  submissions/completions and no errors, drops or pending buffers. Use outer
  IPv4/UDP RSS flags for this non-tunneled traffic; inner flags forwarded all
  controlled flows to queue 0.
- The reverse physical path was validated separately: 256 tagged packets sent
  on Node A p1 were all captured on Node A p0 after Node B, with exactly 32
  controlled packets on each reverse queue and no TX errors, drops or pending
  buffers. This is the implemented RC ACK route.
- `tools/roce_wire.py` is an independent Python RC SEND/checksum oracle. A
  64-input run using the two-member example group captured exactly 128 outputs.
  Every complete frame matched the expected destination MAC/IP/QPN rewrite,
  IPv4 checksum, UDP checksum and full ICRC. Aggregate per-member counters were
  64 submitted/64 completed with zero errors or drops.
- The same oracle passed with IPv6: 64 inputs produced 128/128 exact member
  outputs, all eight queues handled eight controlled inputs, and each member
  completed 64/64 with valid mandatory UDP checksum and full IPv6 ICRC.
- Node A has hardware-offloaded `ovsbr1` joining sender SF representor
  `en3f0pf0sf0` to `p0`, and `ovsbr2` joining `p1` to the receiver SF
  representors. `tools/configure_node_a_ovs.sh setup` installs auditable L2
  routes under cookie `0x49435243`, including the reverse ACK path; `recover`
  removes only those project-owned flows. A controlled run counted exactly 64
  outputs to each receiver representor while Node B completed all 128 sends.
- A live single-receiver RC test used `mlx5_2` as sender and `mlx5_3` as
  receiver. The harness kept a distinct dummy QP alive as virtual QPN `1006`
  while receiver QPN `1005` was in RTR/RTS. Node B matched and rewrote the
  virtual QPN, completed one forward TX, and forwarded the ACK in reverse with
  zero TX errors or drops. Both receiver and sender CQEs succeeded and the
  received 40-byte payload matched exactly.

This validates startup, bidirectional physical forwarding, exact preservation,
all eight RSS queues, two-member wire rewrite/ICRC correctness, receiver-QP
acceptance, RC SEND completion, ACK return and cleanup. Milestone 2 is complete.
The remaining milestone work is the recorded sustained-load behavior and
throughput required by Milestone 3.

### IPv6 hardware path

Restricted IPv6 without extension headers or jumbograms is enabled through the
packet middlebox. One root control pipe classifies
`parser_meta.outer_l3_type` and forwards to non-root IPv4 and IPv6 UDP/4791 RSS
pipes; this satisfies DOCA Flow's one-root-per-port constraint. Constructor,
counter, controlled capture, two-member rewrite, mandatory UDP checksum and
full ICRC tests pass on DOCA 3.5.0098. The live RC QP test also passed over this
IPv6 path, including receiver acceptance and ACK-backed sender completion.

## 11. Primary references to check against installed version

- NVIDIA DOCA 3.5.0 API reference (release family for build 3.5.0098): https://docs.nvidia.com/doca/api/3-5-0/index.html. Check the installed build headers for exact signatures.
- DOCA 3.5 compatibility changes: https://networking-docs.nvidia.com/doca/archive/3-5-0/customer-affecting-changes.
- DPDK mlx5 PMD documentation matching the installed NVIDIA DPDK **26.03.0.5** build; do not assume the earlier 25.07 guide matches this target.
- DPDK `rte_flow` FLEX and MODIFY_FIELD documentation matching the installed DPDK release.
- InfiniBand Architecture Specification for BTH and invariant CRC rules; confirm precise RoCEv2 adaptations using verified implementations and on-wire test vectors.

Treat documentation as version-specific. Cite the exact relevant SDK header, sample, or reference when proposing hardware-dependent functionality.

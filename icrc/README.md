# Node B RoCEv2 middlebox

Runtime target: **DOCA 3.5.0098 on BlueField Arm**, verified by read-only SSH
through the Windows `nvaitc-1` configuration to `DPU` on 2026-10-21. The target
runs Ubuntu 24.04.4 LTS (`aarch64`). DOCA Common, Flow, ETH and the DPDK bridge
all report `3.5.0098`; installed DPDK metadata reports `26.03.0.5`. Neither
remote machine was modified.

Target packet path: DOCA Flow → eight DPDK RX workers → rewrite/full ICRC →
DOCA ETH TX. See [AGENTS.md](AGENTS.md) for the three milestones.

## Runtime connection

Verified on 2026-10-21. The VPN must be active to reach the private network.

| Item | Value |
| --- | --- |
| Windows SSH config | `C:\Users\currychang\.ssh\config` |
| Same config from WSL | `/mnt/c/Users/currychang/.ssh/config` |
| First-hop alias | `nvaitc-1` |
| First-hop address/user | `mtchang@192.168.3.7` |
| Second hop | `ssh DPU` from `nvaitc-1` |
| Verified target | Ubuntu 24.04.4 LTS, `aarch64`, DOCA `3.5.0098`, DPDK `26.03.0.5` |

From Windows PowerShell, use the existing SSH configuration and credentials:

```powershell
ssh nvaitc-1
# Once on nvaitc-1:
ssh DPU
```

From this WSL workspace, use the Windows SSH client so it resolves the Windows
alias and uses the existing Windows credentials. The native WSL SSH client did
not resolve `nvaitc-1` during verification. This command checks versions without
starting the application:

```sh
/mnt/c/Windows/System32/OpenSSH/ssh.exe -o BatchMode=yes -o ConnectTimeout=10 nvaitc-1 \
  'ssh -o BatchMode=yes -o ConnectTimeout=10 DPU "uname -m; cat /opt/mellanox/doca/VERSION; pkg-config --modversion doca-common doca-flow doca-eth doca-dpdk-bridge; sed -n /Version:/p /opt/mellanox/dpdk/lib/aarch64-linux-gnu/pkgconfig/libdpdk.pc"'
```

`DPU` is resolved by SSH on the first-hop machine; its underlying address and
user were not separately recorded. Authentication stays in the existing SSH
configuration; no credentials or key material are stored in this repository.

**Current remote-access scope is read-only inspection.** The container commands
below are operator instructions, not authorization for an agent to install,
build, configure devices or launch processes on either remote machine. Obtain
explicit authorization before those actions.

## Implementation status

Module headers live in `include/`; implementations live in `src/`.

The forwarding, rewrite and file-configured fan-out source paths are implemented. Hardware acceptance for the milestones remains pending. Implemented modules:

- `src/flow.c`: one Flow engine, explicit device/port binding, IPv4 UDP/4791
  matching into RSS queues 0–7, drop on miss, entry completion verification and
  hardware counter reads.
- `src/worker.c`: copy RX once into worker scratch, validate and look up the group,
  produce an independently rewritten TX buffer per member, count completions/drops
  and drain on shutdown. Omitting `--groups` keeps unchanged forwarding.
- `src/dpdk_rx.c`: burst polling, bounded multi-segment mbuf copying and release,
  and TX progress even when there are no incoming packets.
- `src/roce.c` and `src/icrc.c`: bounded RC SEND parsing, destination rewriting,
  full ICRC and IPv4/UDP checksum recomputation.
- `src/groups.c`: startup JSON validation and immutable destination-group lookup.
- `src/eth_tx.c`: per-worker DOCA TX queue, progress engine, registered storage,
  buffer inventory, send tasks, completion callbacks and ordered cleanup.

`src/main.c` connects device probing, actual port-ID discovery, RX queue setup,
eight DPDK lcore workers, signals, counters and bounded TX draining. The executable
and hardware adapters are source implementations, not locally compiled or validated:
this machine has no DOCA/DPDK headers or libraries. The portable parser, checksum,
group loader and worker fan-out are exercised by the local tests below.

## Build and local tests

With Meson and Ninja installed:

```sh
cd icrc
meson setup build
meson test -C build --print-errorlogs
```

The complete portable suite runs with a C compiler and Python standard library:

```sh
./tests/run_local.sh
```

It covers worker ownership and repeated buffer reuse; 400 synthetic frames across
RC SEND opcodes, padding, immediate data and varying payload lengths; independent
Python zlib and Internet-checksum verification; malformed/truncated input and
invalid group files; and two-member worker fan-out with queue exhaustion,
submission failure, TX completion errors and unknown groups. These are software
tests, not hardware acceptance or throughput measurements.

On the BlueField with matching SDK development packages, compile the adapters:

```sh
export PKG_CONFIG_PATH=/opt/mellanox/doca/lib/aarch64-linux-gnu/pkgconfig:/opt/mellanox/dpdk/lib/aarch64-linux-gnu/pkgconfig
pkg-config --modversion doca-common doca-flow doca-eth doca-dpdk-bridge libdpdk
meson setup build-bf3 -Dhardware=true
meson compile -C build-bf3
meson test -C build-bf3 --print-errorlogs
```

This produces `build-bf3/node_b`, hardware libraries and tests. Missing SDK
dependencies cause the hardware build to fail; no mock hardware backend is
substituted. After verifying port ownership, links and hugepages on the target:

```sh
sudo ./build-bf3/node_b <RX-PCI-BDF> <TX-PCI-BDF> <nine-available-lcores> \
  --groups config/groups.example.json
# Example lcore argument: 0-8 (only if these cores are available on the target).
```

The first enabled lcore is EAL's main core; eight other lcores poll queues.
Use distinct physical devices, not representors, for this initial VNF backend.
The initial receive configuration uses standard Ethernet MTU and 2048-byte TX
slots; jumbo frames are not supported yet. The application creates DPDK RX
queues only on ingress and DOCA ETH TX queues only on egress. Confirm that this
combination can start both Flow ports on the target; startup errors are fatal,
not evidence that the topology is unsupported in general. Ctrl-C requests a
stop, drains each worker for up to five seconds and prints final counters.

## SDK source and integration contract

API usage was derived from the user-provided `~/Capstone/doca-samples` tree,
whose `VERSION` is `3.2.1025`; this is the historical source reference, not the
current deployment version. Target compilation must use **3.5.0098**. Relevant
sample paths within that tree:

- `samples/doca_flow/flow_rss_meta/flow_rss_meta_sample.c`
- `samples/doca_flow/flow_drop/flow_drop_sample.c`
- `samples/doca_flow/flow_common.c`
- `samples/doca_eth/eth_txq_send_ethernet_frames/eth_txq_send_ethernet_frames_sample.c`
- `samples/doca_eth/eth_flow_common.c`
- `samples/common.c` and `applications/common/dpdk_utils.c`

The sample source supports the API call shapes; installed target headers remain
necessary to establish compatibility. Hardware mode defaults to off. The sample
license is retained in the adapted source files.

The implemented integration follows these rules; validate them on the target:

1. Initialize EAL using the supplied sample's controlled-probe pattern, open the
   configured devices, and derive actual DPDK port IDs from their DOCA mapping.
   Do not assume p0/p1 equal DPDK ports 0/1. Validate coexistence of DPDK RX and
   DOCA ETH TX on the chosen devices; use a single Flow engine for both ports.
2. Configure eight RX queues on ingress, with VLAN stripping/LRO disabled and
   mbufs large enough for the selected MTU. Resolve target egress port startup
   from the matching ETH sample. The current Flow mode is `vnf,hws,isolated`;
   representor/switch-mode support is not implemented.
3. Start each ETH TX context with a unique queue ID and its own worker. Install
   RSS only once the RX queues and workers are ready. Capability queries reject
   unsupported TX burst sizes and queue types.
4. Call `dpdk_rx_poll()` on each queue's owning core. Completion callbacks run
   on that same core; live statistics must not be read concurrently without a
   snapshot/synchronization mechanism. TX submission is not wire delivery.
5. Stop ingress, call `worker_stop()`, and progress until `worker_drained()`.
   Draining is bounded in the application: if the deadline expires, report failure
   and retain outstanding resources. `eth_tx_close()` may require retries while
   the context stops. Never free registered memory while tasks own it. A failed
   callback buffer release is fatal and pins resources instead of risking DMA
   into freed memory. Stop Flow ports before destroying the global Flow engine.

For source/header discovery on the actual target:

```sh
python3 tools/discover_node_b.py > node-b-inventory.json
```

The collector includes ETH TX headers and samples, alongside Flow and DPDK
package metadata. Supply `--sdk-root` if installed elsewhere. Inventory success
is not a milestone pass. Milestone 1 still requires ConnectX wire RX/TX captures,
matching counters and a sustained test on the BlueField.

## Group configuration and packet behavior

Edit [config/groups.example.json](config/groups.example.json) with the destination
MAC/IP/QPN of actual receiver QPs. Incoming `(destination IPv4, destination QPN)`
selects one group. A single-member group provides ordinary QPN rewriting; use
the original destination MAC and IP when only QPN should change. All outputs
leave through the configured TX device. Source addresses, PSN, P_Key, payload,
padding and immediate data remain unchanged.

Validate a file without hardware using `build-bf3/groups_check FILE` (or
`build-local/groups_check FILE` after the local tests). Limits are 64 groups,
16 members per group, group IDs of 1–63 ASCII characters and files smaller than
1 MiB. The schema accepts printable ASCII strings without escapes and decimal
integer QPNs from 0 through 16777215. Unknown/duplicate fields, duplicate IDs or
match keys, identical duplicate members, empty groups and invalid addresses are
errors. Groups are immutable after startup; restart to apply edits. The parser
has no third-party JSON dependency.

With `--groups`, only Ethernet II + IPv4/IHL=5 + unfragmented UDP/4791 RC SEND
opcodes 0x00–0x05 are accepted. VLAN, IPv6, IP options, ACKs and other opcodes
are explicitly dropped. Input ICRC is always checked; corruption is counted and
dropped rather than concealed by generating a new checksum. Input IP/UDP
checksums are not separately validated. An IPv4 UDP checksum of zero stays zero;
a nonzero checksum is recomputed after the final ICRC. Ethernet padding after
the IP packet is preserved but excluded from ICRC. The API expects no Ethernet
FCS in RX data.

Fan-out is best effort, in file member order. A full TX pool or failed submission
counts a drop for that member; other members may still transmit. There is no
rollback, retransmission or guarantee of all-or-none multicast. Each output owns
its bytes until TX completion. RX counts are input frames; submitted/completed
counts are replicas. Per-member submitted/completed/error/drop counters are
printed after workers stop. Malformed/unsupported/bad-ICRC/unknown-group counters
count rejected input frames before member selection.

ICRC normalization follows the IPv4 behavior in
[Linux v6.12 RXE](https://github.com/torvalds/linux/blob/v6.12/drivers/infiniband/sw/rxe/rxe_icrc.c):
the virtual all-ones LRH prefix, normalized IPv4/UDP/BTH invariant fields,
then transport headers, payload and pad, with a little-endian trailer. The current
portable implementation uses full bitwise CRC recomputation; no line-rate claim
is made. Synthetic zlib agreement is established; independent ConnectX captures
and hardware acceptance remain required.

Node A still owns QP setup and PSN/peer state. This implementation neither
forwards RC ACKs in group mode nor aggregates multicast ACKs. An external return
path and appropriate endpoint arrangement are needed for RC SEND completion;
correct fan-out packets alone do not establish reliable RC multicast.

## Docker on the DPU

[Dockerfile](Dockerfile) builds the hardware executable and runs the portable
suite using NVIDIA's `nvcr.io/nvidia/doca/doca:devel-3.5.0` development image.
The image explicitly sets the verified Arm DOCA/DPDK pkg-config directories;
the target's `libdpdk.pc` exists outside its default search path.
`3.5.0` is the image release tag; the Dockerfile separately requires the
`doca-common` pkg-config version to equal **3.5.0098**. If the tag contains a
different SDK build, the image build stops. Set `DOCA_IMAGE` to an image/tag or
digest containing the matching build; do not assume the release tag guarantees
that exact package version. The retained development image keeps the same SDK
libraries/providers at build and runtime, and the Meson build directory stays
in place so its shared-library RUNPATH remains valid.

Compose explicitly targets `linux/arm64`. Build natively on the DPU, or on an
Arm builder; an x86 builder requires preconfigured Arm emulation. The following
are instructions for the operator; they have **not** been executed remotely:

```sh
cd icrc
cp .env.example .env
# Edit .env: RX_PCI, TX_PCI, LCORES, GROUPS_FILE, and HUGEPAGES_DIR.
# Replace example group members with the actual receiver MAC/IP/QPN values.
docker compose config --quiet
docker compose build
docker compose up
# In another terminal, request graceful TX draining:
docker compose stop
```

`LCORES` must name nine available physical DPU CPU IDs (one main and eight
workers), for example `0-8` only if that set is available. It configures both the
container cpuset and the EAL lcore list. `RX_PCI` and `TX_PCI` must be distinct,
verified physical device BDFs. `GROUPS_FILE` is mounted read-only at
`/etc/node-b/groups.json`; edit it and restart the service to apply changes.
The example values are not receiver discovery.

Before launch, the operator must have working host drivers/firmware, usable
ports, `/dev/infiniband`, and a mounted hugetlbfs at `HUGEPAGES_DIR` with enough
free allocated hugepages. The container does not install host drivers, allocate
hugepages, bind devices, change eSwitch mode or configure Node A. Docker binds
the existing hugepage and RDMA-device directories; missing paths cause an error
rather than creating empty host directories.

[compose.yaml](compose.yaml) uses host networking, privileged device access and
unlimited memlock for the DOCA/DPDK process, following NVIDIA's
[BlueField development-container model](https://networking-docs.nvidia.com/doca/archive/3-5-0/doca-developer-guide).
Launching the service starts packet steering and TX on the selected devices.
The program receives SIGTERM directly and gets 60 seconds for shutdown. There
is no automatic restart or fabricated health check implying receiver acceptance.

DOCA 3.5 deprecates the global Flow resource API used by the existing adapter but
retains ABI support. Hardware builds define `DOCA_ALLOW_DEPRECATED_API` as the
[documented compatibility option](https://networking-docs.nvidia.com/doca/archive/3-5-0/customer-affecting-changes),
alongside `DOCA_ALLOW_EXPERIMENTAL_API`. Other compiler warnings remain errors.
This is not a claim that all 3.2-derived source is already validated against 3.5.
The [NGC tag listing](https://catalog.ngc.nvidia.com/orgs/nvidia/doca/containers/doca/-/tags)
is the source for the development-image tag.

Local validation: Compose renders with explicit sample environment values and
rejects missing required values. The portable suite passes. Full image build,
DOCA 3.5 compilation and packet I/O on the DPU remain unverified; neither remote
machine was changed or used to build/run this application.

# Node B RoCEv2 middlebox

Runtime target: **DOCA 3.5.0098 on BlueField Arm**, verified through the native
WSL SSH aliases on 2026-10-26/27. `nvaitc-1` reaches Node A and `nvaitc-2`
reaches Node B; `ssh DPU` from either jump host enters its associated DPU. Both
DPUs run Ubuntu 24.04.4 LTS (`aarch64`). DOCA Common, Flow, ETH and the DPDK
bridge report `3.5.0098`; installed DPDK metadata reports `26.03.0.5`.

Target packet path: DOCA Flow → eight bidirectional DPDK workers → optional
forward rewrite/full ICRC → DOCA ETH TX. Reverse UDP/4791 traffic is copied
unchanged from port 1 to port 0 for the RC ACK return path. See
[AGENTS.md](AGENTS.md) for the three milestones.

## Runtime connection

Verified on 2026-10-26/27. The VPN must be active to reach the private network.

The DPU inventory maps the physical ports as follows:

| Role | Netdev | PCI BDF | Link |
| --- | --- | --- | --- |
| Forward ingress / reverse egress | `p0` | `0000:03:00.0` | Up |
| Forward egress / reverse ingress | `p1` | `0000:03:00.1` | Up |

These are the current verified values for `.env`; recheck them after hardware,
firmware or port-topology changes.

| Item | Value |
| --- | --- |
| WSL first-hop aliases | `nvaitc-1` (Node A), `nvaitc-2` (Node B) |
| Second hop | `ssh DPU` from the selected jump host |
| Verified target | Ubuntu 24.04.4 LTS, `aarch64`, DOCA `3.5.0098`, DPDK `26.03.0.5` |

From WSL, use the existing native SSH configuration and credentials:

```sh
ssh nvaitc-2
# Once on nvaitc-2, for the Node B runtime:
ssh DPU
```

Use `nvaitc-1` instead to enter Node A. This command checks the Node B versions
without starting the application:

```sh
ssh -o BatchMode=yes -o ConnectTimeout=10 nvaitc-2 \
  'ssh -o BatchMode=yes -o ConnectTimeout=10 DPU "uname -m; cat /opt/mellanox/doca/VERSION; pkg-config --modversion doca-common doca-flow doca-eth doca-dpdk-bridge; sed -n /Version:/p /opt/mellanox/dpdk/lib/aarch64-linux-gnu/pkgconfig/libdpdk.pc"'
```

`DPU` is resolved by SSH on the first-hop machine; its underlying address and
user were not separately recorded. Authentication stays in the existing SSH
configuration; no credentials or key material are stored in this repository.

The Node B checkout used for validation is
`/home/mtchang/Open-Hackathon/icrc` on the DPU reached through `nvaitc-2`.

## Implementation status

Module headers live in `include/`; implementations live in `src/`.

The bidirectional forwarding, rewrite and file-configured fan-out source paths
are implemented. The DOCA 3.5 hardware build, portable tests, queue
initialization and controlled physical-path tests pass on Node B. Tagged
synthetic UDP/4791 traffic passed losslessly in both directions through all
eight queues. Independently generated valid RC SEND frames were rewritten into
two members and all 128 captured outputs matched an independent byte/checksum
oracle. ConnectX endpoint acceptance remains pending. Implemented modules:

The packet/configuration and live hardware paths support Ethernet II with
either restricted IPv4 or IPv6. IPv6 RC SEND parsing, address-family-aware
group lookup, destination rewrite, mandatory UDP checksum and full ICRC agree
with independent Python vectors across all supported SEND opcodes. DOCA Flow
uses one root control pipe per port to classify the L3 family, then forwards to
family-specific non-root RSS pipes. This graph passed constructor and controlled
wire tests on DOCA 3.5.0098.

- `src/flow.c`: one Flow engine, explicit device/port binding, one root IP-family
  classifier and IPv4/IPv6 UDP/4791 RSS leaves into queues 0–7, drop on miss,
  entry completion verification and aggregate hardware counter reads.
- `src/worker.c`: copy RX once into worker scratch, validate and look up the group,
  produce an independently rewritten TX buffer per member, count completions/drops
  and drain on shutdown. Omitting `--groups` keeps unchanged forwarding.
- `src/dpdk_rx.c`: burst polling, bounded multi-segment mbuf copying and release,
  and TX progress even when there are no incoming packets.
- `src/roce.c` and `src/icrc.c`: bounded RC SEND parsing, destination rewriting,
  full IPv4/IPv6 ICRC and IP/UDP checksum recomputation.
- `src/groups.c`: startup JSON validation and immutable destination-group lookup.
- `src/eth_tx.c`: per-worker DOCA TX queue, progress engine, registered storage,
  buffer inventory, send tasks, completion callbacks and ordered cleanup.

`src/main.c` connects device probing, actual port-ID discovery, RX queue setup,
eight DPDK lcore workers, two directions per worker, signals, directional
counters and bounded TX draining. The
development machine has no DOCA/DPDK headers or libraries, so hardware
validation is performed in the DOCA development container on Node B. The
portable parser, checksum, group loader and worker fan-out are exercised by the
tests below.

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
sudo ./build-bf3/node_b <PORT0-PCI-BDF> <PORT1-PCI-BDF> <nine-available-lcores> \
  --groups config/groups.example.json
# Example lcore argument: 0-8 (only if these cores are available on the target).
```

The first enabled lcore is EAL's main core; eight other lcores poll queues.
Use distinct physical devices, not representors, for this initial VNF backend.
The initial receive configuration uses standard Ethernet MTU and 2048-byte TX
slots; jumbo frames are not supported yet. Both ports have eight DPDK RX queues,
eight DOCA ETH TX contexts and a UDP/4791 RSS root. Each lcore owns queue `i` in
both directions. Group parsing/rewrite is only applied from port 0 to port 1;
port 1 to port 0 is an unchanged return path. Ctrl-C requests a stop, drains
both contexts on every worker for up to five seconds and prints directional
counters.

## Controlled physical-path probe

`tools/raw_udp4791.py` sends and captures tagged Ethernet/IPv4/UDP frames using
raw packet sockets. It validates either physical direction without requiring
interface IP configuration. It is a Flow/RSS diagnostic, **not a RoCE packet
generator or receiver-acceptance test**.

Run Node B without `--groups`, then start capture on Node A before sending. The
tool needs a privileged container for raw sockets; all paths below stay inside
the `icrc` checkout:

```sh
# Node A capture terminal
docker run --rm --privileged --network host --entrypoint python3 \
  -v "$PWD/tools/raw_udp4791.py:/opt/probe.py:ro" \
  icrc-node-b:doca-3.5.0098 /opt/probe.py capture \
  --interface p1 --tag manual-rss-check --expect 256 --timeout 30 --quiet

# Node A send terminal; replace --src-mac with Node A p0's verified MAC.
docker run --rm --privileged --network host --entrypoint python3 \
  -v "$PWD/tools/raw_udp4791.py:/opt/probe.py:ro" \
  icrc-node-b:doca-3.5.0098 /opt/probe.py send \
  --interface p0 --src-mac 9c:63:c0:50:96:64 \
  --tag manual-rss-check --count 256 --flows 64 --interval 0.005 --quiet
```

The default broadcast destination makes the unchanged frame receivable on both
physical ports. Each sequence number must be captured once. Without `--quiet`,
the sender and receiver print a SHA-256 for every frame so exact preservation can
be compared. After stopping Node B, confirm every queue has nonzero RX, submitted
equals completed, and all TX error/drop/pending counters are zero.

Reverse the capture/send interfaces and use Node A p1's source MAC
`9c:63:c0:50:96:65` to exercise port 1 → port 0. On 2026-10-27, 256/256
controlled packets were captured in each direction. Every queue handled 32
controlled packets in each direction; submissions equaled completions with no
errors, drops or pending buffers.

## Independent RoCE rewrite wire oracle

`tools/roce_wire.py` constructs restricted Ethernet II/IPv4-or-IPv6/UDP RC SEND
frames using only Python's standard library. It independently computes the
input and expected rewritten IP/UDP checksums and full RoCE ICRC. Capture runs
promiscuously and accepts a packet only when its complete IP-frame bytes equal
one expected member output and all three checksum checks pass. It does not
create a QP or prove transport acceptance.

With Node B running with `config/groups.example.json`, start capture on Node A:

```sh
docker run --rm --privileged --network host --entrypoint python3 \
  -v "$PWD/tools/roce_wire.py:/opt/probe.py:ro" \
  icrc-node-b:doca-3.5.0098 /opt/probe.py capture \
  --interface p1 --src-mac 9c:63:c0:50:96:64 --tag m2-roce-rewrite \
  --count 64 --flows 64 --payload-size 64 \
  --member 02:00:00:00:00:01,192.0.2.1,101 \
  --member 02:00:00:00:00:02,192.0.2.2,102 --timeout 25
```

Then send from another Node A terminal:

```sh
docker run --rm --privileged --network host --entrypoint python3 \
  -v "$PWD/tools/roce_wire.py:/opt/probe.py:ro" \
  icrc-node-b:doca-3.5.0098 /opt/probe.py send \
  --interface p0 --src-mac 9c:63:c0:50:96:64 --tag m2-roce-rewrite \
  --count 64 --flows 64 --payload-size 64 --interval 0.005
```

The verified run reported `count=128, expected=128, members=2`. Each member
recorded 64 submissions and 64 completions in aggregate, with zero member
errors or drops. This closes the capture/oracle part of Milestone 2 and the
two-member no-loss wire-replication part of Milestone 3; it does not close the
receiver-QP/RC-completion or recorded sustained-load gates.

The IPv6 equivalent uses `--src-ip 2001:db8::10 --dst-ip 2001:db8::100` and
members `2001:db8::1` and `2001:db8::2`, with
`config/groups.ipv6.example.json`. On 2026-10-27, 64 controlled inputs produced
exactly 128 independently checksum/ICRC-verified outputs. Every RSS queue
handled eight controlled inputs; both members completed 64/64 submissions with
zero errors, drops or pending buffers.

## Node A OVS setup and recovery

Node A already provides `ovsbr1` (`en3f0pf0sf0`, `p0`, `pf0hpf`) and `ovsbr2`
(`p1`, `pf1hpf`, and receiver SF representors). Hardware offload is enabled.
Run the project script as root on Node A; it owns only flows with cookie
`0x49435243` and never removes the pre-existing bridges or `NORMAL` rules:

```sh
sudo tools/configure_node_a_ovs.sh setup
sudo tools/configure_node_a_ovs.sh show
# Recover the original bridge flow tables:
sudo tools/configure_node_a_ovs.sh recover
```

Do not run host OVS commands through a privileged container or namespace-entry
workaround. Obtain explicit approval before `sudo`, root, privileged-container,
or equivalent operations.

The setup rules forward the dedicated sender SF to `p0`, select receiver SFs
by destination MAC on `p1`, and install the symmetric ACK route. They are
deliberately layer-2-only. Override the script's documented environment
variables if the SF allocation or MAC addresses change.

The current lab mapping is recorded in `config/groups.node-a.ipv6.json`. A
64-input run sent from `enp3s0f0s0`; Node B completed 128 member transmissions,
and the OVS rules counted 64 packets to each of `en3f1pf1sf0` and
`en3f1pf1sf1`. AF_PACKET capture on the receiver SF netdevs did not observe
UDP/4791, which can be consumed by the RDMA hardware path. These OVS counters
prove delivery to the representors, not receiver-QP acceptance or RC completion.

## Milestone 2 RC acceptance test

`tools/rc_qpn_rewrite_test.c` creates a sender QP on `mlx5_2`, a live receiver
QP on `mlx5_3`, and a distinct dummy QP whose QPN is advertised to the sender.
It writes a runtime single-member group mapping the dummy QPN to the live QPN,
then pauses with all QPs alive. Build and start it as the normal Node A user:

```sh
cc -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
  tools/rc_qpn_rewrite_test.c -o tools/rc_qpn_rewrite_test -libverbs
./tools/rc_qpn_rewrite_test config/groups.m2.runtime.json
```

The SF link-local IPv6 neighbors must resolve before QPs enter RTR. In this lab,
temporary static entries were added directly with `sudo ip -6 neigh replace`;
no privileged container was used to execute host commands. Copy the generated
runtime group file to Node B, start the already-reviewed privileged Node B
service with `GROUPS_FILE=./config/groups.m2.runtime.json`, then press Enter in
the harness. Runtime QPNs are ephemeral; never reuse a saved file after the
harness exits.

On 2026-10-27 the sender targeted virtual QPN `1006`, Node B rewrote it to live
receiver QPN `1005`, and the receiver reported a successful 40-byte receive CQE
with an exact payload match. The sender then reported a successful SEND CQE,
proving the RC ACK returned through Node B. Node B recorded one forward member
submission/completion and reverse submissions/completions with zero TX errors,
drops or pending buffers. This completes Milestone 2.

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
2. Configure eight RX queues and eight ETH TX contexts on each port, with VLAN
   stripping/LRO disabled and mbufs large enough for the selected MTU. The
   verified Flow mode is `vnf,hws`;
   representor/switch-mode support is not implemented.
3. Start each ETH TX context with a unique per-port queue ID. Each lcore owns a
   forward and reverse worker/context pair. Install one root control classifier
   and the two family-specific RSS leaves only once the RX queues and workers
   are ready. Capability queries reject unsupported TX burst sizes and queue
   types.
4. Call `dpdk_rx_poll()` on each queue's owning core. Completion callbacks run
   on that same core; live statistics must not be read concurrently without a
   snapshot/synchronization mechanism. TX submission is not wire delivery.
5. Stop both worker directions and progress until both are drained.
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
is not a milestone pass. The synthetic physical-path and RSS checks now pass;
Milestone 1 still requires controlled ConnectX RoCE traffic, receiver acceptance
and a sustained test on the BlueField.

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

The worker accepts Ethernet II with either IPv4/IHL=5/unfragmented or IPv6 with
no extension headers/jumbograms, followed by UDP/4791 RC SEND opcodes
0x00–0x05. VLAN, IPv4 options/fragments, IPv6 extensions/fragments, ACKs and
other opcodes are explicitly dropped. Group endpoints use exactly one of
`dst_ipv4` or `dst_ipv6`; all members must use the match key's address family.
Input ICRC is always checked; corruption is counted and dropped rather than
concealed by generating a new checksum. Input IP/UDP checksums are not
separately validated. An IPv4 UDP checksum of zero stays zero; a nonzero IPv4
checksum and the mandatory IPv6 UDP checksum are recomputed after the final
ICRC. Ethernet padding after the IP packet is preserved but excluded from ICRC.
The API expects no Ethernet FCS in RX data.

Fan-out is best effort, in file member order. A full TX pool or failed submission
counts a drop for that member; other members may still transmit. There is no
rollback, retransmission or guarantee of all-or-none multicast. Each output owns
its bytes until TX completion. RX counts are input frames; submitted/completed
counts are replicas. Per-member submitted/completed/error/drop counters are
printed after workers stop. Malformed/unsupported/bad-ICRC/unknown-group counters
count rejected input frames before member selection.

ICRC normalization follows the IPv4 behavior in
[Linux v6.12 RXE](https://github.com/torvalds/linux/blob/v6.12/drivers/infiniband/sw/rxe/rxe_icrc.c):
the virtual all-ones LRH prefix, normalized IPv4-or-IPv6/UDP/BTH invariant fields,
then transport headers, payload and pad, with a little-endian trailer. The current
portable implementation uses full bitwise CRC recomputation; no line-rate claim
is made. Synthetic zlib agreement is established; independent ConnectX captures
and hardware acceptance remain required.

Node A still owns QP setup and PSN/peer state. Node B now forwards UDP/4791
traffic, including RC ACK packets, unchanged from port 1 to port 0 even while
forward group rewriting is enabled. It does not aggregate multicast ACKs or
alter QP state. A single-receiver test must still configure compatible peers and
PSNs on Node A and observe actual SEND completion; correct fan-out packets alone
do not establish reliable RC multicast.

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
The Dockerfile performs no package-manager operations: it does not run
`apt-get update` or install packages on the shared DPU. The selected DOCA
development image must already contain the compiler, Meson/Ninja, pkg-config
and Python required by the build.

Compose explicitly targets `linux/arm64`. Build natively on the DPU, or on an
Arm builder; an x86 builder requires preconfigured Arm emulation. These commands
were exercised on Node B for the image build and bounded no-group smoke run;
group values remain examples until real receiver QPs are configured:

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
container cpuset and the EAL lcore list. The legacy environment names `RX_PCI`
and `TX_PCI` now mean forward port 0 and port 1 respectively; both are RX and TX
capable and must be distinct verified physical BDFs. `GROUPS_FILE` is mounted read-only at
`/etc/node-b/groups.json`; edit it and restart the service to apply changes.
The example values are not receiver discovery.

Before launch, the operator must have working host drivers/firmware, usable
ports, `/dev/infiniband`, and a mounted hugetlbfs at `/dev/hugepages` (or the
path set by `HUGEPAGES_DIR`) with enough free allocated hugepages. The verified
Node B setup has eight 512 MiB pages (4 GiB total). The container does not install host drivers, allocate
hugepages, bind devices, change eSwitch mode or configure Node A. Docker binds
the existing hugepage and RDMA-device directories; missing paths cause an error
rather than creating empty host directories.

[compose.yaml](compose.yaml) uses host networking, privileged device access and
unlimited memlock for the DOCA/DPDK process, following NVIDIA's
[BlueField development-container model](https://networking-docs.nvidia.com/doca/archive/3-5-0/doca-developer-guide).
Launching the service starts packet steering and TX on the selected devices.
The program receives SIGTERM directly and gets 60 seconds for shutdown. There
is no automatic restart or fabricated health check implying receiver acceptance.

The adapter uses DOCA 3.5 port-scoped Flow resources for its counter and RSS
object. Hardware builds define `DOCA_ALLOW_DEPRECATED_API` and
`DOCA_ALLOW_EXPERIMENTAL_API` for compatibility with SDK headers while other
compiler warnings remain errors. The Flow entry calls use the DOCA 3.5 typed
basic/control APIs, `doca_flow_pipe_basic_add_entry` and
`doca_flow_pipe_control_add_entry`, as required by the installed 3.5 headers.
The verified DPDK 26.03 mlx5 probe uses `txq_mem_algn=0`; otherwise the BF3 Arm
runtime fails its consecutive TX-queue memory allocation despite available
hugepages. DOCA Flow uses port resource mode, reserves RSS/counter resources per
port, and uses one root control pipe to select family-specific IPv4/IPv6 RSS
leaves. Only UDP destination port 4791 is an entry-supplied leaf match field.
The [NGC tag listing](https://catalog.ngc.nvidia.com/orgs/nvidia/doca/containers/doca/-/tags)
is the source for the development-image tag.

Validation on Node B (2026-10-27): the image built against DOCA `3.5.0098` and
DPDK `26.03.0.5`; all six Meson tests passed; physical BDFs `03:00.0` and
`03:00.1` mapped to DPDK ports 2 and 3; sixteen RX queues and sixteen DOCA ETH
TX contexts initialized; and one control root plus IPv4/IPv6 UDP/4791 RSS
leaves were installed on each port.

Three individually logged 81-byte probes were captured on Node A p1 with SHA-256
values exactly matching the frames sent on Node A p0. A second run sent 256
frames across 64 UDP source ports; capture received all 256, and Node B assigned
32 controlled packets to each of its eight queues. Every controlled packet was
submitted and completed with zero TX errors, drops or pending buffers. Additional
ambient UDP/4791 packets were visible in the aggregate counters, so controlled
capture sequence numbers—not aggregate totals—are the acceptance oracle.

The reverse run also captured 256/256 frames from Node A p1 through Node B to
Node A p0; every queue handled 32 controlled packets in each direction. The
independent IPv4 and IPv6 RoCE runs each sent 64 valid RC SEND frames and
captured exactly 128 member outputs. Complete-frame comparison, IP/UDP checksums
and full ICRC all agreed, while per-member submissions/completions were 64/64
with zero errors or drops. The Node A OVS test additionally proved the sender
SF-to-p0 route and exactly 64 outputs to each receiver SF representor. The live
RC harness then proved QPN rewrite into an accepting ConnectX receiver and
ACK-backed SEND completion. Milestone 2 is complete; sustained-load behavior
and throughput remain open Milestone 3 gates.

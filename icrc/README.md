# Node B RoCEv2 middlebox

Target packet path: DOCA Flow → eight DPDK RX workers → rewrite/full ICRC →
DOCA ETH TX. See [AGENTS.md](AGENTS.md) for the three milestones.

## Implementation status

Milestone 1 is in progress. Implemented modules:

- `src/flow.c`: one Flow engine, explicit device/port binding, IPv4 UDP/4791
  matching into RSS queues 0–7, drop on miss, entry completion verification and
  hardware counter reads.
- `src/worker.c`: unchanged packet copy into independent TX slots, bounded
  outstanding submissions, RX release, completion accounting and shutdown drain.
- `src/dpdk_rx.c`: burst polling, bounded multi-segment mbuf copying and release,
  and TX progress even when there are no incoming packets.
- `src/eth_tx.c`: per-worker DOCA TX queue, progress engine, registered storage,
  buffer inventory, send tasks, completion callbacks and ordered cleanup.

`src/main.c` connects device probing, actual port-ID discovery, RX queue setup,
eight DPDK lcore workers, signals, counters and bounded TX draining. The executable
and hardware adapters are source implementations, not locally compiled or validated:
this machine has no DOCA/DPDK headers or libraries. Parser/ICRC and group fan-out
are later milestones and have not been implemented.

## Build and local tests

With Meson and Ninja installed:

```sh
cd icrc
meson setup build
meson test -C build --print-errorlogs
```

The portable worker tests also run with a C compiler and Python alone:

```sh
mkdir -p build-local
cc -std=c11 -Wall -Wextra -Werror -pedantic -Isrc \
  src/worker.c tests/test_worker.c -o build-local/test_worker
./build-local/test_worker
python3 -m unittest discover -s tests -p 'test_*.py' -v
```

Tests exercise all eight queue IDs, segmented input, full queues, submission
failure, truncation, oversize frames, stale/duplicate completions, out-of-order
completion, 160,000 submissions across repeated slot reuse, and shutdown with
outstanding buffers. These are software ownership tests, not hardware traffic
or parallel-worker performance measurements.

On the BlueField with matching SDK development packages, compile the adapters:

```sh
pkg-config --modversion doca-common doca-flow doca-eth doca-dpdk-bridge libdpdk
meson setup build-bf3 -Dhardware=true
meson compile -C build-bf3
meson test -C build-bf3 --print-errorlogs
```

This produces `build-bf3/node_b`, hardware libraries and tests. Missing SDK
dependencies cause the hardware build to fail; no mock hardware backend is
substituted. After verifying port ownership, links and hugepages on the target:

```sh
sudo ./build-bf3/node_b <RX-PCI-BDF> <TX-PCI-BDF> <nine-available-lcores>
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
whose `VERSION` is `3.2.1025`. Relevant sample paths within that tree:

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

# ICRC optimization hardware smoke — 2026-11-02

> This records the initial smoke attempt. The prerequisite was subsequently
> fixed and the bounded RC wire test passed; see
> `reports/icrc-optimization-wire-validation-2026-11-02.md`.

## Result

Node B build and startup passed; the end-to-end RC payload test was blocked by
Node A QP setup before any test packet was sent.

## Node B evidence

- Native Arm image dependencies: DOCA `3.5.0098`, DPDK `26.03.0.5`.
- Ten of ten in-image Meson tests passed.
- Runtime mode: `dpdk-neon-full` plus `incremental-verify`.
- Physical BDFs `03:00.0` and `03:00.1` mapped dynamically to DPDK ports 6/7.
- All eight bidirectional workers reached the polling loop.
- Graceful shutdown returned exit status 0.
- TX errors, pending buffers, and incremental verification failures were zero
  on every queue.
- The Compose container was removed and no `node_b` process remained.

The no-traffic interval observed two unrelated malformed forward frames and two
reverse frames, which were forwarded unchanged. It did not exercise a valid
member rewrite and therefore is not an ICRC wire-performance result.

## Node A blocker

The documented OVS setup command did not execute because remote `sudo -n`
required an interactive password. Both SF interfaces already had their expected
IPv4 addresses and both RDMA devices reported active Ethernet ports with MTU
1024. Nevertheless:

- two five-QP harness attempts failed with `receiver0 RTR failed: Connection
  timed out`;
- the independently compiled single-receiver harness failed with `QP setup
  failed: Connection timed out`;
- neither harness reached `READY`, so no live QPN configuration was generated
  and Node B was never started with stale QPNs.

The next run must begin with an interactive execution on the Node A Arm DPU:

```sh
cd /home/mtchang/Open-Hackathon/icrc
sudo ./tools/configure_node_a_ovs.sh setup
sudo ./tools/configure_node_a_ovs.sh show
```

After testing, recover only the project-owned flow cookie:

```sh
sudo ./tools/configure_node_a_ovs.sh recover
```

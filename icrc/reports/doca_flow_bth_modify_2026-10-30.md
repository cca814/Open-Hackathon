# DOCA Flow BTH modify probe — 2026-10-30

## Question

Can DOCA Flow on the validated BlueField-3 Node B rewrite the 24-bit RoCEv2
BTH Destination QPN and PSN before RSS delivery to the Arm workers?

## Environment

- Node B: DPU reached through `nvaitc-2` then `DPU`
- Checkout: `/home/mtchang/Open-Hackathon/icrc`
- Architecture: `aarch64`
- DOCA Flow: `3.5.0098`
- DPDK: `26.03.0.5`
- Ingress: PCI `03:00.0`, DPDK port 2
- Egress: PCI `03:00.1`, DPDK port 3

The installed headers define `struct doca_flow_header_ib_bth` with
`dest_qp[3]` and `psn[3]`. `struct doca_flow_header_format` contains the
RoCEv2 header and is embedded in both `struct doca_flow_match` and
`struct doca_flow_actions`. The shipped `flow_switch_roce_bth_header` sample
demonstrates BTH matching, not BTH modification.

## Probe shape

The existing IPv4/IPv6 UDP/4791 non-root RSS pipes were left unchanged unless
an explicit environment variable enabled the experiment. The action template
selected `DOCA_FLOW_L4_TYPE_EXT_ROCE_V2` and marked the selected three-byte
BTH field changeable. Entry actions supplied fixed 24-bit values. Forwarding
remained `DOCA_FLOW_FWD_RSS`; the reverse pipe never received a modify action.

Three constructor probes were run:

| Probe | Value | Result |
| --- | --- | --- |
| QPN + PSN | `0x010203`, `0x040506` | `doca_flow_pipe_create`: `Operation not supported` |
| QPN only | `0x010203` | `doca_flow_pipe_create`: `Operation not supported` |
| PSN only | `0x040506` | `doca_flow_pipe_create`: `Operation not supported` |

Every probe failed before entry insertion. All packet counters remained zero;
therefore there was no wire packet on which to evaluate ICRC behavior.

## Baseline and conclusion

After the failed probes, the same image was launched without experimental
variables. It mapped the two devices to DPDK ports 2 and 3, reached
`Eight bidirectional workers running`, handled SIGTERM, and exited with status
0. The normal DOCA Flow classification/RSS path was therefore preserved.

For this exact BF3 / firmware / DOCA 3.5.0098 HWS configuration, the presence
of BTH fields in the public match/action data structures does **not** imply
that BTH QPN or PSN modify actions are supported. Keep QPN/PSN rewriting and
full ICRC recomputation in the DPDK worker path for the MVP.

No standalone DPDK `rte_flow` FLEX/MODIFY_FIELD rule was attempted because it
would require a separately verified parser profile and ownership plan; mixing
it with the active DOCA Flow port is outside this bounded experiment.

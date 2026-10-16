#!/usr/bin/env python3
"""Generate BMv2 CLI rules for N connections between the two scapy nodes.

translate    Same addressing as controller/connections.example.txt (N=1 reproduces
             it): the switch rewrites IP, UDP port, QP and PSN, so every packet is
             sent to the controller for ICRC repair.
passthrough  Each node addresses the other directly. The switch only rewrites MAC,
             TTL and the UDP checksum, none of which the ICRC covers, so packets
             skip the controller. This isolates the cost of the BMv2 pipeline.

Each connection takes four consecutive rules (client Send, server ACK, server
Send, client ACK), the layout stats/common.py expects.
"""

from __future__ import annotations

import argparse
import sys

PSN_MOD = 1 << 24
TABLE = "table_add MyIngress.connections MyIngress.translate_connection"
CLIENT = ("10.0.0.1", 0x020000000001, 100, 1)  # IP, MAC, first QP, switch port
SERVER = ("10.0.0.2", 0x020000000002, 200, 2)
SWITCH_A_SIDE = ("10.0.0.101", 0x020000000101)  # what the client sees as its remote
SWITCH_B_SIDE = ("10.0.0.102", 0x020000000102)  # what the server sees as its remote
MAX_CONNECTIONS = 4096 // 4


def rule(port, src, dst, qp, opcode, new_src_mac, new_dst_mac, new_src, new_dst, sport, new_qp,
         psn_delta, egress) -> str:
    return (f"{TABLE} {port} {src} {dst} {qp} {opcode:#04x} => {new_src_mac:#014x} "
            f"{new_dst_mac:#014x} {new_src} {new_dst} {sport} {new_qp} 65535 "
            f"{psn_delta % PSN_MOD} 0 {egress}")


def translate(i: int) -> list[str]:
    a_ip, a_mac, a_qp, a_port = CLIENT
    b_ip, b_mac, b_qp, b_port = SERVER
    (va_ip, va_mac), (vb_ip, vb_mac) = SWITCH_A_SIDE, SWITCH_B_SIDE
    a_qp, b_qp = a_qp + i, b_qp + i
    a_remote_qp, b_remote_qp = 9001 + 2 * i, 9002 + 2 * i
    # Client Send PSN 1000 arrives as 2000; server echo PSN 9000 arrives as 5000.
    to_b = dict(new_src_mac=vb_mac, new_dst_mac=b_mac, new_src=vb_ip, new_dst=b_ip, sport=50001,
                new_qp=b_qp, egress=b_port)
    to_a = dict(new_src_mac=va_mac, new_dst_mac=a_mac, new_src=va_ip, new_dst=a_ip, sport=50002,
                new_qp=a_qp, egress=a_port)
    return [
        rule(a_port, a_ip, va_ip, a_remote_qp, 0x04, psn_delta=1000, **to_b),
        rule(b_port, b_ip, vb_ip, b_remote_qp, 0x11, psn_delta=-1000, **to_a),
        rule(b_port, b_ip, vb_ip, b_remote_qp, 0x04, psn_delta=-4000, **to_a),
        rule(a_port, a_ip, va_ip, a_remote_qp, 0x11, psn_delta=4000, **to_b),
    ]


def passthrough(i: int) -> list[str]:
    a_ip, a_mac, a_qp, a_port = CLIENT
    b_ip, b_mac, b_qp, b_port = SERVER
    a_qp, b_qp = a_qp + i, b_qp + i
    # UDP source ports are the ones scapy/roce.py sends with, so nothing under the ICRC changes.
    to_b = dict(new_src_mac=SWITCH_B_SIDE[1], new_dst_mac=b_mac, new_src=a_ip, new_dst=b_ip,
                sport=49152, new_qp=b_qp, psn_delta=0, egress=b_port)
    to_a = dict(new_src_mac=SWITCH_A_SIDE[1], new_dst_mac=a_mac, new_src=b_ip, new_dst=a_ip,
                sport=49153, new_qp=a_qp, psn_delta=0, egress=a_port)
    return [
        rule(a_port, a_ip, b_ip, b_qp, 0x04, **to_b),
        rule(b_port, b_ip, a_ip, a_qp, 0x11, **to_a),
        rule(b_port, b_ip, a_ip, a_qp, 0x04, **to_a),
        rule(a_port, a_ip, b_ip, b_qp, 0x11, **to_b),
    ]


def generate(mode: str, connections: int) -> str:
    make = translate if mode == "translate" else passthrough
    return "".join(line + "\n" for i in range(connections) for line in make(i))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("mode", choices=["translate", "passthrough"])
    parser.add_argument("-n", "--connections", type=int, default=1,
                        help="number of connections, 4 rules each (default: %(default)s; "
                        f"the table holds {MAX_CONNECTIONS})")
    parser.add_argument("-o", "--output", help="write here instead of stdout")
    args = parser.parse_args(argv)
    if args.connections < 1:
        parser.error("--connections must be at least 1")
    text = generate(args.mode, args.connections)
    if args.output:
        with open(args.output, "w") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())

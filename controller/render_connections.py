#!/usr/bin/env python3
"""Render BMv2 MITM rules from the two live endpoint information files."""

import argparse
import ipaddress
import json
from pathlib import Path
import re

PSN_MASK = (1 << 24) - 1


def endpoint(path):
    data = json.loads(Path(path).read_text())
    for key, minimum, maximum in (
        ("qpn", 1, PSN_MASK), ("remote_qpn", 1, PSN_MASK),
        ("sq_psn", 0, PSN_MASK), ("rq_psn", 0, PSN_MASK),
        ("switch_port", 1, 509),
    ):
        if type(data[key]) is not int or not minimum <= data[key] <= maximum:
            raise ValueError(f"{path}: invalid {key}")
    for key in ("gid", "remote_gid"):
        address = ipaddress.IPv6Address(data[key]).ipv4_mapped
        if address is None or address.is_unspecified or address.is_multicast:
            raise ValueError(f"{path}: {key} must be an IPv4-mapped unicast GID")
        data[key + "_ip"] = str(address)
    for key in ("local_mac", "remote_mac"):
        value = data[key]
        if not isinstance(value, str) or not re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", value):
            raise ValueError(f"{path}: invalid {key}")
        if int(value[:2], 16) & 1 or int(value.replace(":", ""), 16) == 0:
            raise ValueError(f"{path}: {key} must be a nonzero unicast MAC")
    return data


def render(client, server):
    if client["switch_port"] == server["switch_port"]:
        raise ValueError("client and server must use different switch ports")
    addresses = [node[key] for node in (client, server) for key in ("gid_ip", "remote_gid_ip")]
    if len(set(addresses)) != 4:
        raise ValueError("use distinct real and virtual IPv4 addresses for both endpoints")

    lines = [
        f'table_add MyIngress.forwarding MyIngress.forward {client["switch_port"]} => {server["switch_port"]}',
        f'table_add MyIngress.forwarding MyIngress.forward {server["switch_port"]} => {client["switch_port"]}',
    ]

    def rule(source, target, opcode, delta, udp_sport):
        key = f'{source["switch_port"]} {source["gid_ip"]} {source["remote_gid_ip"]} {source["remote_qpn"]} 0x{opcode:02x}'
        params = (
            f'0x{target["remote_mac"].replace(":", "")} '
            f'0x{target["local_mac"].replace(":", "")} '
            f'{target["remote_gid_ip"]} {target["gid_ip"]} '
            f'{udp_sport} {target["qpn"]} 65535 {delta & PSN_MASK} 0 {target["switch_port"]}'
        )
        lines.append(f"table_add MyIngress.connections MyIngress.translate_connection {key} => {params}")

    # Each ACK reverses the PSN offset of the Send it acknowledges.
    a_to_b = server["rq_psn"] - client["sq_psn"]
    b_to_a = client["rq_psn"] - server["sq_psn"]
    rule(client, server, 0x04, a_to_b, 50001)
    rule(server, client, 0x11, -a_to_b, 50002)
    rule(server, client, 0x04, b_to_a, 50002)
    rule(client, server, 0x11, -b_to_a, 50001)
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("client_info", type=Path)
    parser.add_argument("server_info", type=Path)
    args = parser.parse_args()
    try:
        rules = render(endpoint(args.client_info), endpoint(args.server_info))
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"cannot generate connection rules: {error}\n")
    print(rules, end="")


if __name__ == "__main__":
    main()

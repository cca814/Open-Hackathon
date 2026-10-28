#!/usr/bin/env python3
"""Render the BMv2 CLI script for one multicast group directory.

Reads group<gid>/sender.json and group<gid>/receiver-<rid>.json next to this
script and writes group<gid>-multicast.txt. Load it on a fresh switch only,
because the PRE node handles must start at 0:

    simple_switch_CLI --thrift-port 9090 < group<gid>-multicast.txt
"""

import argparse
import ipaddress
import json
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent

# Must match p4/include/const.p4.
GROUP_ID = 1  # MULTICAST_GROUP
CPU_PORT = 510
MAX_MEMBERS = 32  # MAX_MULTICAST_MEMBERS, the ACK bitmap width
MAX_RID = (1 << 16) - 1  # standard_metadata.egress_rid is bit<16>
PSN_MASK = (1 << 24) - 1
RECEIVER_FILE = re.compile(r"receiver-([0-9]+)\.json")


def endpoint(path):
    data = json.loads(Path(path).read_text())
    for key, minimum, maximum in (
        ("qpn", 1, PSN_MASK),
        ("remote_qpn", 1, PSN_MASK),
        ("sq_psn", 0, PSN_MASK),
        ("rq_psn", 0, PSN_MASK),
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
        if not isinstance(value, str) or not re.fullmatch(
            r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", value
        ):
            raise ValueError(f"{path}: invalid {key}")
        if int(value[:2], 16) & 1 or int(value.replace(":", ""), 16) == 0:
            raise ValueError(f"{path}: {key} must be a nonzero unicast MAC")
    return data


def load_group(directory):
    if not directory.is_dir():
        raise ValueError(f"{directory} does not exist")
    receivers = {}
    for path in directory.glob("receiver-*.json"):
        match = RECEIVER_FILE.fullmatch(path.name)
        if match is None:
            raise ValueError(f"{path}: expected receiver-<rid>.json")
        rid = int(match[1])
        if rid > MAX_RID:
            raise ValueError(f"{path}: rid must be 0..{MAX_RID}")
        if rid in receivers:
            raise ValueError(f"{path}: rid {rid} is already used")
        receivers[rid] = endpoint(path)
    rids = sorted(receivers)
    return endpoint(directory / "sender.json"), [receivers[rid] for rid in rids], rids


def validate_group(sender, receivers):
    if not 1 <= len(receivers) <= MAX_MEMBERS:
        raise ValueError(f"a group needs 1..{MAX_MEMBERS} receivers")
    identities = set()
    ack_keys = set()
    sender_identity = (sender["switch_port"], sender["gid_ip"], sender["qpn"])
    for node in receivers:
        identity = (node["switch_port"], node["gid_ip"], node["qpn"])
        if identity == sender_identity or identity in identities:
            raise ValueError(
                "each receiver must be a distinct QP, separate from the sender"
            )
        identities.add(identity)
        key = (
            node["switch_port"],
            node["gid_ip"],
            node["remote_gid_ip"],
            node["remote_qpn"],
        )
        if key in ack_keys:
            raise ValueError(
                "ambiguous ACK identity: give QPs on the same host distinct remote_qpn values"
            )
        ack_keys.add(key)
    for node in [sender, *receivers]:
        if node["gid_ip"] == node["remote_gid_ip"]:
            raise ValueError("a node's real and virtual peer IPs must differ")


def match_key(node, opcode):
    return (
        f'{node["switch_port"]} {node["gid_ip"]} {node["remote_gid_ip"]} '
        f'{node["remote_qpn"]} 0x{opcode:02x}'
    )


def destination(node, udp_port, delta):
    return (
        f'0x{node["remote_mac"].replace(":", "")} '
        f'0x{node["local_mac"].replace(":", "")} '
        f'{node["remote_gid_ip"]} {node["gid_ip"]} {udp_port} '
        f'{node["qpn"]} 65535 {delta & PSN_MASK} {node["switch_port"]}'
    )


def render(sender, receivers, rids):
    validate_group(sender, receivers)
    count = len(receivers)
    initial_psn = sender["sq_psn"]
    lines = [f"mc_mgrp_create {GROUP_ID}"]
    lines += [f"mc_node_create {rid} {CPU_PORT}" for rid in rids]
    # A fresh switch allocates PRE node handles from 0.
    lines += [f"mc_node_associate {GROUP_ID} {handle}" for handle in range(count)]
    # The rid selects the egress rewrite. The ACK member ID indexes the 32-bit
    # ACK bitmap and must be 0..count-1, so it is the position, not the rid.
    for member_id, (rid, node) in enumerate(zip(rids, receivers)):
        delta = node["rq_psn"] - initial_psn
        lines.append(
            "table_add MyEgress.multicast_members MyEgress.multicast_member "
            f"{rid} => {destination(node, 50001, delta)}"
        )
        lines.append(
            "table_add MyIngress.connections MyIngress.multicast_ack "
            f"{match_key(node, 0x11)} => {member_id} {count} {initial_psn} "
            f"{destination(sender, 50002, -delta)}"
        )
    lines.append("register_reset MyIngress.multicast_state")
    # The sender rule goes last, after all replicas and ACK rules are ready.
    lines.append(
        "table_add MyIngress.connections MyIngress.multicast_send "
        f"{match_key(sender, 0x04)} => {count} {initial_psn}"
    )
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "-g",
        "--group",
        type=int,
        required=True,
        metavar="GID",
        help="read group<GID>/ and write group<GID>-multicast.txt",
    )
    args = parser.parse_args()
    # ingress.p4 always sets mcast_grp to MULTICAST_GROUP and keeps a single
    # ACK state register, so the data plane cannot use any other group ID.
    if args.group != GROUP_ID:
        parser.exit(
            1,
            f"the P4 program only replicates group {GROUP_ID} "
            "(MULTICAST_GROUP in p4/include/const.p4)\n",
        )
    output = HERE / "output" / f"group{args.group}-multicast.txt"
    try:
        text = render(*load_group(HERE / f"group{args.group}"))
        output.write_text(text)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"cannot generate group {args.group}: {error}\n")
    print(f"wrote {output}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Packet-test a configured multicast switch and running ICRC controller.

Emulates the sender and every receiver, deliberately withholding/repeating ACKs.
Uses the same endpoint information files as controller/render_multicast.py.
Run on Linux with root/raw-socket access; see docs/multicast.md.
"""

import argparse
from pathlib import Path
import select
import sys
import time

import roce
from scapy.contrib.roce import BTH

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "controller"))
from render_connections import endpoint
from render_multicast import validate_group

MASK = (1 << 24) - 1
PAYLOAD = b"multicast test!\x00"


def wire_endpoint(name, node):
    return roce.Endpoint(name, node["switch_port"], node["gid_ip"], node["local_mac"],
                         node["qpn"], 65535, node["remote_gid_ip"], node["remote_qpn"],
                         node["remote_mac"], 40000)


class Suite:
    def __init__(self, sender, receivers, interfaces, timeout):
        validate_group(sender, receivers)
        self.sender, self.receivers = sender, receivers
        self.initial = sender["sq_psn"]
        self.timeout = timeout
        used_ports = {node["switch_port"] for node in [sender, *receivers]}
        self.ports = {port: roce.L2Port(interfaces[port]) for port in used_ports}
        for port in self.ports.values():
            port.drain()

    def collect(self, expected=0):
        events = []
        deadline = time.monotonic() + (self.timeout if expected else 0.2)
        by_fd = {port.fileno(): (number, port) for number, port in self.ports.items()}
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return events
            ready, _, _ = select.select(list(by_fd), [], [], remaining)
            for fd in ready:
                number, port = by_fd[fd]
                raw = port.recv(0)
                packet = roce.parse(raw) if raw else None
                if packet is not None:
                    events.append((number, packet))
                    if expected and len(events) == expected:
                        deadline = min(deadline, time.monotonic() + 0.1)

    def quiet(self):
        events = self.collect()
        assert not events, f"unexpected output: {[(p, roce.describe(v)) for p, v in events]}"

    def data(self, psn, expect=True, unknown=False):
        packet = roce.send_only(wire_endpoint("sender", self.sender), psn, PAYLOAD)
        packet[BTH].ackreq = 0  # The group must force an ACK request on every copy.
        if unknown:
            packet[BTH].dqpn += 123
        self.ports[self.sender["switch_port"]].send(packet)
        if not expect:
            self.quiet()
            return
        events = self.collect(len(self.receivers))
        assert len(events) == len(self.receivers), f"expected {len(self.receivers)} copies, got {events}"
        remaining = {(node["switch_port"], node["gid_ip"], node["qpn"]): node
                     for node in self.receivers}
        for port, pkt in events:
            node = remaining.pop((port, pkt.ip_dst, pkt.qp), None)
            assert node is not None, f"wrong/duplicate replica: port {port}, {roce.describe(pkt)}"
            expected_psn = (psn + node["rq_psn"] - self.initial) & MASK
            errors = roce.mismatches([
                ("opcode", pkt.opcode, roce.OP_SEND_ONLY),
                ("PSN", pkt.psn, expected_psn), ("payload", pkt.payload, PAYLOAD),
                ("MAC source", pkt.eth_src, node["remote_mac"]),
                ("MAC destination", pkt.eth_dst, node["local_mac"]),
                ("IP source", pkt.ip_src, node["remote_gid_ip"]),
                ("UDP source", pkt.sport, 50001), ("P_Key", pkt.p_key, 65535),
                ("ACK request", pkt.flags[2], 1), ("TTL", pkt.ttl, roce.SEND_TTL - 1),
                ("IPv4 checksum", pkt.ip_checksum_ok, True),
                ("ICRC", pkt.icrc_ok, True), ("UDP checksum", pkt.udp_checksum, 0),
            ])
            assert not errors, "; ".join(errors)

    def ack(self, member, psn, syndrome=0x1f, unknown=False):
        node = self.receivers[member]
        wire_psn = (psn + node["rq_psn"] - self.initial) & MASK
        msn = (psn - self.initial + (1 if syndrome < 0x20 else 0)) & MASK
        packet = roce.ack(wire_endpoint(f"member-{member}", node), wire_psn, msn, syndrome)
        if unknown:
            packet[BTH].dqpn += 123
        self.ports[node["switch_port"]].send(packet)

    def aggregate(self, psn, syndrome=0x1f):
        events = self.collect(1)
        assert len(events) == 1, f"expected one aggregate/negative ACK, got {events}"
        port, pkt = events[0]
        sender = self.sender
        errors = roce.mismatches([
            ("port", port, sender["switch_port"]),
            ("opcode", pkt.opcode, roce.OP_ACK), ("PSN", pkt.psn, psn),
            ("syndrome", pkt.syndrome, syndrome),
            ("destination QP", pkt.qp, sender["qpn"]),
            ("MAC source", pkt.eth_src, sender["remote_mac"]),
            ("MAC destination", pkt.eth_dst, sender["local_mac"]),
            ("IP source", pkt.ip_src, sender["remote_gid_ip"]),
            ("IP destination", pkt.ip_dst, sender["gid_ip"]),
            ("UDP source", pkt.sport, 50002), ("TTL", pkt.ttl, roce.SEND_TTL - 1),
            ("IPv4 checksum", pkt.ip_checksum_ok, True), ("ICRC", pkt.icrc_ok, True),
        ])
        if syndrome == 0x1f:
            errors += roce.mismatches([("MSN", pkt.msn, (psn - self.initial + 1) & MASK)])
        assert not errors, "; ".join(errors)

    def run(self):
        first = self.initial
        following = (first + 1) & MASK
        count = len(self.receivers)
        try:
            self.ack(0, first)
            self.quiet()
            self.data(first, expect=False, unknown=True)
            self.data(following, expect=False)
            print("[PASS] unknown connection, unsolicited ACK and wrong initial PSN are dropped", flush=True)

            self.data(first)
            self.data(following, expect=False)
            print(f"[PASS] {count} distinct replicas, QP/PSN rewrite, ACK request, ICRC and one TTL decrement", flush=True)

            self.ack(0, following)
            self.quiet()
            self.ack(0, first, unknown=True)
            self.quiet()
            self.ack(0, first, syndrome=0x40)  # Reserved AETH syndrome.
            self.quiet()
            self.ack(count - 1, first, syndrome=0x60)
            self.aggregate(first, syndrome=0x60)
            self.ack(count - 1, first, syndrome=0x25)
            self.aggregate(first, syndrome=0x25)
            print("[PASS] unknown/future ACK ignored; NAK and RNR forwarded without counting", flush=True)

            for member in range(count - 1):
                self.ack(member, first)
                self.quiet()
                self.ack(member, first)
                self.quiet()
                self.ack(member, first, syndrome=0x25)  # Already ACKed member.
                self.quiet()
            self.data(first)  # Must retain every partial ACK already counted.
            self.ack(count - 1, first, syndrome=0x03)  # Per-member receive credits.
            self.aggregate(first)
            for member in range(count):
                self.ack(member, first)
            self.quiet()
            print("[PASS] only distinct ACKs count; retransmit preserves count; one aggregate ACK", flush=True)

            self.data(first)  # Simulate loss of the aggregate ACK.
            self.ack(0, first)
            self.aggregate(first)
            for member in range(count):
                self.ack(member, first)
            self.quiet()
            print("[PASS] lost aggregate ACK recovered by retransmission without resetting the count", flush=True)

            self.data(following)
            for member in range(count):
                self.ack(member, first)
            self.quiet()
            for member in reversed(range(count)):
                self.ack(member, following)
                if member:
                    self.quiet()
                else:
                    self.aggregate(following)
            print(f"[PASS] fresh count for next PSN {following}; stale ACKs ignored; ACK order independent", flush=True)
        finally:
            for port in self.ports.values():
                port.sock.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sender_info", type=Path)
    parser.add_argument("receiver_info", type=Path, nargs="+")
    parser.add_argument("--port", action="append", default=[], metavar="NUMBER=INTERFACE")
    parser.add_argument("--timeout", type=float, default=2)
    args = parser.parse_args()
    interfaces = {1: "node-a", 2: "node-b", 3: "node-c"}
    try:
        for assignment in args.port:
            number, interface = assignment.split("=", 1)
            interfaces[int(number)] = interface
        Suite(endpoint(args.sender_info), [endpoint(path) for path in args.receiver_info],
              interfaces, args.timeout).run()
    except (AssertionError, OSError, ValueError, KeyError) as error:
        parser.exit(1, f"[FAIL] {error}\n")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
from __future__ import annotations

import argparse
import socket
import sys
import time

from scapy.all import ARP, IP, UDP, Ether, IPOption_NOP, Raw
from scapy.contrib.roce import BTH

import roce
from roce import OP_ACK, OP_SEND_ONLY, PSN_MOD, mismatches, psn_add, psn_arg


class StepFailed(Exception):
    pass


class Client:
    def __init__(self, topology: roce.Topology, port, channel: roce.Channel, args):
        self.topology = topology
        self.me = topology.client
        self.port = port
        self.channel = channel
        self.args = args
        self.next_psn = args.psn
        self.expected_echo_psn = 0
        self.delivered = 0  # our Sends the server accepted
        self.echoes = 0  # echoes we accepted and acknowledged
        self.last_send = None
        self.server_errors = 0  # server errors already reported
        self.results = []
        self.broken = False

    def hello(self) -> None:
        reply = self.channel.request(
            {
                "type": "hello",
                "psn": self.next_psn,
                "rules_sha256": self.topology.digest,
                "client_switch_port": self.me.switch_port,
            }
        )
        rule = self.topology.inbound(self.me, OP_SEND_ONLY)
        self.expected_echo_psn = psn_add(reply["psn"], rule.psn_delta)
        self.port.drain()
        print(
            f"server echo PSN {reply['psn']} -> expect PSN {self.expected_echo_psn} here\n"
        )

    def run(self) -> bool:
        self.stateful("echo #1", lambda: self.echo(1))
        self.stateful("retransmitted Send gets the same ACK", self.retransmit)
        self.stateful("out-of-sequence Send gets a NAK", self.out_of_sequence)
        for number in range(2, self.args.count + 1):
            self.stateful(f"echo #{number}", lambda n=number: self.echo(n))
        if not self.args.skip_drop:
            if self.broken:
                print(
                    "note: the echo failed, so a passing drop check may only mean "
                    "nothing is forwarded at all"
                )
            for name, frame in self.drop_cases():
                self.step(f"drop: {name}", lambda f=frame: self.expect_drop(f))
        self.step("no late errors on the server", lambda: self.check_server_errors())

        failed = sum(1 for _, ok in self.results if ok is False)
        skipped = sum(1 for _, ok in self.results if ok is None)
        passed = len(self.results) - failed - skipped
        print(f"\n{passed} passed, {failed} failed, {skipped} skipped")
        return failed == 0 and skipped == 0

    # Step bookkeeping -----------------------------------------------------

    def step(self, name: str, check) -> None:
        try:
            note = check()
        except StepFailed as failure:
            self.results.append((name, False))
            print(f"[FAIL] {name}")
            for line in failure.args[0]:
                print(f"         {line}")
        else:
            self.results.append((name, True))
            print(f"[PASS] {name}" + (f"  ({note})" if note else ""))

    def stateful(self, name: str, check) -> None:
        """Steps that depend on the connection state; skipped after a failure."""
        if self.broken:
            self.results.append((name, None))
            print(f"[SKIP] {name}")
            return
        self.step(name, check)
        self.broken = self.results[-1][1] is False

    # Packet I/O -----------------------------------------------------------

    def transmit(self, frame) -> None:
        self.port.send(frame)
        if self.args.verbose:
            pkt = roce.parse(bytes(frame))
            print(
                f"    tx {roce.describe(pkt) if pkt else Ether(bytes(frame)).summary()}"
            )

    def collect(self, expect: int, settle: float):
        """Wait for `expect` frames (up to --timeout), then `settle` seconds for extras.

        Returns (errors, ACK-opcode packets, Send packets).
        """
        frames = []
        deadline = time.monotonic() + self.args.timeout
        while len(frames) < expect:
            data = self.port.recv(max(0.0, deadline - time.monotonic()))
            if data is None:
                break
            frames.append(data)
        deadline = time.monotonic() + settle
        while (
            data := self.port.recv(max(0.0, deadline - time.monotonic()))
        ) is not None:
            frames.append(data)

        errors, acks, sends = [], [], []
        for data in frames:
            pkt = roce.parse(data)
            if self.args.verbose:
                print(f"    rx {roce.describe(pkt) if pkt else Ether(data).summary()}")
            if pkt is None:
                errors.append(f"unexpected frame: {Ether(data).summary()}")
            elif pkt.opcode == OP_ACK and pkt.syndrome is not None:
                acks.append(pkt)
            elif pkt.opcode == OP_SEND_ONLY:
                sends.append(pkt)
            else:
                errors.append(f"unexpected packet: {roce.describe(pkt)}")
        return errors, acks, sends

    def check_acks(self, acks, errors, psn: int, syndrome: int, delivered: int) -> None:
        rule = self.topology.inbound(self.me, OP_ACK)
        if len(acks) != 1:
            errors.append(f"expected 1 ACK, got {len(acks)}")
        for pkt in acks:
            found = roce.arrival_errors(pkt, rule) + mismatches(
                [
                    ("bth.psn", pkt.psn, psn),
                    ("aeth.syndrome", hex(pkt.syndrome), hex(syndrome)),
                    ("aeth.msn", pkt.msn, psn_add(delivered, rule.msn_delta)),
                ]
            )
            errors += [f"{roce.describe(pkt)}: {e}" for e in found]

    def server_stats(self, until=None) -> dict:
        """Poll the server until `until(stats)` holds, it reports a new error, or --timeout."""
        deadline = time.monotonic() + self.args.timeout
        while True:
            stats = self.channel.request({"type": "stats"})
            if (
                until is None
                or until(stats)
                or len(stats["errors"]) > self.server_errors
                or time.monotonic() >= deadline
            ):
                return stats
            time.sleep(0.05)

    def new_server_errors(self, stats: dict) -> list[str]:
        errors = [f"server {e}" for e in stats["errors"][self.server_errors :]]
        self.server_errors = len(stats["errors"])
        return errors

    # Checks ---------------------------------------------------------------

    def echo(self, number: int) -> str:
        psn, message = self.next_psn, f"{self.args.message} #{number}".encode()
        self.last_send = bytes(roce.send_only(self.me, psn, message))
        self.transmit(self.last_send)
        errors, acks, sends = self.collect(expect=2, settle=0)

        self.check_acks(acks, errors, psn, roce.SYNDROME_ACK, self.delivered + 1)
        if not errors:
            self.delivered += 1
            self.next_psn = psn_add(psn, 1)
        if len(sends) != 1:
            errors.append(f"expected 1 echo Send, got {len(sends)}")
        echo_psn = self.expected_echo_psn
        rule = self.topology.inbound(self.me, OP_SEND_ONLY)
        for pkt in sends:
            found = roce.arrival_errors(pkt, rule) + mismatches(
                [
                    ("bth.psn", pkt.psn, echo_psn),
                    ("payload", pkt.payload, message),
                ]
            )
            errors += [f"{roce.describe(pkt)}: {e}" for e in found]
            if not found and self.expected_echo_psn == echo_psn:
                self.echoes += 1
                self.expected_echo_psn = psn_add(echo_psn, 1)
                self.transmit(roce.ack(self.me, pkt.psn, self.echoes))

        stats = self.server_stats(
            lambda s: s["delivered"] >= number and s["echo_acked"] >= number
        )
        errors += self.new_server_errors(stats)
        if stats["delivered"] < number:
            errors.append("server did not accept the Send")
        elif self.echoes == number and stats["echo_acked"] < number:
            errors.append(f"server did not accept our ACK for echo PSN {echo_psn}")
        if errors:
            raise StepFailed(errors)
        return f"Send PSN {psn} ACKed, echo PSN {echo_psn} ACKed"

    def retransmit(self) -> str:
        psn = psn_add(self.next_psn, -1)
        self.transmit(self.last_send)
        errors, acks, sends = self.collect(expect=1, settle=self.args.settle)
        self.check_acks(acks, errors, psn, roce.SYNDROME_ACK, self.delivered)
        errors += [
            f"duplicate Send produced another echo: {roce.describe(p)}" for p in sends
        ]
        stats = self.server_stats()
        errors += self.new_server_errors(stats)
        if stats["delivered"] != self.delivered:
            errors.append("server delivered the duplicate again")
        if errors:
            raise StepFailed(errors)
        return f"PSN {psn} ACKed again, no second echo"

    def out_of_sequence(self) -> str:
        skipped = psn_add(self.next_psn, 1)
        self.transmit(roce.send_only(self.me, skipped, b"out of sequence"))
        errors, acks, sends = self.collect(expect=1, settle=self.args.settle)
        self.check_acks(
            acks, errors, self.next_psn, roce.SYNDROME_NAK_PSN_SEQ, self.delivered
        )
        errors += [
            f"out-of-sequence Send produced an echo: {roce.describe(p)}" for p in sends
        ]
        stats = self.server_stats()
        errors += self.new_server_errors(stats)
        if stats["delivered"] != self.delivered:
            errors.append("server delivered the out-of-sequence Send")
        if errors:
            raise StepFailed(errors)
        return f"sent PSN {skipped}, NAK asks for PSN {self.next_psn}"

    def drop_cases(self) -> list:
        me = self.me

        def send(layer=None, **fields):
            pkt = roce.send_only(me, self.next_psn, b"must be dropped")
            for name, value in fields.items():
                setattr(pkt[layer], name, value)
            return pkt

        def ack(**fields):
            pkt = roce.ack(me, psn_add(self.expected_echo_psn, -1), self.echoes)
            for name, value in fields.items():
                setattr(pkt[BTH], name, value)
            return pkt

        bad_checksum = bytearray(bytes(send()))
        bad_checksum[roce.ETH_HLEN + 10] ^= 0xFF
        return [
            (
                "unknown destination QP",
                send(BTH, dqpn=(me.remote_qp + 0x1000) % PSN_MOD),
            ),
            ("unknown destination IP", send(IP, dst="198.51.100.1")),
            ("unknown source IP", send(IP, src="198.51.100.2")),
            (
                "server's match key on the client's port",
                roce.send_only(self.topology.server, 0, b"must be dropped"),
            ),
            ("unsupported opcode SEND_FIRST", send(BTH, opcode=0x00)),
            ("BTH transport version 1", send(BTH, version=1)),
            ("IPv4 more-fragments flag", send(IP, flags="MF")),
            ("IPv4 fragment offset", send(IP, frag=1)),
            ("IPv4 options", send(IP, options=[IPOption_NOP()] * 4)),
            ("TTL 1", send(IP, ttl=1)),
            ("bad IPv4 checksum", bytes(bad_checksum)),
            ("IPv4 total length beyond the frame", send(IP, len=len(send()[IP]) + 4)),
            ("UDP destination port 4792", send(UDP, dport=4792)),
            ("ACK with payload", ack() / Raw(bytes(4))),
            ("ACK with pad count", ack(padcount=1)),
            (
                "ARP request",
                Ether(src=me.mac, dst="ff:ff:ff:ff:ff:ff")
                / ARP(hwsrc=me.mac, psrc=me.ip, pdst=me.remote_ip),
            ),
        ]

    def expect_drop(self, frame) -> None:
        before = self.server_stats()
        self.transmit(frame)
        errors, acks, sends = self.collect(expect=0, settle=self.args.settle)
        errors += [
            f"arrived back at the client: {roce.describe(p)}" for p in acks + sends
        ]
        after = self.server_stats()
        errors += self.new_server_errors(after)
        if after["received"] != before["received"]:
            errors.append(
                f"server received {after['received'] - before['received']} frame(s)"
            )
        if errors:
            raise StepFailed(errors)

    def check_server_errors(self) -> None:
        errors = self.new_server_errors(self.server_stats())
        if errors:
            raise StepFailed(errors)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--iface",
        default="node-a",
        help="interface wired to the client's switch port (default: %(default)s)",
    )
    parser.add_argument(
        "--server",
        default="127.0.0.1",
        help="server management address (default: %(default)s)",
    )
    parser.add_argument(
        "--tcp-port",
        type=int,
        default=roce.DEFAULT_TCP_PORT,
        help="management TCP port (default: %(default)s)",
    )
    parser.add_argument(
        "--psn",
        type=psn_arg,
        default=1000,
        help="initial Send PSN (default: %(default)s)",
    )
    parser.add_argument(
        "--count",
        type=int,
        default=3,
        help="number of echo messages, at least 1 (default: %(default)s)",
    )
    parser.add_argument(
        "--message",
        default="hello RDMA",
        help="echo text; ' #N' is appended (default: %(default)r)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=2.0,
        help="seconds to wait for an expected packet (default: %(default)s)",
    )
    parser.add_argument(
        "--settle",
        type=float,
        default=0.3,
        help="seconds to watch for packets that must not arrive "
        "(default: %(default)s)",
    )
    parser.add_argument("--skip-drop", action="store_true", help="skip the drop checks")
    parser.add_argument(
        "-v", "--verbose", action="store_true", help="print every packet"
    )
    roce.add_topology_args(parser)
    args = parser.parse_args(argv)
    if args.count < 1:
        parser.error("--count must be at least 1")

    try:
        topology = roce.load_topology(args.rules, args.client_switch_port)
        port = roce.L2Port(args.iface)
        channel = roce.Channel(
            socket.create_connection((args.server, args.tcp_port), timeout=5)
        )
    except roce.TopologyError as error:
        sys.exit(f"{args.rules}: {error}")
    except PermissionError:
        sys.exit("raw sockets need root or CAP_NET_RAW")
    except OSError as error:
        sys.exit(
            f"cannot start: {error} (is server.py running, and does {args.iface} exist?)"
        )

    me = topology.client
    print(
        f"client {me.ip} qp={me.qp} mac={me.mac} on {args.iface} (switch port "
        f"{me.switch_port}); sends to {me.remote_ip} qp={me.remote_qp}"
    )
    client = Client(topology, port, channel, args)
    try:
        client.hello()
        ok = client.run()
    except (OSError, EOFError, RuntimeError) as error:
        sys.exit(f"management connection: {error}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

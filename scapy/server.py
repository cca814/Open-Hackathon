#!/usr/bin/env python3
"""Emulate the RDMA server (node B) behind the P4 switch.

Each Send is answered with an ACK and an echo Send, the same exchange as
rdma/server.c. Every packet the switch delivers is checked against the
translation the rules call for; the client reads the results over the TCP
management connection and reports them.
"""

from __future__ import annotations

import argparse
import select
import socket
import sys

from scapy.all import Ether

import roce
from roce import OP_ACK, OP_SEND_ONLY, PSN_MOD, psn_add, psn_arg


def log(text: str) -> None:
    print(text, flush=True)


class Session:
    """State of one client connection, reset on every hello."""

    def __init__(self, channel: roce.Channel):
        self.channel = channel
        self.ready = False
        self.expected_psn = 0  # next client Send PSN, as translated by the switch
        self.send_psn = 0  # next echo PSN
        self.unacked = {}  # echo PSN -> echo number
        self.echoes = 0
        self.delivered = 0
        self.echo_acked = 0
        self.nak_sent = False
        self.received = 0
        self.errors = []


class Server:
    def __init__(self, topology: roce.Topology, port, listener: socket.socket, psn: int):
        self.topology = topology
        self.me = topology.server
        self.port = port
        self.listener = listener
        self.psn = psn
        self.session = None

    def run(self) -> None:
        while True:
            watch = [self.port, self.listener] + ([self.session.channel] if self.session else [])
            for ready in select.select(watch, [], [])[0]:
                if ready is self.port:
                    self.on_frame(self.port.recv(0))
                elif ready is self.listener:
                    self.on_accept()
                elif self.session and ready is self.session.channel:
                    self.on_management()

    # Management connection --------------------------------------------------

    def on_accept(self) -> None:
        conn, address = self.listener.accept()
        if self.session:
            log("new client connection replaces the current session")
            self.end_session()
        log(f"client connected from {address[0]}:{address[1]}")
        self.session = Session(roce.Channel(conn))

    def end_session(self) -> None:
        s = self.session
        self.session = None
        s.channel.sock.close()
        log(f"session closed: {s.delivered} delivered, {s.echo_acked} echoes acknowledged, "
            f"{len(s.errors)} errors\n")

    def on_management(self) -> None:
        try:
            messages = self.session.channel.receive_ready()
        except (EOFError, OSError, ValueError):
            self.end_session()
            return
        for message in messages:
            if message.get("type") == "hello":
                self.on_hello(message)
            elif message.get("type") == "stats":
                s = self.session
                s.channel.send({"type": "stats", "received": s.received, "delivered": s.delivered,
                                "echo_acked": s.echo_acked, "errors": s.errors})
            if self.session is None:
                return

    def on_hello(self, message: dict) -> None:
        s = self.session
        problem = None
        if message.get("rules_sha256") != self.topology.digest:
            problem = "client and server loaded different rules files"
        elif message.get("client_switch_port") != self.topology.client.switch_port:
            problem = (f"client uses switch port {message.get('client_switch_port')}, "
                       f"server expects {self.topology.client.switch_port}")
        elif not isinstance(message.get("psn"), int) or not 0 <= message["psn"] < PSN_MOD:
            problem = "invalid client PSN"
        if problem:
            log(f"rejecting client: {problem}")
            s.channel.send({"type": "error", "message": problem})
            self.end_session()
            return
        rule = self.topology.inbound(self.me, OP_SEND_ONLY)
        s.expected_psn = psn_add(message["psn"], rule.psn_delta)
        s.send_psn = self.psn
        s.ready = True
        self.port.drain()
        log(f"client send PSN {message['psn']} -> expect PSN {s.expected_psn} here; "
            f"echo PSN starts at {self.psn}")
        s.channel.send({"type": "hello", "psn": self.psn})

    # RoCE packets ---------------------------------------------------------

    def on_frame(self, data: bytes | None) -> None:
        if data is None:
            return
        pkt = roce.parse(data)
        text = roce.describe(pkt) if pkt else Ether(data).summary()
        s = self.session
        if not s or not s.ready:
            log(f"rx {text}  (ignored: no client session)")
            return
        s.received += 1
        if pkt is None:
            self.reject(text, ["not a RoCEv2 packet"])
        elif pkt.opcode == OP_SEND_ONLY:
            self.on_send(pkt, text)
        elif pkt.opcode == OP_ACK:
            self.on_ack(pkt, text)
        else:
            self.reject(text, [f"unsupported opcode {pkt.opcode:#04x}"])

    def reject(self, text: str, errors: list[str]) -> None:
        log(f"rx {text}\n   FAIL, dropped:")
        for error in errors:
            log(f"     {error}")
        self.session.errors += [f"rx {text}: {error}" for error in errors]

    def transmit(self, frame, note: str) -> None:
        self.port.send(frame)
        log(f"tx {roce.describe(roce.parse(bytes(frame)))}  ({note})")

    def on_send(self, pkt: roce.RocePacket, text: str) -> None:
        errors = roce.arrival_errors(pkt, self.topology.inbound(self.me, OP_SEND_ONLY))
        if errors:
            self.reject(text, errors)
            return
        s = self.session
        ahead = (pkt.psn - s.expected_psn) % PSN_MOD
        if ahead == 0:
            s.expected_psn = psn_add(s.expected_psn, 1)
            s.delivered += 1
            s.nak_sent = False
            log(f"rx {text}  OK, message {pkt.payload!r}")
            self.transmit(roce.ack(self.me, pkt.psn, s.delivered), "ACK")
            s.echoes += 1
            s.unacked[s.send_psn] = s.echoes
            self.transmit(roce.send_only(self.me, s.send_psn, pkt.payload), "echo")
            s.send_psn = psn_add(s.send_psn, 1)
        elif ahead >= PSN_MOD // 2:
            log(f"rx {text}  OK, duplicate")
            self.transmit(roce.ack(self.me, psn_add(s.expected_psn, -1), s.delivered),
                          "ACK for duplicate")
        elif not s.nak_sent:
            s.nak_sent = True
            log(f"rx {text}  OK, out of sequence (expected PSN {s.expected_psn})")
            self.transmit(roce.ack(self.me, s.expected_psn, s.delivered,
                                   roce.SYNDROME_NAK_PSN_SEQ), "NAK PSN sequence error")
        else:
            log(f"rx {text}  OK, out of sequence after NAK, ignored")

    def on_ack(self, pkt: roce.RocePacket, text: str) -> None:
        rule = self.topology.inbound(self.me, OP_ACK)
        errors = roce.arrival_errors(pkt, rule)
        s = self.session
        number = s.unacked.get(pkt.psn)
        if number is None:
            errors.append(f"bth.psn: {pkt.psn} is not an outstanding echo PSN "
                          f"{sorted(s.unacked)}")
        else:
            errors += roce.mismatches([
                ("aeth.syndrome", hex(pkt.syndrome), hex(roce.SYNDROME_ACK)),
                ("aeth.msn", pkt.msn, psn_add(number, rule.msn_delta)),
            ])
        if errors:
            self.reject(text, errors)
            return
        s.unacked = {psn: n for psn, n in s.unacked.items() if n > number}
        s.echo_acked = max(s.echo_acked, number)
        log(f"rx {text}  OK, echo #{number} acknowledged")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--iface", default="node-b",
                        help="interface wired to the server's switch port (default: %(default)s)")
    parser.add_argument("--listen", default="127.0.0.1",
                        help="management address (default: %(default)s)")
    parser.add_argument("--tcp-port", type=int, default=roce.DEFAULT_TCP_PORT,
                        help="management TCP port (default: %(default)s)")
    parser.add_argument("--psn", type=psn_arg, default=9000,
                        help="initial PSN of the echo Sends (default: %(default)s)")
    roce.add_topology_args(parser)
    args = parser.parse_args(argv)

    try:
        topology = roce.load_topology(args.rules, args.client_switch_port)
        port = roce.L2Port(args.iface)
    except roce.TopologyError as error:
        sys.exit(f"{args.rules}: {error}")
    except PermissionError:
        sys.exit("raw sockets need root or CAP_NET_RAW")
    except OSError as error:
        sys.exit(f"{args.iface}: {error}")
    listener = socket.create_server((args.listen, args.tcp_port))

    me, rule = topology.server, topology.inbound(topology.server, OP_SEND_ONLY)
    log(f"server {me.ip} qp={me.qp} mac={me.mac} on {args.iface} (switch port {me.switch_port})")
    log(f"expects client packets as {rule.new_src_ip}:{rule.new_udp_sport} -> {me.ip}, "
        f"Send PSN offset {rule.psn_delta}")
    log(f"management on {args.listen}:{args.tcp_port}, waiting for the client\n")
    try:
        Server(topology, port, listener, args.psn).run()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())

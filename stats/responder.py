#!/usr/bin/env python3
"""Measurement responder on node B (server side of every connection).

Answers each Send with an ACK (or every Nth Send) and, for latency runs, an
echo Send; logs the kernel receive time of every Send. Unlike scapy/server.py
it does not validate headers (scapy/client.py covers correctness) and does not
NAK: it counts the sequence errors an RC receiver would NAK instead, since the
load generator does not retransmit.

The load generator (bench.py) controls runs over TCP, newline-delimited JSON:
  {"type": "start", "echo": bool, "ack_every": int, "size": int, "out": path}
      -> {"type": "started"}; with echo, Send templates of `size` are built first
  {"type": "stop"}  -> {"type": "stopped", ...summary}
Frames that arrive outside a run are ignored.
"""

from __future__ import annotations

import argparse
import select
import socket
import sys
import time

from common import (CONTROL_PORT, OP_ACK, OP_SEND_ONLY, PAYLOAD_OFF, PSN_MOD, Frames, RawPort,
                    load_connections, peek, roce, write_csv)


class Connection:
    def __init__(self, index: int, topology: roce.Topology, echo_psn: int):
        self.index = index
        self.frames = Frames(topology.server)
        self.send_delta = topology.inbound(topology.server, OP_SEND_ONLY).psn_delta
        self.echo_psn = echo_psn
        self.reset()

    def reset(self) -> None:
        self.expected = None  # next client PSN (client numbering)
        self.seen = set()
        self.delivered = 0


class Run:
    def __init__(self, message: dict):
        self.echo = bool(message.get("echo"))
        self.ack_every = max(1, int(message.get("ack_every", 1)))
        self.out = message.get("out")
        self.log = []  # (conn, client PSN, rx_ns, ack_tx_ns, echo_tx_ns)
        self.received = self.in_order = self.duplicates = self.reordered = 0
        self.gaps = self.missing = 0  # sequence errors an RC receiver would NAK
        self.acks_sent = self.echoes_sent = self.echo_acks = 0
        self.ignored = 0


class Responder:
    def __init__(self, connections, port: RawPort, listener, echo_psn: int):
        self.port = port
        self.listener = listener
        self.by_qp = {t.server.qp: Connection(i, t, echo_psn) for i, t in enumerate(connections)}
        if len(self.by_qp) != len(connections):
            sys.exit("two connections share a server QP")
        self.control = None
        self.run = None

    def serve(self) -> None:
        while True:
            watch = [self.port, self.listener] + ([self.control] if self.control else [])
            for ready in select.select(watch, [], [])[0]:
                if ready is self.port:
                    self.on_frames()
                elif ready is self.listener:
                    if self.control:
                        self.control.sock.close()
                    self.control = roce.Channel(self.listener.accept()[0])
                else:
                    self.on_control()

    def on_control(self) -> None:
        try:
            messages = self.control.receive_ready()
        except (EOFError, OSError, ValueError):
            self.control.sock.close()
            self.control = None
            return
        for message in messages:
            if message.get("type") == "start":
                for conn in self.by_qp.values():
                    conn.reset()
                    if message.get("echo") and message.get("size"):
                        conn.frames.send_only(0, int(message["size"]))  # build before timing
                self.port.drain()
                self.run = Run(message)
                self.control.send({"type": "started"})
            elif message.get("type") == "stop":
                self.on_frames()
                self.control.send(self.finish())

    def finish(self) -> dict:
        run, self.run = self.run, None
        if run is None:
            return {"type": "error", "message": "no run in progress"}
        if run.out:
            write_csv(run.out, ["conn", "psn", "rx_ns", "ack_tx_ns", "echo_tx_ns"], run.log)
        fields = ("received", "in_order", "duplicates", "reordered", "gaps", "missing",
                  "acks_sent", "echoes_sent", "echo_acks", "ignored")
        return {"type": "stopped", **{name: getattr(run, name) for name in fields}}

    def on_frames(self) -> None:
        while (got := self.port.recv_nowait()) is not None:
            if self.run is not None:
                self.on_frame(*got)

    def on_frame(self, data: bytes, rx_ns: int) -> None:
        run = self.run
        info = peek(data)
        conn = self.by_qp.get(info[1]) if info else None
        if conn is None:
            run.ignored += 1
            return
        opcode, _, psn, _, size = info
        if opcode == OP_ACK:
            run.echo_acks += 1
            return
        if opcode != OP_SEND_ONLY:
            run.ignored += 1
            return

        run.received += 1
        client_psn = (psn - conn.send_delta) % PSN_MOD
        if conn.expected is None:
            conn.expected = client_psn
        ahead = (client_psn - conn.expected) % PSN_MOD
        if ahead >= PSN_MOD // 2:  # older than expected
            if client_psn in conn.seen:
                run.duplicates += 1
            else:
                run.reordered += 1
        else:
            if ahead:
                run.gaps += 1
                run.missing += ahead
            else:
                run.in_order += 1
            conn.expected = (client_psn + 1) % PSN_MOD
        conn.seen.add(client_psn)

        ack_ns = echo_ns = 0
        conn.delivered += 1
        if conn.delivered % run.ack_every == 0:
            frame = conn.frames.ack(psn, conn.delivered)
            ack_ns = time.time_ns()
            self.port.send(frame)
            run.acks_sent += 1
        if run.echo:
            frame = conn.frames.send_only(conn.echo_psn, size, data[PAYLOAD_OFF:PAYLOAD_OFF + size])
            echo_ns = time.time_ns()
            self.port.send(frame)
            conn.echo_psn = (conn.echo_psn + 1) % PSN_MOD
            run.echoes_sent += 1
        run.log.append((conn.index, client_psn, rx_ns, ack_ns, echo_ns))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--rules", required=True, help="rules file installed on the switch")
    parser.add_argument("--iface", default="node-b")
    parser.add_argument("--listen", default="127.0.0.1")
    parser.add_argument("--control-port", type=int, default=CONTROL_PORT)
    parser.add_argument("--echo-psn", type=roce.psn_arg, default=9000,
                        help="first echo PSN of every connection (default: %(default)s)")
    args = parser.parse_args(argv)

    try:
        connections = load_connections(args.rules)
        port = RawPort(args.iface)
    except roce.TopologyError as error:
        sys.exit(f"{args.rules}: {error}")
    except PermissionError:
        sys.exit("raw sockets need root or CAP_NET_RAW")
    listener = socket.create_server((args.listen, args.control_port))
    print(f"responder: {len(connections)} connection(s) on {args.iface}, "
          f"control {args.listen}:{args.control_port}", flush=True)
    try:
        Responder(connections, port, listener, args.echo_psn).serve()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())

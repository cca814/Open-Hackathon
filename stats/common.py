"""Shared pieces of the P4 statistics tools.

Packets are built once with scapy (scapy/roce.py) and then reused as byte
templates: per packet only the PSN, AETH and ICRC change. Dissecting every
frame with scapy costs ~100 us, which would make the emulated nodes, not the
switch, the bottleneck of a throughput test.
"""

from __future__ import annotations

import csv
import hashlib
import select
import socket
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scapy"))

import roce  # noqa: E402
from roce import OP_ACK, OP_SEND_ONLY, PSN_MOD, SYNDROME_ACK  # noqa: E402,F401

CONTROL_PORT = 18516  # responder control; scapy/server.py uses 18515

ETH_HLEN = 14
BTH_OFF = ETH_HLEN + 20 + 8  # IPv4 without options, UDP
PSN_OFF = BTH_OFF + 9
AETH_OFF = BTH_OFF + 12  # ACK
PAYLOAD_OFF = BTH_OFF + 12  # Send Only

ETH_P_ALL = 0x0003
SOL_PACKET = 263
PACKET_IGNORE_OUTGOING = 23
SO_TIMESTAMPNS = 35
SO_RCVBUFFORCE = 33


# --------------------------------------------------------------------------
# Connections


def load_connections(path) -> list[roce.Topology]:
    """One roce.Topology per connection; the rules file lists each connection's
    four rules consecutively, the first one being the client's Send."""
    text = Path(path).read_text()
    rules = roce.parse_rules(text)
    if not rules or len(rules) % 4:
        raise roce.TopologyError(f"{path}: need 4 rules per connection, found {len(rules)}")
    digest = hashlib.sha256(text.encode()).hexdigest()
    connections = []
    for start in range(0, len(rules), 4):
        chunk = rules[start:start + 4]
        by_key = {(r.ingress_port, r.opcode): r for r in chunk}
        ports = sorted({p for p, _ in by_key})
        if len(by_key) != 4 or len(ports) != 2 or {o for _, o in by_key} != {OP_SEND_ONLY, OP_ACK}:
            raise roce.TopologyError(f"lines {[r.line for r in chunk]}: not Send and ACK on two ports")
        client_port = chunk[0].ingress_port
        server_port = ports[1] if client_port == ports[0] else ports[0]
        topology = roce.Topology(
            roce._endpoint("client", client_port, by_key, roce.CLIENT_UDP_SPORT),
            roce._endpoint("server", server_port, by_key, roce.SERVER_UDP_SPORT),
            by_key, digest)
        for node in (topology.client, topology.server):
            send, ack = topology.outbound(node, OP_SEND_ONLY), topology.inbound(node, OP_ACK)
            if roce.psn_add(send.psn_delta, ack.psn_delta) != 0:
                raise roce.TopologyError(f"lines {send.line}/{ack.line}: ACK psn_delta does not "
                                         f"cancel Send psn_delta")
        connections.append(topology)
    return connections


# --------------------------------------------------------------------------
# Frames


class Frames:
    """Byte templates for the packets one endpoint sends."""

    def __init__(self, node: roce.Endpoint):
        self.node = node
        self.sends = {}  # payload size -> template
        self.ack_template = bytearray(bytes(roce.ack(node, 0, 0)))

    def send_only(self, psn: int, size: int, payload: bytes | None = None) -> bytes:
        frame = self.sends.get(size)
        if frame is None:
            frame = self.sends[size] = bytearray(bytes(roce.send_only(self.node, 0, bytes(size))))
        if payload is not None:
            frame[PAYLOAD_OFF:PAYLOAD_OFF + size] = payload
        frame[PSN_OFF:PSN_OFF + 3] = psn.to_bytes(3, "big")
        return _with_icrc(frame)

    def ack(self, psn: int, msn: int, syndrome: int = SYNDROME_ACK) -> bytes:
        frame = self.ack_template
        frame[PSN_OFF:PSN_OFF + 3] = psn.to_bytes(3, "big")
        frame[AETH_OFF] = syndrome
        frame[AETH_OFF + 1:AETH_OFF + 4] = (msn % PSN_MOD).to_bytes(3, "big")
        return _with_icrc(frame)


def _with_icrc(frame: bytearray) -> bytes:
    end = ETH_HLEN + int.from_bytes(frame[ETH_HLEN + 2:ETH_HLEN + 4], "big")
    frame[end - 4:end] = roce.icrc(bytes(frame[ETH_HLEN:end - 4]))
    return bytes(frame)


def peek(data: bytes):
    """(opcode, dest QP, PSN, AETH syndrome or None, payload length) of a RoCEv2
    frame without IP options, or None for anything else."""
    if (len(data) < AETH_OFF + 4 or data[12:14] != b"\x08\x00" or data[ETH_HLEN] != 0x45
            or data[ETH_HLEN + 9] != 17 or data[ETH_HLEN + 22:ETH_HLEN + 24] != b"\x12\xb7"):
        return None
    opcode = data[BTH_OFF]
    qp = int.from_bytes(data[BTH_OFF + 5:BTH_OFF + 8], "big")
    psn = int.from_bytes(data[PSN_OFF:PSN_OFF + 3], "big")
    if opcode == OP_ACK:
        return opcode, qp, psn, data[AETH_OFF], 0
    ip_len = int.from_bytes(data[ETH_HLEN + 2:ETH_HLEN + 4], "big")
    pad = data[BTH_OFF + 1] >> 4 & 0x3
    return opcode, qp, psn, None, ip_len - 20 - 8 - 12 - 4 - pad


# --------------------------------------------------------------------------
# I/O


class RawPort:
    """AF_PACKET socket that returns inbound frames with their kernel receive
    time (CLOCK_REALTIME ns, comparable to time.time_ns() of the sender)."""

    def __init__(self, iface: str, rcvbuf: int = 64 << 20):
        self.iface = iface
        self.sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
        for option in (SO_RCVBUFFORCE, socket.SO_RCVBUF):
            try:
                self.sock.setsockopt(socket.SOL_SOCKET, option, rcvbuf)
                break
            except OSError:
                continue
        self.sock.bind((iface, ETH_P_ALL))
        self.sock.setsockopt(socket.SOL_SOCKET, SO_TIMESTAMPNS, 1)
        try:
            self.sock.setsockopt(SOL_PACKET, PACKET_IGNORE_OUTGOING, 1)
        except OSError:
            pass  # kernel < 4.20: recv() filters by packet type instead

    def fileno(self) -> int:
        return self.sock.fileno()

    def send(self, data: bytes) -> None:
        self.sock.send(data)

    def recv(self, timeout: float | None):
        """(frame, rx_ns), or None when nothing arrived within `timeout`."""
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            remaining = None if deadline is None else max(0.0, deadline - time.monotonic())
            if not select.select([self.sock], [], [], remaining)[0]:
                return None
            got = self.recv_nowait()
            if got is not None:
                return got

    def recv_nowait(self):
        """(frame, rx_ns), or None when the socket queue is empty."""
        while True:
            try:
                data, ancillary, _, address = self.sock.recvmsg(65535, 64, socket.MSG_DONTWAIT)
            except BlockingIOError:
                return None
            if address[2] == socket.PACKET_OUTGOING:
                continue
            for level, kind, value in ancillary:
                if level == socket.SOL_SOCKET and kind == SO_TIMESTAMPNS:
                    sec, nsec = struct.unpack("qq", value[:16])
                    return data, sec * 1_000_000_000 + nsec
            return data, time.time_ns()

    def drain(self) -> None:
        while self.recv_nowait() is not None:
            pass


def write_csv(path, header, rows) -> None:
    with open(path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(header)
        writer.writerows(rows)


def read_csv(path) -> list[dict]:
    with open(path, newline="") as f:
        return list(csv.DictReader(f))

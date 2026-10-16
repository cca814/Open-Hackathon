"""Shared RoCEv2 helpers for the single-host scapy test of the P4 pipeline.

The client and server emulate the two RDMA nodes. Their identities and the
expected translations come from the BMv2 rules file, so the test follows
whatever connection the switch was configured with. Linux only: frames go
through AF_PACKET sockets on veth interfaces.
"""

from __future__ import annotations

import argparse
import hashlib
import ipaddress
import json
import select
import socket
import struct
import time
import zlib
from dataclasses import dataclass
from pathlib import Path

# Import through scapy.all first: importing single layers hits import cycles in older scapy.
from scapy.all import IP, UDP, Ether, Raw, checksum
from scapy.contrib.roce import AETH, BTH

DEFAULT_RULES = Path(__file__).resolve().parent.parent / "controller" / "connections.example.txt"
DEFAULT_TCP_PORT = 18515

ETH_P_ALL = 0x0003
ETH_HLEN = 14
ROCE_UDP_PORT = 4791
OP_SEND_ONLY = 0x04
OP_ACK = 0x11
PSN_MOD = 1 << 24

# Values the emulated nodes put on the wire that the switch must leave alone.
SEND_TTL = 64
TOS = 0x6A  # DSCP 26 with ECT(0)
IP_FLAG_DF = 0x2
FLAGS = {  # BTH (SE, MigReq, AckReq) per opcode
    OP_SEND_ONLY: (1, 1, 1),
    OP_ACK: (0, 1, 0),
}

SYNDROME_ACK = 0x1F  # ACK without end-to-end credit
SYNDROME_NAK_PSN_SEQ = 0x60

CLIENT_UDP_SPORT = 49152
SERVER_UDP_SPORT = 49153

TABLE = "MyIngress.connections"
ACTION = "MyIngress.translate_connection"


class TopologyError(Exception):
    pass


def psn_add(psn: int, delta: int) -> int:
    return (psn + delta) % PSN_MOD


def psn_arg(text: str) -> int:
    value = int(text, 0)
    if not 0 <= value < PSN_MOD:
        raise argparse.ArgumentTypeError("PSN must be 0..0xffffff")
    return value


# --------------------------------------------------------------------------
# Rules file -> node identities


@dataclass(frozen=True)
class Rule:
    line: int
    ingress_port: int
    src_ip: str
    dst_ip: str
    dst_qp: int
    opcode: int
    new_src_mac: str
    new_dst_mac: str
    new_src_ip: str
    new_dst_ip: str
    new_udp_sport: int
    new_dst_qp: int
    p_key: int
    psn_delta: int
    msn_delta: int
    egress_port: int


def _int(text: str) -> int:
    return int(text, 0)


def _ip(text: str) -> str:
    return str(ipaddress.IPv4Address(text if "." in text else _int(text)))


def _mac(text: str) -> str:
    value = int(text.replace(":", ""), 16) if ":" in text else _int(text)
    return ":".join(f"{b:02x}" for b in value.to_bytes(6, "big"))


def parse_rules(text: str) -> list[Rule]:
    """Parse the `table_add` lines for the connection table, ignoring others."""
    rules = []
    for number, line in enumerate(text.splitlines(), 1):
        words = line.split()
        if words[:3] != ["table_add", TABLE, ACTION]:
            continue
        if "=>" not in words:
            raise TopologyError(f"line {number}: missing '=>'")
        split = words.index("=>")
        keys, params = words[3:split], words[split + 1:]
        if len(keys) != 5 or len(params) != 10:
            raise TopologyError(f"line {number}: expected 5 match keys and 10 action parameters")
        try:
            rules.append(Rule(number, _int(keys[0]), _ip(keys[1]), _ip(keys[2]), _int(keys[3]),
                              _int(keys[4]), _mac(params[0]), _mac(params[1]), _ip(params[2]),
                              _ip(params[3]), *(_int(p) for p in params[4:])))
        except ValueError as error:
            raise TopologyError(f"line {number}: {error}") from None
    return rules


@dataclass(frozen=True)
class Endpoint:
    name: str
    switch_port: int
    ip: str
    mac: str
    qp: int
    p_key: int
    remote_ip: str  # peer identity this node's QP is configured with
    remote_qp: int
    gateway_mac: str  # switch-facing MAC used as the Ethernet destination
    udp_sport: int


@dataclass(frozen=True)
class Topology:
    client: Endpoint
    server: Endpoint
    rules: dict  # (ingress port, opcode) -> Rule
    digest: str

    def peer(self, node: Endpoint) -> Endpoint:
        return self.server if node.name == "client" else self.client

    def outbound(self, node: Endpoint, opcode: int) -> Rule:
        """Rule applied to packets `node` sends."""
        return self.rules[(node.switch_port, opcode)]

    def inbound(self, node: Endpoint, opcode: int) -> Rule:
        """Rule that produced the packets `node` receives."""
        return self.outbound(self.peer(node), opcode)


def _endpoint(name: str, port: int, rules: dict, udp_sport: int) -> Endpoint:
    send, ack = rules[(port, OP_SEND_ONLY)], rules[(port, OP_ACK)]
    peer_port = next(p for p, _ in rules if p != port)
    to_me = [rules[(peer_port, OP_SEND_ONLY)], rules[(peer_port, OP_ACK)]]

    def same(what: str, values: list, lines: list) -> None:
        if len(set(values)) != 1:
            raise TopologyError(f"lines {lines}: {name} {what} differs: {values}")

    same("source IP / remote IP / remote QP", [(r.src_ip, r.dst_ip, r.dst_qp) for r in (send, ack)],
         [send.line, ack.line])
    for what, field in (("destination MAC", "new_dst_mac"), ("destination QP", "new_dst_qp"),
                        ("P_Key", "p_key")):
        same(what, [getattr(r, field) for r in to_me], [r.line for r in to_me])
    for rule in to_me:
        problems = [f"{label} is {got}, expected {want}" for label, got, want in (
            ("egress port", rule.egress_port, port),
            ("destination IP", rule.new_dst_ip, send.src_ip),
            ("source IP", rule.new_src_ip, send.dst_ip),
        ) if got != want]
        if problems:
            raise TopologyError(f"line {rule.line}: packets to the {name} at port {port}: "
                                + "; ".join(problems))
    return Endpoint(name, port, send.src_ip, to_me[0].new_dst_mac, to_me[0].new_dst_qp,
                    to_me[0].p_key, send.dst_ip, send.dst_qp, to_me[0].new_src_mac, udp_sport)


def load_topology(path: Path, client_switch_port: int | None = None) -> Topology:
    """Derive both nodes from the four rules of one bidirectional RC connection."""
    text = Path(path).read_text()
    rules = parse_rules(text)
    if len(rules) != 4:
        raise TopologyError(f"{path}: need exactly 4 {ACTION} rules (Send and ACK in each "
                            f"direction), found {len(rules)}")
    by_key = {}
    for rule in rules:
        if rule.opcode not in FLAGS:
            raise TopologyError(f"line {rule.line}: unsupported opcode {rule.opcode:#x}")
        if (rule.ingress_port, rule.opcode) in by_key:
            raise TopologyError(f"line {rule.line}: second rule for port {rule.ingress_port} "
                                f"opcode {rule.opcode:#04x}")
        by_key[(rule.ingress_port, rule.opcode)] = rule
    ports = sorted({p for p, _ in by_key})
    if len(ports) != 2 or len(by_key) != 4:
        raise TopologyError(f"{path}: need Send and ACK rules on exactly two ingress ports, "
                            f"found {sorted(by_key)}")
    client_port = rules[0].ingress_port if client_switch_port is None else client_switch_port
    if client_port not in ports:
        raise TopologyError(f"--client-switch-port {client_port} is not one of {ports}")
    server_port = ports[1] if client_port == ports[0] else ports[0]
    topology = Topology(_endpoint("client", client_port, by_key, CLIENT_UDP_SPORT),
                        _endpoint("server", server_port, by_key, SERVER_UDP_SPORT),
                        by_key, hashlib.sha256(text.encode()).hexdigest())

    # An ACK confirms the PSN of the Send it answers, so its offset must undo that Send's.
    for node in (topology.client, topology.server):
        send, ack = topology.outbound(node, OP_SEND_ONLY), topology.inbound(node, OP_ACK)
        if psn_add(send.psn_delta, ack.psn_delta) != 0:
            raise TopologyError(f"lines {send.line}/{ack.line}: ACK psn_delta {ack.psn_delta} "
                                f"does not cancel Send psn_delta {send.psn_delta}")
    return topology


def add_topology_args(parser) -> None:
    parser.add_argument("--rules", type=Path, default=DEFAULT_RULES,
                        help="BMv2 CLI rules installed on the switch (default: %(default)s)")
    parser.add_argument("--client-switch-port", type=int,
                        help="ingress port of the client in the rules (default: first rule's)")


# --------------------------------------------------------------------------
# Packets


def icrc(ip_packet: bytes) -> bytes:
    """RoCEv2 ICRC of an IPv4 packet that does not yet carry one.

    Same masking as Linux rxe_icrc.c: eight 0xff bytes stand in for the LRH;
    IPv4 TOS, TTL and checksum, the UDP checksum and BTH resv8a become all ones.
    """
    masked = bytearray(ip_packet)
    udp = (masked[0] & 0x0F) * 4
    masked[1] = 0xFF
    masked[8] = 0xFF
    masked[10:12] = b"\xff\xff"
    masked[udp + 6:udp + 8] = b"\xff\xff"
    masked[udp + 12] = 0xFF
    return struct.pack("<I", zlib.crc32(b"\xff" * 8 + bytes(masked)))


def frame(node: Endpoint, transport) -> Ether:
    """Ethernet/IPv4/UDP headers `node` uses toward its configured remote."""
    return (Ether(src=node.mac, dst=node.gateway_mac)
            / IP(src=node.ip, dst=node.remote_ip, tos=TOS, ttl=SEND_TTL, flags="DF")
            / UDP(sport=node.udp_sport, dport=ROCE_UDP_PORT, chksum=0)
            / transport)


def _bth(node: Endpoint, opcode: int, psn: int, pad: int = 0) -> BTH:
    se, migreq, ackreq = FLAGS[opcode]
    return BTH(opcode=opcode, solicited=se, migreq=migreq, padcount=pad, pkey=node.p_key,
               dqpn=node.remote_qp, ackreq=ackreq, psn=psn)


def send_only(node: Endpoint, psn: int, payload: bytes) -> Ether:
    pad = -len(payload) % 4
    return frame(node, _bth(node, OP_SEND_ONLY, psn, pad) / Raw(payload + bytes(pad)))


def ack(node: Endpoint, psn: int, msn: int, syndrome: int = SYNDROME_ACK) -> Ether:
    return frame(node, _bth(node, OP_ACK, psn) / AETH(syndrome=syndrome, msn=msn % PSN_MOD))


@dataclass(frozen=True)
class RocePacket:
    eth_src: str
    eth_dst: str
    ip_src: str
    ip_dst: str
    tos: int
    ttl: int
    ip_flags: int
    frag: int
    ip_len: int
    ip_checksum_ok: bool
    sport: int
    dport: int
    udp_len: int
    udp_checksum: int
    opcode: int
    flags: tuple
    pad: int
    tver: int
    p_key: int
    resv8a: int
    qp: int
    resv7: int
    psn: int
    syndrome: int | None
    msn: int | None
    payload: bytes  # Send payload without padding
    icrc_ok: bool


def parse(data: bytes) -> RocePacket | None:
    """Decode an Ethernet/IPv4/UDP 4791 frame; None for anything else."""
    eth = Ether(data)
    if IP not in eth or UDP not in eth or BTH not in eth:
        return None
    ip, udp, bth = eth[IP], eth[UDP], eth[BTH]
    ip_bytes = data[ETH_HLEN:ETH_HLEN + ip.len]
    icrc_ok = len(ip_bytes) == ip.len and ip_bytes[-4:] == icrc(ip_bytes[:-4])
    syndrome = msn = None
    payload = b""
    if AETH in bth:
        syndrome, msn = bth[AETH].syndrome, bth[AETH].msn
    elif Raw in bth:
        payload = bth[Raw].load
        payload = payload[:len(payload) - bth.padcount]
    return RocePacket(
        eth.src, eth.dst, ip.src, ip.dst, ip.tos, ip.ttl, int(ip.flags), ip.frag, ip.len,
        checksum(data[ETH_HLEN:ETH_HLEN + ip.ihl * 4]) == 0, udp.sport, udp.dport, udp.len,
        udp.chksum, bth.opcode, (bth.solicited, bth.migreq, bth.ackreq), bth.padcount,
        bth.version, bth.pkey, bth.fecn << 7 | bth.becn << 6 | bth.resv6, bth.dqpn, bth.resv7,
        bth.psn, syndrome, msn, payload, icrc_ok)


def syndrome_name(syndrome: int) -> str:
    return {0: "ACK", 1: "RNR-NAK", 2: "reserved", 3: "NAK"}[syndrome >> 5 & 0x3]


def describe(pkt: RocePacket) -> str:
    if pkt.opcode == OP_ACK and pkt.syndrome is not None:
        kind = syndrome_name(pkt.syndrome)
        detail = f"syndrome={pkt.syndrome:#04x} msn={pkt.msn}"
    else:
        kind = "SEND" if pkt.opcode == OP_SEND_ONLY else f"opcode={pkt.opcode:#04x}"
        detail = f"len={len(pkt.payload)}"
    return (f"{kind} {pkt.ip_src}:{pkt.sport} -> {pkt.ip_dst} qp={pkt.qp} psn={pkt.psn} "
            f"{detail} ttl={pkt.ttl} icrc={'ok' if pkt.icrc_ok else 'BAD'}")


def mismatches(checks) -> list[str]:
    return [f"{name}: got {got}, expected {want}" for name, got, want in checks if got != want]


def arrival_errors(pkt: RocePacket, rule: Rule) -> list[str]:
    """Header differences between a delivered packet and what `rule` should produce."""
    is_ack = rule.opcode == OP_ACK
    return mismatches([
        ("eth.src", pkt.eth_src, rule.new_src_mac),
        ("eth.dst", pkt.eth_dst, rule.new_dst_mac),
        ("ip.src", pkt.ip_src, rule.new_src_ip),
        ("ip.dst", pkt.ip_dst, rule.new_dst_ip),
        ("ip.tos", hex(pkt.tos), hex(TOS)),
        ("ip.ttl", pkt.ttl, SEND_TTL - 1),
        ("ip.flags/frag", (pkt.ip_flags, pkt.frag), (IP_FLAG_DF, 0)),
        ("ip.checksum", "ok" if pkt.ip_checksum_ok else "bad", "ok"),
        ("udp.sport", pkt.sport, rule.new_udp_sport),
        ("udp.len", pkt.udp_len, 28 if is_ack else pkt.ip_len - 20),
        ("udp.checksum", hex(pkt.udp_checksum), hex(0)),
        ("bth.opcode", hex(pkt.opcode), hex(rule.opcode)),
        ("bth.(se, migreq, ackreq)", pkt.flags, FLAGS[rule.opcode]),
        ("bth.pad_count", pkt.pad, 0 if is_ack else pkt.pad),
        ("bth.version", pkt.tver, 0),
        ("bth.p_key", hex(pkt.p_key), hex(rule.p_key)),
        ("bth.resv8a", hex(pkt.resv8a), hex(0)),
        ("bth.dest_qp", pkt.qp, rule.new_dst_qp),
        ("bth.resv7", pkt.resv7, 0),
        ("icrc", "ok" if pkt.icrc_ok else "bad", "ok"),
    ])


# --------------------------------------------------------------------------
# I/O


class L2Port:
    """AF_PACKET socket on one interface that returns only inbound frames."""

    def __init__(self, iface: str):
        self.iface = iface
        self.sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
        self.sock.bind((iface, ETH_P_ALL))

    def fileno(self) -> int:
        return self.sock.fileno()

    def send(self, data) -> None:
        self.sock.send(bytes(data))

    def recv(self, timeout: float | None) -> bytes | None:
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            remaining = None if deadline is None else max(0.0, deadline - time.monotonic())
            if not select.select([self.sock], [], [], remaining)[0]:
                return None
            data, address = self.sock.recvfrom(65535)
            if address[0] == self.iface and address[2] != socket.PACKET_OUTGOING:
                return data

    def drain(self) -> None:
        while self.recv(0) is not None:
            pass


class Channel:
    """Newline-delimited JSON over the TCP management connection."""

    def __init__(self, sock: socket.socket):
        self.sock = sock
        self.buffer = b""

    def fileno(self) -> int:
        return self.sock.fileno()

    def send(self, message: dict) -> None:
        self.sock.sendall(json.dumps(message).encode() + b"\n")

    def receive_ready(self) -> list[dict]:
        """Read once from a readable socket; EOFError when the peer closed."""
        data = self.sock.recv(65536)
        if not data:
            raise EOFError
        self.buffer += data
        *lines, self.buffer = self.buffer.split(b"\n")
        return [json.loads(line) for line in lines if line.strip()]

    def request(self, message: dict, timeout: float = 5.0) -> dict:
        self.send(message)
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buffer:
            if not select.select([self.sock], [], [], max(0.0, deadline - time.monotonic()))[0]:
                raise TimeoutError(f"no reply to {message['type']} from the server")
            data = self.sock.recv(65536)
            if not data:
                raise EOFError("server closed the management connection")
            self.buffer += data
        line, self.buffer = self.buffer.split(b"\n", 1)
        reply = json.loads(line)
        if reply.get("type") == "error":
            raise RuntimeError(f"server: {reply['message']}")
        return reply

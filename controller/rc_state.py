"""Per-node RC connection state kept by the controller (hackthon.md architecture).

Every node opens its RC connection to a virtual endpoint on the switch, and the
controller is the far end of each one. A node's REQ for a virtual IP makes the
controller open connections to the nodes behind that IP; the initiator gets its
REP only after all of them have answered. Data is translated by the switch's
connections table. ACKs end here and are regenerated toward the sender once
every receiver has acknowledged.

Handshake: a simplified CM (REQ/REP/RTU) carried as RC SEND_ONLY to QP 1, the
only packet shape the P4 parser accepts besides ACK. Handshake packets and ACKs
reach the controller through rules that set the UDP source port to PUNT_SPORT:
that marks the ICRC dirty, so the switch sends them to the CPU port.
"""

import itertools
import json
import struct
import subprocess
import time
from dataclasses import dataclass, field

import icrc

OP_SEND_ONLY = 0x04
OP_ACK = 0x11
PSN_MOD = 1 << 24
PUNT_SPORT = 0

CM_QPN = 1
CM_MAGIC = b"RCM1"
CM_REQ, CM_REP, CM_RTU, CM_REJ = 1, 2, 3, 4
CM_NAMES = {CM_REQ: "REQ", CM_REP: "REP", CM_RTU: "RTU", CM_REJ: "REJ"}
_CM = struct.Struct("!4sB3xIIII")  # magic, type, local id, remote id, QPN, start PSN

TABLE = "MyIngress.connections"
ACTION = "MyIngress.translate_connection"


# --------------------------------------------------------------------------
# Wire formats


@dataclass(frozen=True)
class CmMsg:
    type: int
    local_id: int
    remote_id: int
    qpn: int
    psn: int

    @property
    def name(self):
        return CM_NAMES.get(self.type, str(self.type))


def encode_cm(type_, local_id, remote_id, qpn, psn):
    return _CM.pack(CM_MAGIC, type_, local_id, remote_id, qpn % PSN_MOD, psn % PSN_MOD)


def decode_cm(payload):
    if len(payload) < _CM.size or payload[:4] != CM_MAGIC:
        return None
    _, type_, local_id, remote_id, qpn, psn = _CM.unpack(payload[:_CM.size])
    return CmMsg(type_, local_id, remote_id, qpn, psn)


def _mac(text):
    return bytes.fromhex(text.replace(":", ""))


def _ip(text):
    return bytes(int(x) for x in text.split("."))


def _ip_str(b):
    return ".".join(str(x) for x in b)


def _ip_checksum(hdr):
    s = sum(struct.unpack("!10H", hdr))
    s = (s & 0xFFFF) + (s >> 16)
    s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


@dataclass
class View:
    """Fields of an Ethernet/IPv4(IHL 5)/UDP/BTH frame ending with the ICRC."""
    eth_dst: str
    eth_src: str
    ip_src: str
    ip_dst: str
    sport: int
    opcode: int
    pkey: int
    dqpn: int
    psn: int
    syndrome: int = None
    msn: int = None
    payload: bytes = b""


def view(frame):
    v = View(frame[0:6].hex(":"), frame[6:12].hex(":"), _ip_str(frame[26:30]),
             _ip_str(frame[30:34]), int.from_bytes(frame[34:36], "big"), frame[42],
             int.from_bytes(frame[44:46], "big"), int.from_bytes(frame[47:50], "big"),
             int.from_bytes(frame[51:54], "big"))
    if v.opcode == OP_ACK:
        v.syndrome, v.msn = frame[54], int.from_bytes(frame[55:58], "big")
    else:
        body = frame[54:-4]
        v.payload = body[:len(body) - ((frame[43] >> 4) & 3)]
    return v


def build_bth(opcode, dqpn, psn, pkey, se=0, migreq=1, ackreq=0, pad=0):
    return struct.pack("!BBHB3sB3s", opcode, se << 7 | migreq << 6 | pad << 4, pkey, 0,
                       dqpn.to_bytes(3, "big"), ackreq << 7, psn.to_bytes(3, "big"))


def build_frame(eth_src, eth_dst, ip_src, ip_dst, sport, bth, body, tos=0, ttl=64):
    udp_len = 8 + len(bth) + len(body) + 4
    ip = bytearray(struct.pack("!BBHHHBBH4s4s", 0x45, tos, 20 + udp_len, 0, 0x4000, ttl, 17, 0,
                               _ip(ip_src), _ip(ip_dst)))
    ip[10:12] = struct.pack("!H", _ip_checksum(ip))
    frame = (_mac(eth_dst) + _mac(eth_src) + b"\x08\x00" + bytes(ip)
             + struct.pack("!HHHH", sport, 4791, udp_len, 0) + bth + body + bytes(4))
    return icrc.rewrite_icrc(frame)


def rewrite_ack(template, eth_src, eth_dst, ip_src, ip_dst, sport, dqpn, psn, pkey,
                syndrome, msn):
    """Re-address an ACK; TOS, TTL, IP flags and BTH flags come from `template`."""
    f = bytearray(template)
    f[0:6], f[6:12] = _mac(eth_dst), _mac(eth_src)
    f[26:30], f[30:34] = _ip(ip_src), _ip(ip_dst)
    f[24:26] = b"\x00\x00"
    f[24:26] = struct.pack("!H", _ip_checksum(bytes(f[14:34])))
    f[34:36] = struct.pack("!H", sport)
    f[44:46] = struct.pack("!H", pkey)
    f[47:50] = dqpn.to_bytes(3, "big")
    f[51:54] = psn.to_bytes(3, "big")
    f[54] = syndrome
    f[55:58] = msn.to_bytes(3, "big")
    return icrc.rewrite_icrc(bytes(f))


def psn_le(a, b):
    """a <= b in 24-bit sequence space."""
    return (b - a) % PSN_MOD < PSN_MOD // 2


# --------------------------------------------------------------------------
# Switch rules


@dataclass(frozen=True)
class Rule:
    port: int
    src_ip: str
    dst_ip: str
    qp: int
    opcode: int
    new_src_mac: str
    new_dst_mac: str
    new_src_ip: str
    new_dst_ip: str
    sport: int
    new_qp: int
    pkey: int
    psn_delta: int
    msn_delta: int
    egress: int

    def table_add(self):
        mac = lambda m: "0x" + m.replace(":", "")  # noqa: E731
        return (f"table_add {TABLE} {ACTION} {self.port} {self.src_ip} {self.dst_ip} "
                f"{self.qp} {self.opcode:#04x} => {mac(self.new_src_mac)} "
                f"{mac(self.new_dst_mac)} {self.new_src_ip} {self.new_dst_ip} {self.sport} "
                f"{self.new_qp} {self.pkey} {self.psn_delta % PSN_MOD} "
                f"{self.msn_delta % PSN_MOD} {self.egress}")


class SwitchTable:
    """Installs connections-table entries through simple_switch_CLI (Thrift)."""

    def __init__(self, thrift_port=9090, cli="simple_switch_CLI"):
        self.thrift_port, self.cli = thrift_port, cli

    def add(self, rules):
        lines = [r.table_add() for r in rules]
        res = subprocess.run([self.cli, "--thrift-port", str(self.thrift_port)],
                             input="\n".join(lines) + "\n", capture_output=True, text=True,
                             timeout=60)
        added = res.stdout.count("Entry has been added")
        if added != len(lines):
            raise RuntimeError(f"installed {added}/{len(lines)} rules: "
                               f"{res.stdout[-400:]} {res.stderr[-200:]}")


class EventLog:
    def __init__(self, path=None):
        self.f = open(path, "a") if path else None

    def emit(self, ev, **fields):
        if self.f:
            self.f.write(json.dumps({"t": time.time(), "ev": ev, **fields}) + "\n")
            self.f.flush()


# --------------------------------------------------------------------------
# Connection state


@dataclass(frozen=True)
class Node:
    name: str
    ip: str
    mac: str
    port: int
    gateway_mac: str  # switch MAC on the node's port


@dataclass(eq=False)
class Conn:
    """One node's RC connection; the controller is the far end."""
    node: Node
    vip: str          # address the node talks to
    vqpn: int         # controller's QP on this connection
    sport: int        # UDP source port used toward the node
    local_id: int
    remote_id: int = 0
    node_qpn: int = None
    node_psn: int = None  # node's starting send PSN
    ctrl_psn: int = None  # controller's starting send PSN toward the node
    state: str = "INIT"
    relay: object = None


@dataclass(eq=False)
class Relay:
    """An initiator's connection and the connections opened on its behalf."""
    initiator: Conn
    targets: list = field(default_factory=list)
    waiting: set = field(default_factory=set)


@dataclass(eq=False)
class Direction:
    """Data from `src` to `dsts`; tracks which sends still lack an ACK."""
    src: Conn
    dsts: list
    delta: dict  # dst conn -> PSN offset from src to dst numbering
    pending: list = field(default_factory=list)  # [src PSN, dst conns not yet ACKed]
    msn: dict = field(default_factory=dict)


class RcState:
    def __init__(self, config, table, events=None, rules_out=None):
        self.table = table
        self.events = events or EventLog()
        self.rules_out = rules_out
        self.pkey = config.get("p_key", 0xFFFF)
        self.nodes = {n: Node(n, **d) for n, d in config["nodes"].items()}
        self.by_ip = {n.ip: n for n in self.nodes.values()}
        self.vips = {vip: [self.nodes[n] for n in names]
                     for vip, names in config["virtual_ips"].items()}
        self._vqpn = itertools.count(config.get("vqpn_base", 9001))
        self._sport = itertools.count(config.get("sport_base", 50001))
        self._id = itertools.count(1)
        self.by_id = {}        # local id -> conn
        self.by_vep = {}       # (node IP, vip, vqpn) -> conn
        self.by_node_qp = {}   # (node IP, node QPN) -> conn
        self.dir_by_dst = {}   # conn -> direction whose data it receives
        self.relays = []

    def vip_of(self, node):
        """The virtual IP that stands for `node` alone."""
        return next((v for v, ns in self.vips.items() if ns == [node]), None)

    def _punt(self, node, src_ip, dst_ip, qp, opcode):
        return Rule(node.port, src_ip, dst_ip, qp, opcode, node.gateway_mac, node.mac,
                    src_ip, dst_ip, PUNT_SPORT, qp, self.pkey, 0, 0, node.port)

    def _conn(self, node, vip, **kw):
        c = Conn(node, vip, next(self._vqpn), next(self._sport), next(self._id), **kw)
        self.by_id[c.local_id] = c
        return c

    def startup(self):
        """Let every node reach the controller with CM messages for its virtual peers."""
        rules = [self._punt(n, n.ip, vip, CM_QPN, OP_SEND_ONLY)
                 for n in self.nodes.values() for vip, ns in self.vips.items() if n not in ns]
        self.table.add(rules)
        self.events.emit("cm_listen", rules=len(rules))

    # Packets from the CPU port ---------------------------------------------

    def handle_cpu(self, frame):
        """(consumed, [(egress port, frame)]). Translated data is not consumed."""
        v = view(frame)
        if v.sport != PUNT_SPORT:
            return False, []
        if v.opcode == OP_SEND_ONLY and v.dqpn == CM_QPN:
            return True, self._on_cm(v)
        if v.opcode == OP_ACK:
            return True, self._on_ack(v, frame)
        self.events.emit("punt_unknown", src=v.ip_src, dst=v.ip_dst, opcode=v.opcode)
        return True, []

    def on_data(self, frame):
        """Record a translated Send on its way to a receiver."""
        v = view(frame)
        conn = self.by_node_qp.get((v.ip_dst, v.dqpn))
        d = self.dir_by_dst.get(conn)
        if v.opcode == OP_SEND_ONLY and d is not None:
            d.pending.append([(v.psn - d.delta[conn]) % PSN_MOD, set(d.dsts)])

    # Handshake ------------------------------------------------------------

    def _cm_out(self, conn, type_, remote_id):
        body = encode_cm(type_, conn.local_id, remote_id, conn.vqpn, conn.ctrl_psn or 0)
        frame = build_frame(conn.node.gateway_mac, conn.node.mac, conn.vip, conn.node.ip,
                            conn.sport, build_bth(OP_SEND_ONLY, CM_QPN, 0, self.pkey), body)
        self.events.emit("cm_tx", type=CM_NAMES[type_], node=conn.node.name, vip=conn.vip,
                         qpn=conn.vqpn, psn=conn.ctrl_psn)
        return conn.node.port, frame

    def _on_cm(self, v):
        msg, node = decode_cm(v.payload), self.by_ip.get(v.ip_src)
        if msg is None or node is None:
            self.events.emit("cm_invalid", src=v.ip_src)
            return []
        self.events.emit("cm_rx", type=msg.name, node=node.name, vip=v.ip_dst,
                         qpn=msg.qpn, psn=msg.psn)
        if msg.type == CM_REQ:
            return self._on_req(node, v.ip_dst, msg)
        conn = self.by_id.get(msg.remote_id)
        if conn is None or conn.node is not node:
            self.events.emit("cm_unexpected", type=msg.name, node=node.name)
            return []
        if msg.type == CM_REP and conn.state == "REQ_SENT":
            return self._on_rep(conn, msg)
        if msg.type == CM_RTU and conn.state == "REP_SENT":
            conn.state = "ESTABLISHED"
            self.events.emit("established", node=node.name, vqpn=conn.vqpn)
        return []

    def _on_req(self, node, vip, msg):
        targets, back = self.vips.get(vip), self.vip_of(node)
        if not targets or back is None or node in targets:
            self.events.emit("cm_reject", node=node.name, vip=vip)
            tmp = Conn(node, vip, 0, PUNT_SPORT + 1, 0)
            return [self._cm_out(tmp, CM_REJ, msg.local_id)]
        cx = self._conn(node, vip, remote_id=msg.local_id, node_qpn=msg.qpn,
                        node_psn=msg.psn, state="REQ_RCVD")
        relay = Relay(cx)
        cx.relay = relay
        out = []
        for t in targets:  # the receivers expect the initiator's PSNs unchanged
            ct = self._conn(t, back, ctrl_psn=msg.psn, state="REQ_SENT", relay=relay)
            relay.targets.append(ct)
            relay.waiting.add(ct)
            out.append(self._cm_out(ct, CM_REQ, 0))
        self.relays.append(relay)
        self.events.emit("relay_open", initiator=node.name, vip=vip,
                         targets=[t.name for t in targets], acked=0, need=len(targets))
        return out

    def _on_rep(self, ct, msg):
        ct.remote_id, ct.node_qpn, ct.node_psn = msg.local_id, msg.qpn, msg.psn
        ct.state = "REP_RCVD"
        relay = ct.relay
        relay.waiting.discard(ct)
        need = len(relay.targets)
        self.events.emit("relay_counter", node=ct.node.name, acked=need - len(relay.waiting),
                         need=need)
        return [] if relay.waiting else self._complete(relay)

    def _complete(self, relay):
        """All targets answered: install rules, finish the targets, then answer the initiator."""
        cx = relay.initiator
        cx.ctrl_psn = relay.targets[0].node_psn
        rules, effective = self._relay_rules(relay)
        self.events.emit("rules_install_start", n=len(rules))
        self.table.add(rules)
        self.events.emit("rules_install_done", n=len(rules))
        if self.rules_out and effective:
            with open(self.rules_out, "w") as f:
                f.write("\n".join(r.table_add() for r in effective) + "\n")

        out = []
        for ct in relay.targets:
            out.append(self._cm_out(ct, CM_RTU, ct.remote_id))
            ct.state = "ESTABLISHED"
            self.events.emit("established", node=ct.node.name, vqpn=ct.vqpn)
        out.append(self._cm_out(cx, CM_REP, cx.remote_id))
        cx.state = "REP_SENT"
        return out

    def _relay_rules(self, relay):
        """Switch rules for the relay, and the end-to-end effect including regenerated ACKs."""
        cx = relay.initiator
        x = cx.node
        rules = [self._punt(x, x.ip, cx.vip, cx.vqpn, OP_ACK)]
        fwd = Direction(cx, list(relay.targets),
                        {ct: (ct.ctrl_psn - cx.node_psn) % PSN_MOD for ct in relay.targets})
        self.by_vep[(x.ip, cx.vip, cx.vqpn)] = cx
        self.by_node_qp[(x.ip, cx.node_qpn)] = cx
        for ct in relay.targets:
            t = ct.node
            rules.append(self._punt(t, t.ip, ct.vip, ct.vqpn, OP_ACK))
            fwd.msn[ct] = 0
            self.dir_by_dst[ct] = fwd
            self.by_vep[(t.ip, ct.vip, ct.vqpn)] = ct
            self.by_node_qp[(t.ip, ct.node_qpn)] = ct

        if len(relay.targets) != 1:
            # TODO multicast: the controller has to replicate data to every target.
            self.events.emit("multicast_data_not_implemented", targets=len(relay.targets))
            return rules, []

        ct = relay.targets[0]
        t = ct.node
        d_fwd = fwd.delta[ct]
        d_rev = (cx.ctrl_psn - ct.node_psn) % PSN_MOD
        self.dir_by_dst[cx] = Direction(ct, [cx], {cx: d_rev}, msn={cx: 0})
        data_fwd = Rule(x.port, x.ip, cx.vip, cx.vqpn, OP_SEND_ONLY, t.gateway_mac, t.mac,
                        ct.vip, t.ip, ct.sport, ct.node_qpn, self.pkey, d_fwd, 0, t.port)
        data_rev = Rule(t.port, t.ip, ct.vip, ct.vqpn, OP_SEND_ONLY, x.gateway_mac, x.mac,
                        cx.vip, x.ip, cx.sport, cx.node_qpn, self.pkey, d_rev, 0, x.port)
        rules += [data_fwd, data_rev]
        effective = [
            data_fwd,
            Rule(x.port, x.ip, cx.vip, cx.vqpn, OP_ACK, t.gateway_mac, t.mac, ct.vip, t.ip,
                 ct.sport, ct.node_qpn, self.pkey, -d_rev, 0, t.port),
            data_rev,
            Rule(t.port, t.ip, ct.vip, ct.vqpn, OP_ACK, x.gateway_mac, x.mac, cx.vip, x.ip,
                 cx.sport, cx.node_qpn, self.pkey, -d_fwd, 0, x.port),
        ]
        return rules, effective

    # ACK aggregation ------------------------------------------------------

    def _ack_out(self, d, template, psn, msn, syndrome):
        s = d.src
        frame = rewrite_ack(template, s.node.gateway_mac, s.node.mac, s.vip, s.node.ip, s.sport,
                            s.node_qpn, psn, self.pkey, syndrome, msn)
        return s.node.port, frame

    def _on_ack(self, v, frame):
        conn = self.by_vep.get((v.ip_src, v.ip_dst, v.dqpn))
        d = self.dir_by_dst.get(conn)
        if d is None:
            self.events.emit("ack_unknown", src=v.ip_src, dst=v.ip_dst, qp=v.dqpn)
            return []
        psn = (v.psn - d.delta[conn]) % PSN_MOD
        if (v.syndrome >> 5) & 3:  # NAK / RNR NAK: the sender has to act now
            d.pending = [e for e in d.pending if not psn_le(psn, e[0])]
            self.events.emit("nak_forward", node=conn.node.name, to=d.src.node.name, psn=psn,
                             syndrome=v.syndrome)
            return [self._ack_out(d, frame, psn, v.msn, v.syndrome)]

        d.msn[conn] = v.msn
        for e in d.pending:
            if psn_le(e[0], psn):
                e[1].discard(conn)
        done = [e for e in d.pending if not e[1]]
        if not done:
            self.events.emit("ack_wait", node=conn.node.name, psn=psn)
            return []
        d.pending = [e for e in d.pending if e[1]]
        msn = min(d.msn[c] for c in d.dsts)
        self.events.emit("ack_aggregate", to=d.src.node.name, psn=done[-1][0], msn=msn,
                         acked=len(d.dsts))
        return [self._ack_out(d, frame, done[-1][0], msn, v.syndrome)]


def load_config(path):
    with open(path) as f:
        return json.load(f)

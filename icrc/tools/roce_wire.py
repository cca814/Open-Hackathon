#!/usr/bin/env python3
"""Generate and verify restricted RoCEv2 RC SEND frames on a raw interface.

This is an independent wire oracle for Node B's parser/rewrite/ICRC path. It
does not create an RDMA QP and therefore cannot demonstrate RC acceptance.
"""

import argparse
import hashlib
import ipaddress
import json
import socket
import struct
import time
import zlib


ETH_P_ALL = 0x0003
ETH_P_IP = 0x0800
ETH_P_IPV6 = 0x86DD
IPPROTO_UDP = 17
SOL_PACKET = 263
PACKET_ADD_MEMBERSHIP = 1
PACKET_MR_PROMISC = 1
PAYLOAD_PREFIX = b"ICRC-ROCE:"


def mac_bytes(value):
    parts = value.split(":")
    if len(parts) != 6 or any(len(part) != 2 for part in parts):
        raise ValueError(f"invalid MAC address: {value}")
    try:
        return bytes(int(part, 16) for part in parts)
    except ValueError as error:
        raise ValueError(f"invalid MAC address: {value}") from error


def checksum(data):
    if len(data) & 1:
        data += b"\0"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def packet_layout(frame):
    version = frame[14] >> 4
    if version == 4:
        return version, 14 + int.from_bytes(frame[16:18], "big"), 34, 42
    if version == 6:
        return version, 54 + int.from_bytes(frame[18:20], "big"), 54, 62
    raise ValueError("unsupported IP version")


def icrc(frame):
    version, end, udp_offset, bth_offset = packet_layout(frame)
    ip = bytearray(frame[14:udp_offset])
    udp = bytearray(frame[udp_offset:bth_offset])
    bth = bytearray(frame[bth_offset:bth_offset + 12])
    if version == 4:
        ip[1] = ip[8] = 0xFF
        ip[10:12] = b"\xff\xff"
    else:
        ip[0:4] = b"\x6f\xff\xff\xff"
        ip[7] = 0xFF
    udp[6:8] = b"\xff\xff"
    bth[4] = 0xFF
    canonical = b"\xff" * 8 + ip + udp + bth + frame[bth_offset + 12:end - 4]
    return struct.pack("<I", zlib.crc32(canonical))


def udp_checksum(frame):
    version, end, udp_offset, _ = packet_layout(frame)
    udp_length = end - udp_offset
    if version == 4:
        pseudo = frame[26:34] + b"\0\x11" + struct.pack("!H", udp_length)
    else:
        pseudo = (frame[22:54] + struct.pack("!I", udp_length) +
                  b"\0\0\0" + bytes([IPPROTO_UDP]))
    return checksum(pseudo + frame[udp_offset:end])


def build_frame(src_mac, dst_mac, src_ip, dst_ip, src_port, dst_qpn,
                tag, sequence, payload_size):
    encoded = tag.encode("ascii")
    marker = PAYLOAD_PREFIX + encoded + b":" + struct.pack("!I", sequence)
    if payload_size < len(marker):
        raise ValueError(f"payload-size must be at least {len(marker)}")
    payload = marker + bytes((sequence + i) & 0xFF for i in range(payload_size - len(marker)))
    padding = (-len(payload)) & 3
    bth = (bytes([4, padding << 4]) + b"\xff\xff\x80" +
           dst_qpn.to_bytes(3, "big") + b"\x80" + (sequence & 0xFFFFFF).to_bytes(3, "big"))
    transport = bth + payload + bytes(padding) + b"\0" * 4
    source = ipaddress.ip_address(src_ip)
    destination = ipaddress.ip_address(dst_ip)
    if source.version != destination.version:
        raise ValueError("source and destination IP families differ")
    udp_length = 8 + len(transport)
    if source.version == 4:
        ip_length = 20 + udp_length
        ip = bytearray(struct.pack("!BBHHHBBH4s4s", 0x45, 0x2A, ip_length,
                                   sequence & 0xFFFF, 0x4000, 64, IPPROTO_UDP, 0,
                                   source.packed, destination.packed))
        ip[10:12] = checksum(bytes(ip)).to_bytes(2, "big")
        ether_type = ETH_P_IP
    else:
        ip = bytearray(struct.pack("!IHBB16s16s", 0x62A00000, udp_length,
                                   IPPROTO_UDP, 64, source.packed, destination.packed))
        ether_type = ETH_P_IPV6
    udp = struct.pack("!HHHH", src_port, 4791, udp_length, 0)
    frame = bytearray(mac_bytes(dst_mac) + mac_bytes(src_mac) +
                      struct.pack("!H", ether_type) + ip + udp + transport)
    frame[-4:] = icrc(bytes(frame))
    value = udp_checksum(bytes(frame))
    udp_offset = 34 if source.version == 4 else 54
    frame[udp_offset + 6:udp_offset + 8] = (value or 0xFFFF).to_bytes(2, "big")
    return bytes(frame)


def rewrite_expected(frame, member):
    mac, address, qpn = member
    result = bytearray(frame)
    result[:6] = mac_bytes(mac)
    version, _, udp_offset, bth_offset = packet_layout(result)
    destination = ipaddress.ip_address(address)
    if destination.version != version:
        raise ValueError("member IP family differs from input")
    if version == 4:
        result[30:34] = destination.packed
        result[24:26] = b"\0\0"
        result[24:26] = checksum(bytes(result[14:34])).to_bytes(2, "big")
    else:
        result[38:54] = destination.packed
    result[bth_offset + 5:bth_offset + 8] = qpn.to_bytes(3, "big")
    result[-4:] = icrc(bytes(result))
    result[udp_offset + 6:udp_offset + 8] = b"\0\0"
    value = udp_checksum(bytes(result))
    result[udp_offset + 6:udp_offset + 8] = (value or 0xFFFF).to_bytes(2, "big")
    return bytes(result)


def parse_member(value):
    try:
        mac, address, qpn_text = value.split(",")
        mac_bytes(mac)
        address = str(ipaddress.ip_address(address))
        qpn = int(qpn_text, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError("member must be MAC,IP,QPN") from error
    if not 0 <= qpn <= 0xFFFFFF:
        raise argparse.ArgumentTypeError("member QPN must fit in 24 bits")
    return mac, address, qpn


def raw_socket(interface, promiscuous=False):
    sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
    sock.bind((interface, 0))
    if promiscuous:
        request = struct.pack("IHH8s", socket.if_nametoindex(interface),
                              PACKET_MR_PROMISC, 0, b"\0" * 8)
        sock.setsockopt(SOL_PACKET, PACKET_ADD_MEMBERSHIP, request)
    return sock


def input_frame(args, sequence):
    return build_frame(args.src_mac, args.dst_mac, args.src_ip, args.dst_ip,
                       args.src_port + sequence % args.flows, args.dst_qpn,
                       args.tag, sequence, args.payload_size)


def send(args):
    sock = raw_socket(args.interface)
    for sequence in range(args.count):
        frame = input_frame(args, sequence)
        if sock.send(frame) != len(frame):
            raise RuntimeError("short raw send")
        if sequence + 1 < args.count:
            time.sleep(args.interval)
    print(json.dumps({"event": "send_complete", "count": args.count}))


def capture(args):
    sock = raw_socket(args.interface, promiscuous=True)
    expected = {}
    for sequence in range(args.count):
        incoming = input_frame(args, sequence)
        for index, member in enumerate(args.member):
            frame = rewrite_expected(incoming, member)
            expected[hashlib.sha256(frame).digest()] = (index, sequence, frame)
    deadline = time.monotonic() + args.timeout
    observed = {}
    while len(observed) < len(expected):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        sock.settimeout(remaining)
        try:
            raw = sock.recv(65535)
        except TimeoutError:
            break
        if len(raw) < 54 or int.from_bytes(raw[12:14], "big") not in (ETH_P_IP, ETH_P_IPV6):
            continue
        try:
            version, end, _, _ = packet_layout(raw)
        except ValueError:
            continue
        frame = raw[:end]
        digest = hashlib.sha256(frame).digest()
        match = expected.get(digest)
        if match is None:
            continue
        index, sequence, exact = match
        if (frame != exact or (version == 4 and checksum(frame[14:34]) != 0) or
                udp_checksum(frame) != 0 or frame[-4:] != icrc(frame)):
            raise RuntimeError("matched frame failed independent checksum verification")
        observed[(index, sequence)] = digest
    print(json.dumps({"event": "capture_complete", "count": len(observed),
                      "expected": len(expected), "members": len(args.member)}))
    return 0 if len(observed) == len(expected) else 1


def add_packet_arguments(parser):
    parser.add_argument("--interface", required=True)
    parser.add_argument("--src-mac", required=True)
    parser.add_argument("--dst-mac", default="ff:ff:ff:ff:ff:ff")
    parser.add_argument("--src-ip", default="192.0.2.10")
    parser.add_argument("--dst-ip", default="192.0.2.100")
    parser.add_argument("--src-port", type=int, default=12345)
    parser.add_argument("--dst-qpn", type=int, default=9000)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--count", type=int, default=1)
    parser.add_argument("--flows", type=int, default=1)
    parser.add_argument("--payload-size", type=int, default=64)


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)
    sender = commands.add_parser("send")
    add_packet_arguments(sender)
    sender.add_argument("--interval", type=float, default=0.01)
    sender.set_defaults(handler=send)
    receiver = commands.add_parser("capture")
    add_packet_arguments(receiver)
    receiver.add_argument("--member", type=parse_member, action="append", required=True)
    receiver.add_argument("--timeout", type=float, default=15)
    receiver.set_defaults(handler=capture)
    return result


def main():
    args = parser().parse_args()
    marker_size = len(PAYLOAD_PREFIX) + len(args.tag.encode("ascii")) + 5
    if (args.count < 1 or args.flows < 1 or args.payload_size < marker_size or
            not 0 <= args.dst_qpn <= 0xFFFFFF or
            args.src_port < 1 or args.src_port + args.flows - 1 > 65535):
        raise SystemExit("invalid count, flows, payload size, source port, or QPN")
    try:
        source = ipaddress.ip_address(args.src_ip)
        destination = ipaddress.ip_address(args.dst_ip)
        if source.version != destination.version:
            raise ValueError("source and destination IP families differ")
        if hasattr(args, "member") and any(
                ipaddress.ip_address(member[1]).version != source.version
                for member in args.member):
            raise ValueError("member IP family differs from input")
        status = args.handler(args)
    except (OSError, UnicodeError, ValueError, RuntimeError) as error:
        raise SystemExit(str(error)) from error
    raise SystemExit(status or 0)


if __name__ == "__main__":
    main()

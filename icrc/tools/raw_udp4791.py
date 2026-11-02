#!/usr/bin/env python3
"""Send or capture a tagged raw Ethernet/IPv4/UDP probe.

This is a physical-path diagnostic, not a RoCE packet generator. It deliberately
uses UDP destination port 4791 so the Node B DOCA Flow rule handles it.
"""

import argparse
import hashlib
import json
import socket
import struct
import time


ETH_P_ALL = 0x0003
ETH_P_IP = 0x0800
IPPROTO_UDP = 17
PROBE_PREFIX = b"ICRC-PROBE:"


def mac_bytes(value):
    parts = value.split(":")
    if len(parts) != 6:
        raise ValueError(f"invalid MAC address: {value}")
    try:
        result = bytes(int(part, 16) for part in parts)
    except ValueError as error:
        raise ValueError(f"invalid MAC address: {value}") from error
    if any(len(part) != 2 for part in parts):
        raise ValueError(f"invalid MAC address: {value}")
    return result


def internet_checksum(data):
    if len(data) & 1:
        data += b"\0"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def probe_payload(tag, sequence):
    encoded = tag.encode("ascii")
    if not encoded or len(encoded) > 512:
        raise ValueError("tag must contain 1..512 ASCII bytes")
    return PROBE_PREFIX + encoded + b":" + struct.pack("!I", sequence)


def build_frame(src_mac, dst_mac, src_ip, dst_ip, src_port, dst_port,
                tag, sequence):
    payload = probe_payload(tag, sequence)
    udp = struct.pack("!HHHH", src_port, dst_port, 8 + len(payload), 0)
    total_length = 20 + len(udp) + len(payload)
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, total_length,
                     sequence & 0xFFFF, 0x4000, 64, IPPROTO_UDP, 0,
                     socket.inet_aton(src_ip), socket.inet_aton(dst_ip))
    checksum = internet_checksum(ip)
    ip = ip[:10] + struct.pack("!H", checksum) + ip[12:]
    return (mac_bytes(dst_mac) + mac_bytes(src_mac) +
            struct.pack("!H", ETH_P_IP) + ip + udp + payload)


def parse_probe(frame, tag, dst_port):
    if len(frame) < 14 + 20 + 8 or struct.unpack_from("!H", frame, 12)[0] != ETH_P_IP:
        return None
    ip_offset = 14
    version_ihl = frame[ip_offset]
    if version_ihl >> 4 != 4:
        return None
    ihl = (version_ihl & 0x0F) * 4
    if ihl < 20 or len(frame) < ip_offset + ihl + 8:
        return None
    total_length = struct.unpack_from("!H", frame, ip_offset + 2)[0]
    if (frame[ip_offset + 9] != IPPROTO_UDP or total_length < ihl + 8 or
            len(frame) < ip_offset + total_length):
        return None
    udp_offset = ip_offset + ihl
    observed_port, udp_length = struct.unpack_from("!2xHH", frame, udp_offset)
    if observed_port != dst_port or udp_length < 8:
        return None
    end = udp_offset + udp_length
    if end > ip_offset + total_length or end > len(frame):
        return None
    prefix = PROBE_PREFIX + tag.encode("ascii") + b":"
    payload = frame[udp_offset + 8:end]
    if len(payload) != len(prefix) + 4 or not payload.startswith(prefix):
        return None
    sequence = struct.unpack("!I", payload[-4:])[0]
    return {
        "sequence": sequence,
        "length": end,
        "sha256": hashlib.sha256(frame[:end]).hexdigest(),
    }


def send(args):
    sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
    sock.bind((args.interface, 0))
    records = []
    for sequence in range(args.count):
        src_port = args.src_port + sequence % args.flows
        frame = build_frame(args.src_mac, args.dst_mac, args.src_ip, args.dst_ip,
                            src_port, args.dst_port, args.tag, sequence)
        written = sock.send(frame)
        if written != len(frame):
            raise RuntimeError(f"short raw send: {written}/{len(frame)}")
        record = parse_probe(frame, args.tag, args.dst_port)
        records.append(record)
        if not args.quiet:
            print(json.dumps({"event": "sent", "src_port": src_port, **record}),
                  flush=True)
        if sequence + 1 < args.count:
            time.sleep(args.interval)
    print(json.dumps({"event": "send_complete", "count": len(records)}))


def capture(args):
    sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
    sock.bind((args.interface, 0))
    deadline = time.monotonic() + args.timeout
    records = {}
    while len(records) < args.expect:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        sock.settimeout(remaining)
        try:
            frame = sock.recv(65535)
        except TimeoutError:
            break
        record = parse_probe(frame, args.tag, args.dst_port)
        if record is not None and record["sequence"] not in records:
            records[record["sequence"]] = record
            if not args.quiet:
                print(json.dumps({"event": "captured", **record}), flush=True)
    print(json.dumps({"event": "capture_complete", "count": len(records),
                      "expected": args.expect}))
    return 0 if len(records) == args.expect else 1


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)

    sender = commands.add_parser("send")
    sender.add_argument("--interface", required=True)
    sender.add_argument("--src-mac", required=True)
    sender.add_argument("--dst-mac", default="ff:ff:ff:ff:ff:ff")
    sender.add_argument("--src-ip", default="192.0.2.1")
    sender.add_argument("--dst-ip", default="192.0.2.2")
    sender.add_argument("--src-port", type=int, default=49152)
    sender.add_argument("--dst-port", type=int, default=4791)
    sender.add_argument("--tag", required=True)
    sender.add_argument("--count", type=int, default=1)
    sender.add_argument("--flows", type=int, default=1,
                        help="number of UDP source ports to cycle across")
    sender.add_argument("--interval", type=float, default=0.1)
    sender.add_argument("--quiet", action="store_true",
                        help="print only the completion summary")
    sender.set_defaults(handler=send)

    receiver = commands.add_parser("capture")
    receiver.add_argument("--interface", required=True)
    receiver.add_argument("--dst-port", type=int, default=4791)
    receiver.add_argument("--tag", required=True)
    receiver.add_argument("--expect", type=int, default=1)
    receiver.add_argument("--timeout", type=float, default=10)
    receiver.add_argument("--quiet", action="store_true",
                          help="print only the completion summary")
    receiver.set_defaults(handler=capture)
    return result


def main():
    args = parser().parse_args()
    if getattr(args, "count", 1) < 1 or getattr(args, "expect", 1) < 1:
        raise SystemExit("count/expect must be positive")
    if (getattr(args, "flows", 1) < 1 or
            getattr(args, "src_port", 0) + getattr(args, "flows", 1) - 1 > 65535):
        raise SystemExit("flows must be positive and keep source ports <= 65535")
    try:
        status = args.handler(args)
    except (OSError, ValueError, RuntimeError) as error:
        raise SystemExit(str(error)) from error
    raise SystemExit(status or 0)


if __name__ == "__main__":
    main()

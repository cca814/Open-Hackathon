#!/usr/bin/env python3
"""Inject a bounded IPv4 SEND/two-ACK sequence for Milestone 4 metrics.

This is a wire-level hardware probe, not an RC QP acceptance test.  It sends a
sender-domain SEND followed by one positive ACK from each receiver domain so
Node B exercises PSN translation, ACK aggregation, ICRC timing, and RTT timing.
"""

import argparse
import hashlib
import ipaddress
import json
import struct
import time

from roce_wire import (ETH_P_IP, IPPROTO_UDP, build_frame, checksum, icrc,
                       mac_bytes, packet_layout, raw_socket, udp_checksum)


ROCE_PORT = 4791
RC_ACK_OPCODE = 0x11


def build_ack(src_mac, dst_mac, src_ip, dst_ip, dst_qpn, psn, ip_id=0):
    """Build one IPv4 RoCEv2 positive ACK with AETH syndrome zero."""
    source = ipaddress.ip_address(src_ip)
    destination = ipaddress.ip_address(dst_ip)
    if source.version != 4 or destination.version != 4:
        raise ValueError("the metric probe accepts IPv4 addresses only")
    if not 0 <= dst_qpn <= 0xFFFFFF or not 0 <= psn <= 0xFFFFFF:
        raise ValueError("QPN and PSN must fit in 24 bits")

    bth = (bytes([RC_ACK_OPCODE, 0]) + b"\xff\xff\x00" +
           dst_qpn.to_bytes(3, "big") + b"\x00" + psn.to_bytes(3, "big"))
    aeth = b"\x00\x00\x00\x00"
    transport = bth + aeth + b"\x00" * 4
    udp_length = 8 + len(transport)
    ip_length = 20 + udp_length
    ip = bytearray(struct.pack("!BBHHHBBH4s4s", 0x45, 0x2A, ip_length,
                               ip_id & 0xFFFF, 0x4000, 64, IPPROTO_UDP, 0,
                               source.packed, destination.packed))
    ip[10:12] = checksum(bytes(ip)).to_bytes(2, "big")
    udp = struct.pack("!HHHH", ROCE_PORT, ROCE_PORT, udp_length, 0)
    frame = bytearray(mac_bytes(dst_mac) + mac_bytes(src_mac) +
                      struct.pack("!H", ETH_P_IP) + ip + udp + transport)
    frame[-4:] = icrc(bytes(frame))
    value = udp_checksum(bytes(frame))
    frame[40:42] = (value or 0xFFFF).to_bytes(2, "big")
    return bytes(frame)


def validate_frame(frame, expected_opcode, expected_psn):
    version, end, _, bth_offset = packet_layout(frame)
    if version != 4 or end != len(frame):
        raise RuntimeError("probe generated an invalid IPv4 frame length")
    if checksum(frame[14:34]) != 0 or udp_checksum(frame) != 0:
        raise RuntimeError("probe generated an invalid IPv4/UDP checksum")
    if frame[-4:] != icrc(frame):
        raise RuntimeError("probe generated an invalid RoCE ICRC")
    if frame[bth_offset] != expected_opcode:
        raise RuntimeError("probe generated an unexpected RoCE opcode")
    if int.from_bytes(frame[bth_offset + 9:bth_offset + 12], "big") != expected_psn:
        raise RuntimeError("probe generated an unexpected PSN")


def frames(args, index):
    sender_psn = (args.sender_initial_psn + index) & 0xFFFFFF
    member0_psn = (args.member0_initial_psn + index) & 0xFFFFFF
    member1_psn = (args.member1_initial_psn + index) & 0xFFFFFF
    send_frame = build_frame(
        args.sender_mac, args.virtual_mac, args.sender_ip, args.virtual_ip,
        args.source_port, args.virtual_qpn, args.tag, sender_psn,
        args.payload_size)
    ack0 = build_ack(
        args.member0_mac, args.sender_mac, args.member0_ip, args.sender_ip,
        args.sender_qpn, member0_psn, sender_psn)
    ack1 = build_ack(
        args.member1_mac, args.sender_mac, args.member1_ip, args.sender_ip,
        args.sender_qpn, member1_psn, sender_psn)
    validate_frame(send_frame, 0x04, sender_psn)
    validate_frame(ack0, RC_ACK_OPCODE, member0_psn)
    validate_frame(ack1, RC_ACK_OPCODE, member1_psn)
    return send_frame, ack0, ack1


def run(args):
    first = frames(args, 0)
    last = frames(args, args.count - 1)
    if args.dry_run:
        print(json.dumps({
            "event": "dry_run_complete",
            "family": "IPv4",
            "count": args.count,
            "first_sha256": [hashlib.sha256(frame).hexdigest() for frame in first],
            "last_sha256": [hashlib.sha256(frame).hexdigest() for frame in last],
        }))
        return

    sender = raw_socket(args.sender_interface)
    member0 = raw_socket(args.member0_interface)
    member1 = raw_socket(args.member1_interface)
    try:
        for index in range(args.count):
            send_frame, ack0, ack1 = frames(args, index)
            sender.sendall(send_frame)
            time.sleep(args.send_to_ack_ms / 1000.0)
            member0.sendall(ack0)
            time.sleep(args.ack_gap_ms / 1000.0)
            member1.sendall(ack1)
            if index + 1 < args.count:
                time.sleep(args.interval_ms / 1000.0)
    finally:
        sender.close()
        member0.close()
        member1.close()
    print(json.dumps({
        "event": "probe_complete",
        "family": "IPv4",
        "count": args.count,
        "expected_aggregate_acks": args.count,
        "injected_rtt_floor_ms": args.send_to_ack_ms + args.ack_gap_ms,
    }))


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--sender-interface", default="enp3s0f0s0")
    result.add_argument("--member0-interface", default="enp3s0f1s0")
    result.add_argument("--member1-interface", default="enp3s0f1s1")
    result.add_argument("--sender-mac", default="02:96:42:f8:5e:f2")
    result.add_argument("--virtual-mac", default="02:00:00:00:01:00")
    result.add_argument("--member0-mac", default="02:7e:6d:fd:79:7c")
    result.add_argument("--member1-mac", default="02:7e:6d:fd:79:01")
    result.add_argument("--sender-ip", default="192.0.2.10")
    result.add_argument("--virtual-ip", default="192.0.2.100")
    result.add_argument("--member0-ip", default="192.0.2.1")
    result.add_argument("--member1-ip", default="192.0.2.2")
    result.add_argument("--sender-qpn", type=int, default=100)
    result.add_argument("--virtual-qpn", type=int, default=9000)
    result.add_argument("--sender-initial-psn", type=int, default=100000)
    result.add_argument("--member0-initial-psn", type=int, default=200000)
    result.add_argument("--member1-initial-psn", type=int, default=300000)
    result.add_argument("--source-port", type=int, default=12345)
    result.add_argument("--payload-size", type=int, default=64)
    result.add_argument("--tag", default="m4-ipv4-metrics")
    result.add_argument("--count", type=int, default=32)
    result.add_argument("--send-to-ack-ms", type=float, default=2.0)
    result.add_argument("--ack-gap-ms", type=float, default=1.0)
    result.add_argument("--interval-ms", type=float, default=1.0)
    result.add_argument("--dry-run", action="store_true")
    return result


def main():
    args = parser().parse_args()
    values = (args.sender_qpn, args.virtual_qpn, args.sender_initial_psn,
              args.member0_initial_psn, args.member1_initial_psn)
    if args.count < 1 or any(not 0 <= value <= 0xFFFFFF for value in values):
        raise SystemExit("count must be positive; every QPN/PSN must fit in 24 bits")
    if min(args.send_to_ack_ms, args.ack_gap_ms, args.interval_ms) < 0:
        raise SystemExit("probe delays must be non-negative")
    try:
        run(args)
    except (OSError, ValueError, RuntimeError) as error:
        raise SystemExit(str(error)) from error


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Check ordinary traffic through BMv2 port 1 <-> 2, without a server process.

Run after loading controller/connections.example.txt and before server.py.
Both host-facing veth interfaces must be accessible on this Linux machine.
"""

import argparse
import socket
import sys

from scapy.all import ARP, ICMP, IP, IPv6, TCP, UDP, Ether, IPOption_NOP, Raw

from roce import L2Port


def cases(src_mac, dst_mac, src_ip, dst_ip):
    eth = Ether(src=src_mac, dst=dst_mac)
    ip = IP(src=src_ip, dst=dst_ip)
    return [
        ("ARP request", Ether(src=src_mac, dst="ff:ff:ff:ff:ff:ff") /
         ARP(hwsrc=src_mac, psrc=src_ip, pdst=dst_ip)),
        ("ARP reply", eth / ARP(op=2, hwsrc=src_mac, hwdst=dst_mac,
                               psrc=src_ip, pdst=dst_ip)),
        ("TCP SYN port 10001", eth / ip / TCP(sport=40000, dport=10001, flags="S",
                                             options=[("MSS", 1460), ("WScale", 7)])),
        ("TCP SYN-ACK", eth / ip / TCP(sport=10001, dport=40000, flags="SA", ack=1)),
        ("TCP 20-byte payload", eth / ip / TCP(sport=40000, dport=10001, flags="PA") /
         Raw(bytes(range(20)))),
        ("TCP with IP options", eth / IP(src=src_ip, dst=dst_ip,
                                         options=[IPOption_NOP()] * 4) /
         TCP(sport=40000, dport=10001) / Raw(b"options")),
        ("ICMP, TTL unchanged", eth / IP(src=src_ip, dst=dst_ip, ttl=1) /
         ICMP() / Raw(b"bridge ping")),
        ("ordinary UDP", eth / ip / UDP(sport=40000, dport=4792) / Raw(b"ordinary UDP")),
        ("IPv6 TCP", eth / IPv6(src="2001:db8::1", dst="2001:db8::2") /
         TCP(sport=40000, dport=10001) / Raw(b"IPv6")),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client-iface", default="node-a")
    parser.add_argument("--server-iface", default="node-b")
    parser.add_argument("--cpu-iface", default="ctl-cpu")
    parser.add_argument("--timeout", type=float, default=1.0)
    args = parser.parse_args()
    if not hasattr(socket, "AF_PACKET"):
        parser.exit(1, "this test requires Linux AF_PACKET sockets\n")
    ports = []
    try:
        ports = [L2Port(args.client_iface), L2Port(args.server_iface), L2Port(args.cpu_iface)]
        failures = passed = 0
        for source, target, label, src_mac, dst_mac, src_ip, dst_ip in (
            (ports[0], ports[1], "1 -> 2", "02:00:00:00:00:01", "02:00:00:00:00:02", "10.0.0.1", "10.0.0.2"),
            (ports[1], ports[0], "2 -> 1", "02:00:00:00:00:02", "02:00:00:00:00:01", "10.0.0.2", "10.0.0.1"),
        ):
            for name, packet in cases(src_mac, dst_mac, src_ip, dst_ip):
                for port in ports:
                    port.drain()
                expected = bytes(packet)
                source.send(expected)
                received = target.recv(args.timeout)
                # Some interfaces pad short Ethernet frames to 60 bytes.
                unchanged = received in (expected, expected.ljust(60, b"\0"))
                cpu = ports[2].recv(0.05)
                reflected = source.recv(0)
                duplicate = target.recv(0)
                if unchanged and cpu is None and reflected is None and duplicate is None:
                    passed += 1
                    print(f"[PASS] {label}: {name}")
                else:
                    failures += 1
                    print(f"[FAIL] {label}: {name}: unchanged={unchanged}, "
                          f"CPU={cpu is not None}, reflected={reflected is not None}, duplicate={duplicate is not None}")
        print(f"\n{passed} passed, {failures} failed")
        return int(failures != 0)
    except OSError as error:
        parser.exit(1, f"cannot open test interfaces (requires root and veth.sh up): {error}\n")
    finally:
        for port in ports:
            port.sock.close()


if __name__ == "__main__":
    sys.exit(main())

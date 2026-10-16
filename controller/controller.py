#!/usr/bin/env python3
"""P4 switch ICRC controller.

Packets arrive from the switch CPU port prefixed with a 2-byte packet_in header
(egress port). Strip it, recompute the ICRC, and send the packet back with the
same 2 bytes as packet_out.

With --rc-config the controller also keeps every node's RC connection state
(rc_state.py): it handles the in-band handshake, installs the connection rules
and aggregates ACKs.

usage:
    sudo python3 controller.py -i <iface> [--rc-config rc_config.json] [-v]
"""

import argparse
import errno
import os
import socket
import struct
import sys

import icrc
import rc_state

ETH_P_ALL = 0x0003
PACKET_OUTGOING = 4
CPU_HDR_LEN = 2  # packet_in / packet_out: bit<16> egress_port


def autodetect_iface():
    """First non-loopback interface, preferring names containing "eth"."""
    candidates = [n for n in sorted(os.listdir("/sys/class/net")) if n != "lo"]
    if not candidates:
        raise RuntimeError("no non-loopback interface found")
    return next((n for n in candidates if "eth" in n), candidates[0])


def open_raw_socket(iface):
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
    s.bind((iface, 0))
    return s


def run(iface, byte_order=icrc.ICRC_LITTLE, cpu_header=True, verbose=False, rc=None):
    sock = open_raw_socket(iface)
    print(f"[controller] listening on {iface}, icrc byte-order={byte_order}, "
          f"cpu_header={cpu_header}, rc_state={'on' if rc else 'off'}", flush=True)

    n_seen = n_fixed = 0
    while True:
        try:
            frame, addr = sock.recvfrom(65535)
        except OSError as e:
            if e.errno in (errno.ENETDOWN, errno.ENODEV, errno.ENXIO):
                print(f"[controller] iface {iface} down, stopping "
                      f"[seen={n_seen} fixed={n_fixed}]", flush=True)
                return
            raise
        if addr[2] == PACKET_OUTGOING:  # our own transmissions
            continue

        n_seen += 1
        if cpu_header:
            if len(frame) < CPU_HDR_LEN:
                continue
            cpu_hdr, roce = frame[:CPU_HDR_LEN], frame[CPU_HDR_LEN:]
        else:
            cpu_hdr, roce = b"", frame

        if not icrc.is_rocev2(roce):
            if verbose:
                print(f"[controller] skip non-RoCEv2 frame len={len(frame)}", flush=True)
            continue

        if rc is not None:
            consumed, out = rc.handle_cpu(roce)
            if consumed:
                for port, frame_out in out:
                    sock.send(struct.pack("!H", port) + frame_out)
                if verbose:
                    v = rc_state.view(roce)
                    print(f"[controller] rc consumed opcode={v.opcode:#04x} from {v.ip_src} "
                          f"-> sent {len(out)} frame(s) to ports {[p for p, _ in out]}",
                          flush=True)
                continue

        fixed = icrc.rewrite_icrc(roce, byte_order=byte_order)
        if rc is not None:
            rc.on_data(fixed)
        sock.send(cpu_hdr + fixed)
        n_fixed += 1

        if verbose:
            egress = struct.unpack("!H", cpu_hdr)[0] if cpu_hdr else None
            old, new = roce[-4:].hex(), fixed[-4:].hex()
            changed = "changed" if old != new else "same"
            print(f"[controller] roce len={len(roce)} egress_port={egress} "
                  f"icrc {old} -> {new} ({changed}) [seen={n_seen} fixed={n_fixed}]",
                  flush=True)


def main():
    ap = argparse.ArgumentParser(description="RoCEv2 ICRC recompute controller for the P4 switch")
    ap.add_argument("-i", "--iface", default=None,
                    help="interface wired to the switch CPU port (default: autodetect)")
    ap.add_argument("--byte-order", choices=[icrc.ICRC_LITTLE, icrc.ICRC_BIG],
                    default=icrc.ICRC_LITTLE, help="ICRC byte order (default: little)")
    ap.add_argument("--no-cpu-header", dest="cpu_header", action="store_false",
                    help="frames carry no packet_in/packet_out header")
    ap.add_argument("-v", "--verbose", action="store_true", help="log every packet")
    ap.add_argument("--rc-config", help="keep RC connection state (see rc_state.py)")
    ap.add_argument("--thrift-port", type=int, default=9090, help="switch Thrift port")
    ap.add_argument("--switch-cli", default="simple_switch_CLI", help="BMv2 CLI binary")
    ap.add_argument("--event-log", help="append RC state events here (JSON lines)")
    ap.add_argument("--rules-out", help="write the end-to-end translation of each connection")
    args = ap.parse_args()

    iface = args.iface or autodetect_iface()
    rc = None
    if args.rc_config:
        if not args.cpu_header:
            ap.error("--rc-config needs the CPU header")
        rc = rc_state.RcState(rc_state.load_config(args.rc_config),
                              rc_state.SwitchTable(args.thrift_port, args.switch_cli),
                              rc_state.EventLog(args.event_log), args.rules_out)
        rc.startup()
    try:
        run(iface, byte_order=args.byte_order, cpu_header=args.cpu_header,
            verbose=args.verbose, rc=rc)
    except PermissionError:
        print("raw sockets need root; run with sudo", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\n[controller] stopped", flush=True)


if __name__ == "__main__":
    main()

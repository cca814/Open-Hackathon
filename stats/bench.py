#!/usr/bin/env python3
"""Load generator on node A (client side of every connection).

latency     One message at a time, the rdma/ echo exchange: Send -> ACK + echo
            Send -> ACK. Records when the ACK and the echo came back.
throughput  Open loop: Sends at a fixed rate, round-robin over the connections,
            regardless of ACKs. A child process collects the returning ACKs.

Both need responder.py on node B. Timestamps are CLOCK_REALTIME ns: the
sender's time.time_ns() right before send(), the kernel receive time on the
receiving side, so client and responder logs on one host can be joined.

Output, for --out PREFIX:
  PREFIX.json        parameters, --meta labels, responder summary
  PREFIX.client.csv  one row per message sent
  PREFIX.server.csv  one row per Send the responder received
  PREFIX.acks.csv    throughput only: one row per ACK received
"""

from __future__ import annotations

import argparse
import json
import multiprocessing
import socket
import sys
import time
from array import array

from common import (CONTROL_PORT, OP_ACK, OP_SEND_ONLY, PAYLOAD_OFF, PSN_MOD, Frames, RawPort,
                    load_connections, peek, roce, write_csv)


def tag(i: int, size: int) -> bytes:
    """Payload that identifies message `i`, so a late echo is not taken for the current one."""
    data = i.to_bytes(8, "big")
    return data[-size:] if size < 8 else data + bytes(size - 8)


def prebuilt(connections, size: int) -> list[Frames]:
    """Templates for every connection, so no message pays for a scapy build (~ms)."""
    frames = [Frames(t.client) for t in connections]
    for f in frames:
        f.send_only(0, size)
    return frames


def run_latency(args, connections, port: RawPort, control: roce.Channel) -> dict:
    frames = prebuilt(connections, args.size)
    by_qp = {t.client.qp: i for i, t in enumerate(connections)}
    next_psn = [args.psn] * len(connections)
    echoes = [0] * len(connections)
    rows, lost_ack, lost_echo = [], 0, 0
    port.drain()
    control.request({"type": "start", "echo": not args.no_echo, "ack_every": 1, "size": args.size,
                     "out": f"{args.out}.server.csv"}, timeout=120)
    for i in range(args.warmup + args.count):
        c = i % len(connections)
        psn, next_psn[c] = next_psn[c], (next_psn[c] + 1) % PSN_MOD
        payload = tag(i, args.size)
        frame = frames[c].send_only(psn, args.size, payload)
        ack_ns = echo_ns = 0
        tx_ns = time.time_ns()
        port.send(frame)
        deadline = time.monotonic() + args.timeout
        while not ack_ns or not (echo_ns or args.no_echo):
            got = port.recv(max(0.0, deadline - time.monotonic()))
            if got is None:
                break
            data, rx_ns = got
            info = peek(data)
            if info is None or by_qp.get(info[1]) != c:
                continue
            opcode, _, rx_psn, _, size = info
            if opcode == OP_ACK and rx_psn == psn:
                ack_ns = rx_ns
            elif opcode == OP_SEND_ONLY and data[PAYLOAD_OFF:PAYLOAD_OFF + size] == payload:
                echo_ns = rx_ns
                echoes[c] += 1
                port.send(frames[c].ack(rx_psn, echoes[c]))
        if i >= args.warmup:
            rows.append((i - args.warmup, c, psn, tx_ns, ack_ns, echo_ns))
            lost_ack += not ack_ns
            lost_echo += not (echo_ns or args.no_echo)
        if args.gap:
            time.sleep(args.gap)
    time.sleep(0.05)  # last ACK of an echo
    summary = control.request({"type": "stop"}, timeout=60)
    write_csv(f"{args.out}.client.csv", ["seq", "conn", "psn", "tx_ns", "ack_ns", "echo_ns"], rows)
    return {"sent": len(rows), "lost_ack": lost_ack, "lost_echo": lost_echo, "responder": summary}


def collect_acks(iface, by_qp, out, ready, stop) -> None:
    port = RawPort(iface)
    port.drain()
    rows = []
    ready.set()
    while True:
        stopping = stop.is_set()
        got = port.recv(0.05)
        while got is not None:
            data, rx_ns = got
            info = peek(data)
            if info and info[0] == OP_ACK and info[1] in by_qp:
                rows.append((by_qp[info[1]], info[2], info[3], rx_ns))
            got = port.recv_nowait()
        if stopping:
            break
    write_csv(out, ["conn", "psn", "syndrome", "rx_ns"], rows)


def run_throughput(args, connections, port: RawPort, control: roce.Channel) -> dict:
    frames = prebuilt(connections, args.size)
    by_qp = {t.client.qp: i for i, t in enumerate(connections)}
    count = max(1, int(args.rate * args.duration))
    k = len(connections)

    context = multiprocessing.get_context("fork")
    ready, stop = context.Event(), context.Event()
    receiver = context.Process(target=collect_acks,
                               args=(args.iface, by_qp, f"{args.out}.acks.csv", ready, stop))
    receiver.start()
    if not ready.wait(10):
        receiver.kill()
        raise RuntimeError("ACK collector did not start")
    control.request({"type": "start", "echo": False, "ack_every": args.ack_every,
                     "out": f"{args.out}.server.csv"})

    tx = array("q", bytes(8 * count))
    period = 1e9 / args.rate
    send, size, base = port.sock.send, args.size, args.psn
    start = time.perf_counter_ns() + 1_000_000
    for i in range(count):
        target = start + int(i * period)
        if target - time.perf_counter_ns() > 2_000_000:
            time.sleep((target - time.perf_counter_ns() - 1_000_000) / 1e9)
        while time.perf_counter_ns() < target:
            pass
        frame = frames[i % k].send_only((base + i // k) % PSN_MOD, size)
        tx[i] = time.time_ns()
        send(frame)
    elapsed = (tx[-1] - tx[0]) / 1e9 if count > 1 else 0.0

    time.sleep(args.linger)
    summary = control.request({"type": "stop"}, timeout=120)
    stop.set()
    receiver.join(30)
    write_csv(f"{args.out}.client.csv", ["seq", "conn", "psn", "tx_ns"],
              ((i, i % k, (base + i // k) % PSN_MOD, tx[i]) for i in range(count)))
    return {"sent": count, "send_seconds": elapsed,
            "achieved_pps": (count - 1) / elapsed if elapsed else 0.0, "responder": summary}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mode", choices=["latency", "throughput"])
    parser.add_argument("--rules", required=True, help="rules file installed on the switch")
    parser.add_argument("--out", required=True, help="output path prefix")
    parser.add_argument("--meta", default="{}", help="JSON labels stored with the results")
    parser.add_argument("--iface", default="node-a")
    parser.add_argument("--control-port", type=int, default=CONTROL_PORT)
    parser.add_argument("--size", type=int, default=64, help="Send payload bytes, 1..256")
    parser.add_argument("--psn", type=roce.psn_arg, default=1000, help="first PSN per connection")
    parser.add_argument("--connections", type=int,
                        help="use only the first N connections of the rules file")
    latency = parser.add_argument_group("latency")
    latency.add_argument("--count", type=int, default=1000)
    latency.add_argument("--warmup", type=int, default=50)
    latency.add_argument("--gap", type=float, default=0.0005, help="seconds between messages")
    latency.add_argument("--timeout", type=float, default=1.0)
    latency.add_argument("--no-echo", action="store_true", help="only wait for the ACK")
    throughput = parser.add_argument_group("throughput")
    throughput.add_argument("--rate", type=float, default=1000, help="Sends per second")
    throughput.add_argument("--duration", type=float, default=5.0, help="seconds of sending")
    throughput.add_argument("--ack-every", type=int, default=1, help="responder ACKs every Nth Send")
    throughput.add_argument("--linger", type=float, default=1.0,
                            help="seconds to wait for stragglers after the last Send")
    args = parser.parse_args(argv)
    if not 1 <= args.size <= 256:
        parser.error("--size must be 1..256 (one packet per message at path MTU 256)")

    try:
        connections = load_connections(args.rules)[:args.connections]
        port = RawPort(args.iface)
        control = roce.Channel(socket.create_connection(("127.0.0.1", args.control_port), timeout=5))
    except roce.TopologyError as error:
        sys.exit(f"{args.rules}: {error}")
    except PermissionError:
        sys.exit("raw sockets need root or CAP_NET_RAW")
    except OSError as error:
        sys.exit(f"cannot start: {error} (is responder.py running?)")

    started = time.time()
    run = run_latency if args.mode == "latency" else run_throughput
    result = run(args, connections, port, control)
    record = {"mode": args.mode, "meta": json.loads(args.meta), "started": started,
              "params": {k: v for k, v in vars(args).items() if k not in ("meta", "out")},
              "connections": len(connections), **result}
    with open(f"{args.out}.json", "w") as f:
        json.dump(record, f, indent=2)
    r = result["responder"]
    print(f"{args.mode}: sent {result['sent']}, responder received {r.get('received')} "
          f"(in order {r.get('in_order')}, missing {r.get('missing')}, reordered "
          f"{r.get('reordered')})" + (f", {result['achieved_pps']:.0f} pps offered"
                                      if "achieved_pps" in result else ""), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())

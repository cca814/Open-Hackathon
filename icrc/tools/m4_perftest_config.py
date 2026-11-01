#!/usr/bin/env python3
"""Generate a one-receiver reliable IPv4 group from gated perftest QPs."""

import argparse
import csv
import glob
import json
from pathlib import Path


def read_metadata(directory):
    result = {}
    for name in glob.glob(str(Path(directory) / "*.meta")):
        with open(name, newline="", encoding="ascii") as stream:
            rows = list(csv.reader(stream))
        if len(rows) != 1 or len(rows[0]) != 5:
            raise ValueError(f"invalid QP metadata: {name}")
        device, qp_num, dest_qpn, rq_psn, sq_psn = rows[0]
        if device in result:
            raise ValueError(f"duplicate QP metadata for {device}")
        values = tuple(int(value) for value in (qp_num, dest_qpn, rq_psn, sq_psn))
        if any(not 0 <= value <= 0xFFFFFF for value in values):
            raise ValueError(f"out-of-range QP metadata: {name}")
        result[device] = values
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory")
    parser.add_argument("output")
    parser.add_argument("--sender-device", default="mlx5_2")
    parser.add_argument("--receiver-device", default="mlx5_3")
    parser.add_argument("--best-effort", action="store_true",
                        help="generate the valid single-receiver Stage A group")
    args = parser.parse_args()
    if not args.best_effort:
        raise SystemExit("stock perftest has one receiver; use --best-effort for Stage A")
    metadata = read_metadata(args.directory)
    if args.sender_device not in metadata or not set(metadata).issubset(
            {args.sender_device, args.receiver_device}):
        raise SystemExit(f"expected sender {args.sender_device} and optional receiver "
                         f"{args.receiver_device}; found {sorted(metadata)}")
    sender_qpn, sender_dest_qpn, _, sender_sq_psn = metadata[args.sender_device]
    receiver_qpn = sender_dest_qpn
    receiver_rq_psn = sender_sq_psn
    reciprocal_verified = args.receiver_device in metadata
    if reciprocal_verified:
        receiver_qpn, receiver_dest_qpn, receiver_rq_psn, _ = metadata[args.receiver_device]
        if sender_dest_qpn != receiver_qpn or receiver_dest_qpn != sender_qpn:
            raise SystemExit("perftest QP destinations are not reciprocal")
        if receiver_rq_psn != sender_sq_psn:
            raise SystemExit("receiver RQ PSN does not match sender SQ PSN")
    group = {
        "id": "ib-send-bw-ipv4-stage-a",
        "match": {"dst_ipv4": "10.0.2.1", "dst_qpn": receiver_qpn},
        "members": [{"dst_mac": "02:7e:6d:fd:79:7c",
                     "dst_ipv4": "10.0.2.1", "dst_qpn": receiver_qpn}],
    }
    config = {"groups": [group]}
    with open(args.output, "w", encoding="ascii") as stream:
        json.dump(config, stream, indent=2)
        stream.write("\n")
    print(json.dumps({"event": "config_generated", "sender_qpn": sender_qpn,
                      "receiver_qpn": receiver_qpn, "initial_psn": sender_sq_psn,
                      "reciprocal_verified": reciprocal_verified,
                      "mode": "best-effort",
                      "output": args.output}))


if __name__ == "__main__":
    main()

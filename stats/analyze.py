#!/usr/bin/env python3
"""Summarize a stats/suite.py result directory into RESULT/summary/*.csv.

Pure standard library (no scapy, no numpy), so it also runs on a laptop after
copying the result directory over.

    python3 stats/analyze.py result/stats/20261017-120000
"""

from __future__ import annotations

import csv
import json
import math
import struct
import sys
from collections import defaultdict
from pathlib import Path

PSN_MOD = 1 << 24
PATH_ORDER = {"direct": 0, "fast": 1, "ctl": 2}


# --------------------------------------------------------------------------
# Helpers


def read_csv(path) -> list[dict]:
    try:
        with open(path, newline="") as f:
            return list(csv.DictReader(f))
    except FileNotFoundError:
        return []


def write_csv(path: Path, rows: list[dict]) -> None:
    if not rows:
        return
    header = list(dict.fromkeys(key for row in rows for key in row))
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, header)
        writer.writeheader()
        writer.writerows(rows)


def percentile(ordered: list, p: float) -> float:
    if not ordered:
        return math.nan
    rank = (len(ordered) - 1) * p / 100
    low = int(rank)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (rank - low)


def describe(values) -> dict:
    ordered = sorted(values)
    if not ordered:
        return {"n": 0}
    mean = sum(ordered) / len(ordered)
    std = math.sqrt(sum((v - mean) ** 2 for v in ordered) / len(ordered))
    return {"n": len(ordered), "mean": round(mean, 2), "std": round(std, 2),
            "min": round(ordered[0], 2), "p50": round(percentile(ordered, 50), 2),
            "p90": round(percentile(ordered, 90), 2), "p99": round(percentile(ordered, 99), 2),
            "p999": round(percentile(ordered, 99.9), 2), "max": round(ordered[-1], 2)}


def load_runs(out: Path) -> list[tuple[Path, dict]]:
    runs = []
    for path in sorted(out.rglob("*.json")):
        if path.name.endswith((".side.json", ".counters.json")) or path.name in ("env.json",
                                                                                 "install.json"):
            continue
        record = json.loads(path.read_text())
        if isinstance(record, dict) and "mode" in record:
            runs.append((path.with_suffix(""), record))
    return runs


def side(prefix: Path) -> dict:
    path = Path(f"{prefix}.side.json")
    return json.loads(path.read_text()) if path.exists() else {}


def delta(info: dict, dev: str, field: str):
    try:
        return info["ifstats_after"][dev][field] - info["ifstats_before"][dev][field]
    except (KeyError, TypeError):
        return None


def labels(prefix: Path, record: dict) -> dict:
    meta = record.get("meta", {})
    return {"run": prefix.name, "test": meta.get("test"), "path": meta.get("path"),
            "size": record["params"].get("size"), "connections": record.get("connections")}


# --------------------------------------------------------------------------
# Latency (T2, T3, T7)


def latency_rows(runs) -> list[dict]:
    rows = []
    for prefix, record in runs:
        if record["mode"] != "latency":
            continue
        client = read_csv(f"{prefix}.client.csv")
        server = {(r["conn"], r["psn"]): r for r in read_csv(f"{prefix}.server.csv")}
        metrics = defaultdict(list)
        for r in client:
            tx, ack, echo = int(r["tx_ns"]), int(r["ack_ns"]), int(r["echo_ns"])
            if ack:
                metrics["rtt_ack_us"].append((ack - tx) / 1e3)
            if echo:
                metrics["rtt_echo_us"].append((echo - tx) / 1e3)
            s = server.get((r["conn"], r["psn"]))
            if s:
                rx = int(s["rx_ns"])
                metrics["oneway_send_us"].append((rx - tx) / 1e3)
                if ack and int(s["ack_tx_ns"]):
                    metrics["oneway_ack_us"].append((ack - int(s["ack_tx_ns"])) / 1e3)
                    metrics["responder_us"].append((int(s["ack_tx_ns"]) - rx) / 1e3)
        base = {**labels(prefix, record), "rep": record.get("meta", {}).get("rep"),
                "sent": record["sent"], "lost_ack": record["lost_ack"],
                "lost_echo": record["lost_echo"]}
        for metric, values in metrics.items():
            rows.append({**base, "metric": metric, **describe(values)})
    return rows


# --------------------------------------------------------------------------
# Throughput (T5, T6, T7, T9)


def throughput_rows(out: Path, runs) -> list[dict]:
    rows = []
    for prefix, record in runs:
        if record["mode"] != "throughput":
            continue
        tx = {(r["conn"], r["psn"]): int(r["tx_ns"]) for r in read_csv(f"{prefix}.client.csv")}
        server = read_csv(f"{prefix}.server.csv")
        acks = read_csv(f"{prefix}.acks.csv")
        resp = record["responder"]
        owd = [(int(r["rx_ns"]) - tx[(r["conn"], r["psn"])]) / 1e3 for r in server
               if (r["conn"], r["psn"]) in tx]
        rtt = [(int(a["rx_ns"]) - tx[(a["conn"], a["psn"])]) / 1e3 for a in acks
               if (a["conn"], a["psn"]) in tx]
        rx_times = sorted(int(r["rx_ns"]) for r in server)
        span = (rx_times[-1] - rx_times[0]) / 1e9 if len(rx_times) > 1 else 0
        unique = resp["received"] - resp["duplicates"]
        sent = record["sent"]
        info = side(prefix)
        counters = info.get("counters") or {}
        path = record.get("meta", {}).get("path")
        row = {**labels(prefix, record),
               "offered_pps": record.get("meta", {}).get("rate", record["params"]["rate"]),
               "ack_every": record["params"]["ack_every"],
               "sent": sent, "achieved_pps": round(record["achieved_pps"], 1),
               "received": resp["received"], "delivered_pps": round((unique - 1) / span, 1)
               if span else 0, "loss_pct": round(100 * (1 - unique / sent), 3) if sent else None,
               "reordered": resp["reordered"], "duplicates": resp["duplicates"],
               "gaps": resp["gaps"], "missing": resp["missing"],
               "acks_sent": resp["acks_sent"], "acks_received": len(acks),
               "ack_loss_pct": round(100 * (1 - len(acks) / resp["acks_sent"]), 3)
               if resp["acks_sent"] else None}
        for name, values in (("owd", owd), ("rtt", rtt)):
            stats = describe(values)
            row.update({f"{name}_{k}_us": stats.get(k) for k in ("p50", "p90", "p99", "max")})
        # Where Sends went missing: client -> kernel -> BMv2 -> controller -> BMv2 -> responder.
        if path == "direct":
            row.update({"funnel_kernel": delta(info, "node-b", "rx_packets")})
        else:
            row.update({"funnel_kernel": delta(info, "sw-p1", "rx_packets"),
                        "funnel_bmv2_in": counters.get("port_rx_1"),
                        "funnel_bmv2_out": delta(info, "sw-p2", "tx_packets")})
        row.update({f"ctr_{k}": v for k, v in counters.items()})
        for dev in ("sw-p1", "sw-p2", "sw-cpu", "ctl-cpu", "node-a", "node-b"):
            for field in ("rx_dropped", "tx_dropped"):
                value = delta(info, dev, field)
                if value:
                    row[f"{dev}_{field}"] = value
        cpu = defaultdict(list)
        for sample in read_csv(f"{prefix}.cpu.csv"):
            cpu[sample["process"]].append(float(sample["cpu_pct"]))
        row.update({f"cpu_{name}": round(sum(v) / len(v), 1) for name, v in cpu.items() if v})
        rows.append(row)
    return rows


# --------------------------------------------------------------------------
# Stage breakdown from pcaps (T4)


def read_pcap(path):
    data = Path(path).read_bytes()
    formats = {b"\x4d\x3c\xb2\xa1": ("<", 1), b"\xd4\xc3\xb2\xa1": ("<", 1000),
               b"\xa1\xb2\x3c\x4d": (">", 1), b"\xa1\xb2\xc3\xd4": (">", 1000)}
    if data[:4] not in formats:
        raise ValueError(f"{path}: not a pcap file")
    endian, scale = formats[data[:4]]
    offset = 24
    while offset + 16 <= len(data):
        sec, frac, incl, _ = struct.unpack(endian + "IIII", data[offset:offset + 16])
        offset += 16
        yield sec * 1_000_000_000 + frac * scale, data[offset:offset + incl]
        offset += incl


def roce_fields(frame: bytes, o: int):
    """(src IP, dst IP, opcode, dest QP, PSN) of a RoCEv2 frame starting at offset o."""
    if len(frame) < o + 54 or frame[o + 12:o + 14] != b"\x08\x00" or frame[o + 23] != 17 \
            or frame[o + 36:o + 38] != b"\x12\xb7":
        return None
    return (frame[o + 26:o + 30], frame[o + 30:o + 34], frame[o + 42],
            int.from_bytes(frame[o + 47:o + 50], "big"), int.from_bytes(frame[o + 51:o + 54], "big"))


def ip_bytes(text: str) -> bytes:
    return bytes(int(part) for part in text.split("."))


def parse_rules(path) -> dict:
    """(ingress port, src IP, dst IP, QP, opcode) -> (new dst IP, new QP, PSN delta)."""
    rules = {}
    for line in Path(path).read_text().splitlines():
        w = line.split()
        if len(w) == 19 and w[0] == "table_add" and w[8] == "=>":
            rules[(int(w[3]), ip_bytes(w[4]), ip_bytes(w[5]), int(w[6], 0), int(w[7], 0))] = (
                ip_bytes(w[12]), int(w[14], 0), int(w[16], 0))
    return rules


KINDS = {(1, 0x04): "send", (2, 0x11): "ack", (2, 0x04): "echo", (1, 0x11): "echo_ack"}


def stage_rows(out: Path, runs) -> list[dict]:
    rows = []
    for prefix, record in runs:
        if record.get("meta", {}).get("test") != "T4":
            continue
        rules_path = Path(record["params"]["rules"])
        if not rules_path.exists():  # result directory copied off the test host
            rules_path = out / "rules" / rules_path.name
        rules = parse_rules(rules_path)
        times = defaultdict(lambda: defaultdict(list))  # point -> key -> [ns]
        ingress = []  # (port, key, kind) in arrival order
        for dev, direction, cpu in (("sw-p1", "in", 0), ("sw-p2", "in", 0), ("sw-p1", "out", 0),
                                    ("sw-p2", "out", 0), ("sw-cpu", "out", 2), ("sw-cpu", "in", 2)):
            pcap = Path(f"{prefix}.{dev}.{direction}.pcap")
            if not pcap.exists():
                continue
            for ts, frame in read_pcap(pcap):
                fields = roce_fields(frame, cpu)
                if fields is None:
                    continue
                src, dst, opcode, qp, psn = fields
                if direction == "in" and not cpu:
                    port = 1 if dev == "sw-p1" else 2
                    rule = rules.get((port, src, dst, qp, opcode))
                    if rule is None:
                        continue
                    key = (rule[0], rule[1], (psn + rule[2]) % PSN_MOD, opcode)
                    ingress.append((port, key, KINDS.get((port, opcode), "other")))
                else:
                    key = (dst, qp, psn, opcode)
                times[f"{dev}.{direction}"][key].append(ts)
        seen = defaultdict(int)
        for port, key, kind in ingress:
            j = seen[(port, key)]
            seen[(port, key)] += 1

            def at(point):
                values = times[point].get(key, [])
                return values[j] if j < len(values) else None

            t_in = at(f"sw-p{port}.in")
            t_out = at(f"sw-p{3 - port}.out")
            t_punt, t_back = at("sw-cpu.out"), at("sw-cpu.in")
            row = {"run": prefix.name, "path": record["meta"]["path"],
                   "capture": record["meta"].get("capture"), "kind": kind,
                   "delivered": t_out is not None,
                   "total_us": (t_out - t_in) / 1e3 if t_out else None}
            if t_punt and t_back and t_out:
                row.update({"bmv2_ingress_us": (t_punt - t_in) / 1e3,
                            "controller_us": (t_back - t_punt) / 1e3,
                            "bmv2_return_us": (t_out - t_back) / 1e3})
            elif t_out:
                row.update({"bmv2_ingress_us": (t_out - t_in) / 1e3})
            rows.append(row)
    return rows


def stage_summary(rows) -> list[dict]:
    groups = defaultdict(lambda: defaultdict(list))
    for r in rows:
        for stage in ("total_us", "bmv2_ingress_us", "controller_us", "bmv2_return_us"):
            if r.get(stage) is not None:
                groups[(r["path"], r["capture"], r["kind"])][stage].append(r[stage])
    summary = []
    for (path, capture, kind), stages in sorted(groups.items()):
        for stage, values in stages.items():
            summary.append({"path": path, "capture": capture, "kind": kind, "stage": stage,
                            **describe(values)})
    return summary


# --------------------------------------------------------------------------
# Scale (T7) and correctness counters (T1, T8)


def scale_rows(out: Path, latency, throughput) -> list[dict]:
    path = out / "scale" / "install.json"
    if not path.exists():
        return []
    rows = []
    for info in json.loads(path.read_text()):
        k = info["connections"]
        row = {"connections": k, "rules": info["rules"], "added": info["added"],
               "table_full": info["table_full"], "entries": info["entries"],
               "install_s": round(info["seconds"], 3),
               "rules_per_s": round(info["added"] / info["seconds"], 1) if info["seconds"] else None}
        for r in latency:
            if r["run"] == f"k{k}-latency" and r["metric"] == "rtt_ack_us":
                row.update({"rtt_p50_us": r.get("p50"), "rtt_p99_us": r.get("p99"),
                            "lost_ack": r["lost_ack"]})
        for r in throughput:
            if r["run"] == f"k{k}-load":
                row.update({k2: r.get(k2) for k2 in ("offered_pps", "delivered_pps", "loss_pct",
                                                     "owd_p50_us", "owd_p99_us")})
        rows.append(row)
    return rows


def t1_counter_rows(out: Path) -> list[dict]:
    rows = []
    for path in sorted((out / "t1").glob("*.counters.json")):
        scenario, rep = path.name[:-len(".counters.json")].rsplit("-r", 1)
        rows.append({"scenario": scenario, "rep": int(rep), **json.loads(path.read_text())})
    return rows


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1:
        sys.exit(__doc__)
    out = Path(argv[0])
    summary = out / "summary"
    summary.mkdir(exist_ok=True)
    runs = load_runs(out)
    latency = latency_rows(runs)
    throughput = throughput_rows(out, runs)
    stages = stage_rows(out, runs)
    tables = {"latency.csv": latency, "throughput.csv": throughput, "stages.csv": stages,
              "stages_summary.csv": stage_summary(stages),
              "scale.csv": scale_rows(out, latency, throughput),
              "t1_counters.csv": t1_counter_rows(out)}
    for name, rows in tables.items():
        write_csv(summary / name, rows)
    print(f"summary: {len(runs)} runs -> " + ", ".join(
        f"{name} ({len(rows)})" for name, rows in tables.items() if rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())

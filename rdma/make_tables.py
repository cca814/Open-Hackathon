#!/usr/bin/env python3
"""Turn a flows.sh results file into Markdown tables (flows x payload size).

usage: make_tables.py RESULTS_FILE

RESULTS_FILE is what `flows.sh -o` wrote. Each table is one row per flows value
and one column per message size, the mean of the runs of that cell. A line that
does not match the flows.sh format is reported on stderr and left out.
"""
import re
import sys
from collections import defaultdict
from statistics import mean

NUM = r"\d+(?:\.\d+)?"
LINE = re.compile(
    r"flows=(\d+) receivers=(\d+) size=(\d+) count=(\d+) .*?"
    rf"\| source ({NUM}) Mmsg/s in all, ({NUM}) s longest, ({NUM}) Gbps in all "
    r"\| relay in ([\d.,]+) out ([\d.,]+) Gbps "
    r"\| relay busy ([\d.,]+) % "
    rf"\| sinks ({NUM})(?:\.\.({NUM}))? Gbps "
    r"\| verify (\d+)/(\d+) "
    rf"\| latency p50 ({NUM})(?:\.\.({NUM}))? p99 ({NUM})(?:\.\.({NUM}))? us"
    rf"(?: \| wire p0 data ({NUM})x ack ({NUM})x, p1 data ({NUM})x ack ({NUM})x)?")

SIZES = [64, 256, 1024, 4096, 16384, 65536]
LABEL = {64: "64 B", 256: "256 B", 1024: "1 KB", 4096: "4 KB", 16384: "16 KB", 65536: "64 KB"}


def floats(text):
    return [float(x) for x in text.split(",")]


def parse(path):
    runs, bad = [], 0
    for raw in open(path):
        m = LINE.search(raw)
        if not m:
            if raw.strip():
                bad += 1
                print("not parsed: " + raw.strip()[:140], file=sys.stderr)
            continue
        g = m.groups()
        run = dict(
            flows=int(g[0]), receivers=int(g[1]), size=int(g[2]), count=int(g[3]),
            mps=float(g[4]) * 1e6, secs=float(g[5]), src_gbps=float(g[6]),
            r_in=floats(g[7]), r_out=floats(g[8]), busy=floats(g[9]),
            sink_lo=float(g[10]), sink_hi=float(g[11] or g[10]),
            ok=int(g[12]), of=int(g[13]),
            p50_lo=float(g[14]), p50_hi=float(g[15] or g[14]),
            p99_lo=float(g[16]), p99_hi=float(g[17] or g[16]),
            wire=[float(x) for x in g[18:22]] if g[18] else None)
        runs.append(run)
    return runs, bad


def table(title, note, cell, groups, flows_list, sizes):
    print(f"**{title}**" + (f" {note}" if note else ""))
    print()
    print("| flows ＼ payload | " + " | ".join(LABEL[s] for s in sizes) + " |")
    print("|---|" + "---|" * len(sizes))
    for f in flows_list:
        cells = []
        for s in sizes:
            rs = groups.get((f, s))
            cells.append(cell(rs) if rs else "—")
        print(f"| {f} | " + " | ".join(cells) + " |")
    print()


def main(path):
    runs, bad = parse(path)
    if not runs:
        sys.exit("no runs parsed")
    groups = defaultdict(list)
    for r in runs:
        groups[(r["flows"], r["size"])].append(r)
    flows_list = sorted({r["flows"] for r in runs})
    sizes = [s for s in SIZES if any(k[1] == s for k in groups)]
    receivers = sorted({r["receivers"] for r in runs})
    reps = sorted({len(v) for v in groups.values()})
    counts = {s: sorted({r["count"] for r in runs if r["size"] == s}) for s in sizes}

    print(f"{len(runs)} runs from `{path}`; receivers per flow: {receivers}; "
          f"runs per cell: {reps}; messages per flow: "
          + ", ".join(f"{LABEL[s]} {counts[s][0]:,}" for s in sizes) + ".")
    if bad:
        print(f"\n{bad} line(s) were not parsed (see stderr).")
    print()

    def rng(vals, fmt):
        lo, hi = min(vals), max(vals)
        return fmt(lo) if fmt(lo) == fmt(hi) else f"{fmt(lo)}–{fmt(hi)}"

    i0 = lambda v: f"{v:,.0f}"
    f1 = lambda v: f"{v:.1f}"
    f2 = lambda v: f"{v:.2f}"

    table("Message rate, all flows together, msg/s",
          "(flows × count ÷ the longest source time; mean of the runs, range below)",
          lambda rs: f"{mean(r['mps'] for r in rs):,.0f}<br><sub>{rng([r['mps'] for r in rs], i0)}</sub>",
          groups, flows_list, sizes)
    table("Source time, the longest flow, s", "(mean of the runs)",
          lambda rs: f"{mean(r['secs'] for r in rs):.3f}", groups, flows_list, sizes)
    table("Relay input, Gbps per flow", "(mean over the flows and the runs)",
          lambda rs: f"{mean(x for r in rs for x in r['r_in']):.2f}", groups, flows_list, sizes)
    table("Relay output, Gbps, all flows added", "(5 receivers × the input; mean of the runs)",
          lambda rs: f"{mean(sum(r['r_out']) for r in rs):.1f}", groups, flows_list, sizes)
    table("Relay busy, % of the time spent on completions",
          "(the busiest flow; mean of the runs, highest run below)",
          lambda rs: f"{mean(max(r['busy']) for r in rs):.1f}<br><sub>{max(max(r['busy']) for r in rs):.1f}</sub>",
          groups, flows_list, sizes)
    table("Receiver rate, Gbps per receiver", "(lowest–highest over all receivers and runs)",
          lambda rs: rng([r['sink_lo'] for r in rs] + [r['sink_hi'] for r in rs], f2), groups, flows_list, sizes)
    table("Latency, source post to a receiver's poll, p50 / p99, µs",
          "(p50: lowest–highest over all receivers and runs; p99: the highest)",
          lambda rs: f"{rng([r['p50_lo'] for r in rs] + [r['p50_hi'] for r in rs], f1)} / "
                     f"{max(r['p99_hi'] for r in rs):.1f}", groups, flows_list, sizes)
    table("Correctness: receivers that passed `--verify` / receivers checked", "(all runs of the cell)",
          lambda rs: f"{sum(r['ok'] for r in rs)}/{sum(r['of'] for r in rs)}", groups, flows_list, sizes)
    wires = {k: [r for r in v if r['wire']] for k, v in groups.items()}
    if any(wires.values()):
        table("Port bytes ÷ expected, data direction (p0 / p1)",
              "(estimate, see RELAY.md; from 1.2 there was more on the link than the run sent)",
              lambda rs: (lambda w: f"{rng([r['wire'][0] for r in w], f2)} / {rng([r['wire'][2] for r in w], f2)}"
                          if w else "—")([r for r in rs if r['wire']]), groups, flows_list, sizes)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])

#!/usr/bin/env python3
"""Draw the figures and report.md for a stats/suite.py result directory.

Reads RESULT/summary/*.csv (run analyze.py first) plus the raw latency logs,
writes RESULT/figures/*.png and RESULT/report.md. Needs matplotlib.

    python3 stats/analyze.py RESULT && python3 stats/plot.py RESULT
"""

from __future__ import annotations

import csv
import json
import math
import sys
from collections import defaultdict
from pathlib import Path

try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.ticker import FuncFormatter
except ImportError:
    sys.exit("plot.py needs matplotlib: sudo apt install python3-matplotlib "
             "(or pip install matplotlib), or copy the result directory and plot elsewhere")

# Reference palette (light surface); categorical slots in fixed order.
SURFACE, INK, INK_2, MUTED = "#fcfcfb", "#0b0b0b", "#52514e", "#898781"
GRID, AXIS = "#e1e0d9", "#c3c2b7"
SLOTS = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"]
GOOD, CRITICAL = "#0ca30c", "#d03b3b"
BLUE_RAMP = ["#86b6ef", "#5598e7", "#2a78d6", "#1c5cab", "#104281", "#0d366b"]  # ordinal, >=250

PATHS = ["direct", "fast", "ctl"]
PATH_LABEL = {"direct": "direct veth (no switch)", "fast": "BMv2 fast path",
              "ctl": "BMv2 + controller (ICRC repair)"}
PATH_COLOR = dict(zip(PATHS, SLOTS))
STAGE_LABEL = {"bmv2_ingress_us": "BMv2 pass 1", "controller_us": "controller hop",
               "bmv2_return_us": "BMv2 pass 2 (packet_out)"}
KIND_LABEL = {"send": "client Send", "ack": "server ACK", "echo": "server echo Send",
              "echo_ack": "client ACK of echo"}

plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
    "axes.edgecolor": AXIS, "axes.linewidth": 0.8, "axes.labelcolor": INK_2,
    "axes.titlecolor": INK, "axes.titlesize": 11, "axes.titleweight": "bold",
    "axes.titlelocation": "left", "axes.labelsize": 9.5,
    "axes.spines.top": False, "axes.spines.right": False,
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.7, "grid.linestyle": "-",
    "axes.axisbelow": True, "xtick.color": MUTED, "ytick.color": MUTED,
    "xtick.labelcolor": INK_2, "ytick.labelcolor": INK_2, "xtick.labelsize": 8.5,
    "ytick.labelsize": 8.5, "legend.frameon": False, "legend.fontsize": 8.5,
    "legend.labelcolor": INK_2, "lines.linewidth": 1.8, "lines.solid_capstyle": "round",
    "lines.markersize": 6, "font.size": 9.5, "text.color": INK,
})


def read(path: Path) -> list[dict]:
    if not path.exists():
        return []
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def num(value):
    try:
        x = float(value)
        return None if math.isnan(x) else x
    except (TypeError, ValueError):
        return None


def compact(x, _=None) -> str:
    if x >= 1e6:
        return f"{x / 1e6:g}M"
    if x >= 1e3:
        return f"{x / 1e3:g}K"
    return f"{x:g}"


def marker(ax, xs, ys, color, label=None, **kw):
    """2px line with ringed markers (surface-colored edge separates overlaps)."""
    ax.plot(xs, ys, color=color, marker="o", markeredgecolor=SURFACE, markeredgewidth=1.5,
            label=label, **kw)


def ramp(n: int) -> list[str]:
    """n ordinal steps spread over the blue ramp, light to dark."""
    if n == 1:
        return [BLUE_RAMP[len(BLUE_RAMP) // 2]]
    return [BLUE_RAMP[round(i * (len(BLUE_RAMP) - 1) / (n - 1))] if n <= len(BLUE_RAMP)
            else BLUE_RAMP[i * len(BLUE_RAMP) // n] for i in range(n)]


def finish(fig, path: Path, title: str, subtitle: str = "", legend_title: str | None = None):
    """Title block above, one shared legend below (only for two or more series)."""
    h = fig.get_figheight()
    fig.text(0.01, 1 - 0.15 / h, title, ha="left", va="top", fontsize=13, fontweight="bold",
             color=INK)
    top = 1 - 0.5 / h
    if subtitle:
        fig.text(0.01, 1 - 0.46 / h, subtitle, ha="left", va="top", fontsize=9, color=INK_2)
        top = 1 - 0.75 / h
    handles = {}
    for ax in fig.axes:
        for handle, label in zip(*ax.get_legend_handles_labels()):
            if not label.startswith("_"):
                handles.setdefault(label, handle)
    bottom = 0
    if len(handles) > 1:
        fig.legend(list(handles.values()), list(handles.keys()), loc="lower center",
                   bbox_to_anchor=(0.5, 0.01), ncol=min(len(handles), 6), title=legend_title,
                   title_fontsize=8.5)
        bottom = (0.4 + (0.22 if legend_title else 0)) / h
    fig.tight_layout(rect=(0, bottom, 1, top))
    fig.savefig(path, dpi=144)
    plt.close(fig)


class Report:
    def __init__(self, out: Path):
        self.out = out
        self.figures = out / "figures"
        self.figures.mkdir(exist_ok=True)
        self.summary = out / "summary"
        self.sections = []
        self.made = []

    def add(self, heading: str, text: str = "", figure: str | None = None,
            table: list[list] | None = None) -> None:
        lines = [f"## {heading}", ""]
        if text:
            lines += [text, ""]
        if figure:
            lines += [f"![{heading}](figures/{figure})", ""]
            self.made.append(figure)
        if table:
            lines += ["| " + " | ".join(map(str, table[0])) + " |",
                      "|" + "---|" * len(table[0])]
            lines += ["| " + " | ".join("" if v is None else str(v) for v in row) + " |"
                      for row in table[1:]]
            lines.append("")
        self.sections.append("\n".join(lines))

    # T1 -------------------------------------------------------------------

    def correctness(self) -> None:
        rows = read(self.out / "correctness.csv")
        if not rows:
            return
        runs = list(dict.fromkeys(f"{r['scenario']} #{r['rep']}" for r in rows))
        checks = list(dict.fromkeys(r["check"] for r in rows))
        grid = {(f"{r['scenario']} #{r['rep']}", r["check"]): r["result"] for r in rows}
        fig, ax = plt.subplots(figsize=(1.6 + 1.35 * len(runs), 0.8 + 0.27 * len(checks)))
        ax.grid(False)
        colors = {"PASS": GOOD, "FAIL": CRITICAL, "SKIP": AXIS}
        for x, run in enumerate(runs):
            for y, check in enumerate(checks):
                result = grid.get((run, check))
                if result is None:
                    continue
                ax.add_patch(plt.Rectangle((x + 0.04, y + 0.06), 0.92, 0.88,
                                           color=colors[result], linewidth=0))
                ax.text(x + 0.5, y + 0.5, result, ha="center", va="center", fontsize=7,
                        color="white" if result != "SKIP" else INK, fontweight="bold")
        ax.set_xlim(0, len(runs))
        ax.set_ylim(len(checks), 0)
        ax.set_xticks([x + 0.5 for x in range(len(runs))], runs, rotation=30, ha="right")
        ax.set_yticks([y + 0.5 for y in range(len(checks))], checks)
        ax.tick_params(length=0)
        for spine in ax.spines.values():
            spine.set_visible(False)
        finish(fig, self.figures / "t1_correctness.png", "T1 functional checks",
               "scapy/client.py per scenario and repetition")
        totals = defaultdict(lambda: defaultdict(int))
        for r in rows:
            totals[f"{r['scenario']} #{r['rep']}"][r["result"]] += 1
        table = [["run", "PASS", "FAIL", "SKIP"]] + [
            [run, totals[run]["PASS"], totals[run]["FAIL"], totals[run]["SKIP"]] for run in runs]
        self.add("T1 功能正確性", "每格為 scapy/client.py 的一項檢查；translate 為現行轉換規則，"
                 "psn-wrap 讓 PSN 跨越 0xffffff，passthrough 為不經 controller 的直通規則。",
                 "t1_correctness.png", table)

    def t1_counters(self) -> None:
        order = ["translate", "translate-psn-wrap", "passthrough"]
        rows = sorted((r for r in read(self.summary / "t1_counters.csv") if r["rep"] == "1"),
                      key=lambda r: order.index(r["scenario"]) if r["scenario"] in order else 9)
        if not rows:
            return
        names = [k for k in rows[0] if k not in ("scenario", "rep") and
                 any(num(r[k]) for r in rows)]
        fig, ax = plt.subplots(figsize=(8, 0.9 + 0.5 * len(names)))
        height = min(0.8 / len(rows), 0.3)
        for i, r in enumerate(rows):
            ys = [y + (i - (len(rows) - 1) / 2) * height for y in range(len(names))]
            ax.barh(ys, [num(r[n]) or 0 for n in names], height=height * 0.85,
                    color=SLOTS[i], label=r["scenario"])
        ax.set_yticks(range(len(names)), names)
        ax.invert_yaxis()
        ax.set_xlabel("packets during one client.py run")
        ax.grid(axis="y", visible=False)
        finish(fig, self.figures / "t8_counters_t1.png", "T8 switch counters during T1",
               "where every packet of a correctness run ended up in ingress")
        self.add("T8 交換器計數器（T1 期間）", "16 種應丟棄封包應分別落在 drop_* 原因；"
                 "translate 的封包全數 punt_icrc，passthrough 全數 fast_path。",
                 "t8_counters_t1.png",
                 [["counter"] + [r["scenario"] for r in rows]] +
                 [[n] + [r[n] for r in rows] for n in names])

    # T2/T3 ----------------------------------------------------------------

    def latency(self) -> None:
        rows = read(self.summary / "latency.csv")
        base = [r for r in rows if r["test"] == "T2/T3"]
        if not base:
            return
        sizes = sorted({int(r["size"]) for r in base})
        size = 64 if 64 in sizes else sizes[len(sizes) // 2]

        # One-way Send delay (client send() -> responder kernel receive) isolates the path;
        # the round trip also carries the responder's Python wake-up time.
        fig, (ax, bx) = plt.subplots(1, 2, figsize=(11, 4.2), width_ratios=[1.6, 1])
        table = [["path", "metric", "n", "p50 µs", "p90 µs", "p99 µs", "p99.9 µs", "lost"]]
        box_data, box_paths = [], []
        for path in PATHS:
            oneway, rtt = [], []
            for log in sorted((self.out / "latency").glob(f"{path}-s{size}-r*.client.csv")):
                server = {(r["conn"], r["psn"]): int(r["rx_ns"])
                          for r in read(Path(str(log).replace(".client.csv", ".server.csv")))}
                for r in read(log):
                    rx = server.get((r["conn"], r["psn"]))
                    if rx:
                        oneway.append((rx - int(r["tx_ns"])) / 1e3)
                    if int(r["ack_ns"]):
                        rtt.append((int(r["ack_ns"]) - int(r["tx_ns"])) / 1e3)
            if oneway:
                oneway.sort()
                ax.plot(oneway, [(i + 1) / len(oneway) for i in range(len(oneway))],
                        color=PATH_COLOR[path], label=PATH_LABEL[path])
            if rtt:
                box_data.append(rtt)
                box_paths.append(path)
        ax.set_xscale("log")
        ax.xaxis.set_major_formatter(FuncFormatter(compact))
        ax.set_xlabel("one-way Send delay, client → responder (µs, log)")
        ax.set_ylabel("fraction of messages")
        ax.set_title(f"CDF, {size} B payload")
        if box_data:
            style = dict(widths=0.45, showfliers=False, patch_artist=True,
                         medianprops={"color": INK, "linewidth": 1.5},
                         whiskerprops={"color": MUTED}, capprops={"color": MUTED})
            try:
                parts = bx.boxplot(box_data, orientation="horizontal", **style)
            except TypeError:  # matplotlib < 3.10
                parts = bx.boxplot(box_data, vert=False, **style)
            for patch, path in zip(parts["boxes"], box_paths):
                patch.set(facecolor=PATH_COLOR[path], alpha=0.35, edgecolor=PATH_COLOR[path])
            bx.set_yticks(range(1, len(box_paths) + 1), box_paths)
            bx.set_xscale("log")
            bx.xaxis.set_major_formatter(FuncFormatter(compact))
            bx.set_xlabel("µs (log), whiskers at 1.5 IQR")
            bx.set_title("Send → ACK round trip")
            bx.invert_yaxis()
        finish(fig, self.figures / "t2_latency_cdf.png", "T2 latency by path",
               "one message in flight; kernel receive timestamps")

        pooled = defaultdict(list)
        for r in base:
            if int(r["size"]) == size:
                pooled[(r["path"], r["metric"])].append(r)
        for path in PATHS:
            for metric in ("oneway_send_us", "oneway_ack_us", "rtt_ack_us", "rtt_echo_us",
                           "responder_us"):
                group = pooled.get((path, metric))
                if group:
                    avg = lambda k: round(sum(num(g[k]) or 0 for g in group) / len(group), 1)  # noqa
                    table.append([path, metric, sum(int(g["n"]) for g in group), avg("p50"),
                                  avg("p90"), avg("p99"), avg("p999"),
                                  sum(int(g["lost_ack"]) for g in group)])
        self.add("T2 延遲基線比較", "三條路徑相減即可拆出成本：fast − direct 為 BMv2 管線，"
                 "ctl − fast 為 controller 繞行（含兩次額外的 BMv2 處理）。多次重複的百分位取平均。",
                 "t2_latency_cdf.png", table)

        if len(sizes) > 1:
            fig, axes = plt.subplots(1, 2, figsize=(11, 4), sharey=True)
            for ax, stat in zip(axes, ("p50", "p99")):
                for path in PATHS:
                    points = defaultdict(list)
                    for r in base:
                        if r["path"] == path and r["metric"] == "oneway_send_us":
                            points[int(r["size"])].append(num(r[stat]))
                    if points:
                        xs = sorted(points)
                        marker(ax, xs, [sum(points[x]) / len(points[x]) for x in xs],
                               PATH_COLOR[path], PATH_LABEL[path])
                ax.set_xscale("log", base=2)
                ax.xaxis.set_major_formatter(FuncFormatter(lambda x, _: f"{x:g}"))
                ax.set_xlabel("Send payload (bytes, log2)")
                ax.set_title(f"{stat} one-way Send delay")
                ax.set_ylim(bottom=0)
            axes[0].set_ylabel("µs")
            finish(fig, self.figures / "t3_latency_size.png", "T3 latency vs message size",
                   "1..256 B, one packet per message")
            self.add("T3 延遲對訊息大小", "ICRC 是對整個封包算 CRC，controller 的成本應隨大小略增。",
                     "t3_latency_size.png")

    # T4 -------------------------------------------------------------------

    def stages(self) -> None:
        rows = [r for r in read(self.summary / "stages_summary.csv")]
        if not rows:
            return
        bars = []  # (label, {stage: p50})
        for path in ("fast", "ctl"):
            for kind in KIND_LABEL:
                stages = {r["stage"]: num(r["p50"]) for r in rows if r["path"] == path
                          and r["kind"] == kind and r["capture"] == "latency"}
                if stages:
                    bars.append((f"{path}: {KIND_LABEL[kind]}", stages))
        if bars:
            fig, ax = plt.subplots(figsize=(10, 0.8 + 0.45 * len(bars)))
            for i, (label, stages) in enumerate(bars):
                left = 0
                for j, stage in enumerate(STAGE_LABEL):
                    width = stages.get(stage)
                    if not width:
                        continue
                    ax.barh(i, width, left=left, height=0.55, color=SLOTS[j],
                            edgecolor=SURFACE, linewidth=2, label=STAGE_LABEL[stage])
                    left += width
                ax.text(left, i, f"  {left:,.0f} µs", va="center", fontsize=8, color=INK_2)
            ax.set_yticks(range(len(bars)), [b[0] for b in bars])
            ax.invert_yaxis()
            ax.set_xlabel("median µs from switch ingress to switch egress (pcap timestamps)")
            ax.grid(axis="y", visible=False)
            ax.set_xlim(right=ax.get_xlim()[1] * 1.12)
            finish(fig, self.figures / "t4_stages.png", "T4 where the time goes inside the switch",
                   "median per stage, one message in flight")

        service = defaultdict(list)
        for r in read(self.summary / "stages.csv"):
            if r["path"] == "ctl" and num(r.get("controller_us")) is not None:
                service[r["capture"]].append(num(r["controller_us"]))
        if service:
            fig, ax = plt.subplots(figsize=(8, 3.8))
            names = {"latency": "idle, one message in flight", "load": "under load"}
            for i, (capture, values) in enumerate(sorted(service.items())):
                values.sort()
                ax.plot(values, [(k + 1) / len(values) for k in range(len(values))],
                        color=SLOTS[i], label=f"{names.get(capture, capture)} "
                        f"({len(values)} packets)")
            ax.set_xscale("log")
            ax.xaxis.set_major_formatter(FuncFormatter(compact))
            ax.set_xlabel("packet_in → packet_out on the CPU port (µs, log)")
            ax.set_ylabel("fraction of packets")
            finish(fig, self.figures / "t4_controller_cdf.png", "T4 controller service time",
                   "idle (one message in flight) vs under load")
        table = [["path", "capture", "kind", "stage", "n", "p50 µs", "p99 µs"]] + [
            [r["path"], r["capture"], r["kind"], r["stage"], r["n"], r["p50"], r["p99"]]
            for r in rows]
        self.add("T4 延遲拆解", "以交換器三個介面的 pcap 時間戳拆出 BMv2 第一次處理、controller "
                 "往返（含 veth 與 socket）、packet_out 回到 BMv2 後的第二次處理。",
                 "t4_stages.png" if bars else None, table)
        if service:
            self.add("T4 controller 服務時間", "", "t4_controller_cdf.png")

    # T5/T6/T9 -------------------------------------------------------------

    def throughput(self) -> None:
        rows = [r for r in read(self.summary / "throughput.csv") if r["test"] == "T5"]
        if not rows:
            return
        by_path = defaultdict(list)
        for r in rows:
            by_path[r["path"]].append(r)
        for path in by_path:
            by_path[path].sort(key=lambda r: num(r["offered_pps"]))

        def series(ax, key, transform=lambda v: v):
            for path in PATHS:
                points = [(num(r["achieved_pps"]), num(r[key])) for r in by_path.get(path, [])
                          if num(r.get(key)) is not None]
                if points:
                    marker(ax, [p[0] for p in points], [transform(p[1]) for p in points],
                           PATH_COLOR[path], PATH_LABEL[path])
            ax.set_xscale("log")
            ax.xaxis.set_major_formatter(FuncFormatter(compact))
            ax.set_xlabel("offered Sends/s (achieved by the generator, log)")

        fig, (ax, bx) = plt.subplots(1, 2, figsize=(11, 4.2))
        series(ax, "delivered_pps")
        lo, hi = ax.get_xlim()
        ax.plot([lo, hi], [lo, hi], color=AXIS, linewidth=1, zorder=0)
        ax.text(hi, hi, "delivered = offered ", ha="right", va="bottom", fontsize=8, color=MUTED)
        ax.set_yscale("log")
        ax.yaxis.set_major_formatter(FuncFormatter(compact))
        ax.set_ylabel("delivered Sends/s at the responder (log)")
        ax.set_title("goodput")
        series(bx, "loss_pct")
        bx.set_ylabel("% of Sends never delivered")
        bx.set_ylim(bottom=0)
        bx.set_title("loss")
        finish(fig, self.figures / "t5_throughput.png", "T5 throughput and saturation",
               "open-loop Sends, ACK per Send; the direct path is the generator's own ceiling")

        fig, axes = plt.subplots(1, 2, figsize=(11, 4), sharey=True)
        for ax, stat in zip(axes, ("p50", "p99")):
            series(ax, f"owd_{stat}_us")
            ax.set_yscale("log")
            ax.yaxis.set_major_formatter(FuncFormatter(compact))
            ax.set_title(f"{stat} one-way delay, client → responder")
        axes[0].set_ylabel("µs (log)")
        finish(fig, self.figures / "t5_latency_load.png", "T5 latency under load",
               "queueing shows up as the knee where p99 leaves the idle level")

        fig, axes = plt.subplots(1, 2, figsize=(11, 4))
        series(axes[0], "gaps")
        axes[0].set_ylabel("sequence errors (would-NAK events)")
        axes[0].set_title("PSN gaps at the responder")
        series(axes[1], "reordered")
        axes[1].set_ylabel("Sends older than the expected PSN")
        axes[1].set_title("reordered arrivals")
        for ax in axes:
            ax.set_ylim(bottom=0)
        finish(fig, self.figures / "t6_order.png", "T6 ordering under load",
               "an RC receiver NAKs every gap and drops later packets until the retransmission")

        processes = [p for p in ("switch", "controller", "responder", "bench")
                     if any(num(r.get(f"cpu_{p}")) is not None for r in rows)]
        if processes:
            fig, axes = plt.subplots(1, len(processes), figsize=(3.2 * len(processes), 3.8),
                                     sharey=True)
            axes = list(axes) if len(processes) > 1 else [axes]
            for ax, process in zip(axes, processes):
                series(ax, f"cpu_{process}")
                ax.set_title(process)
                ax.set_ylim(bottom=0)
                ax.set_xlabel("offered Sends/s (log)")
            axes[0].set_ylabel("CPU % (100 = one core)")
            finish(fig, self.figures / "t9_cpu.png", "T9 CPU use per process",
                   "average over each run, children included")

        funnel_paths = [p for p in ("fast", "ctl") if by_path.get(p)]
        if funnel_paths:
            stages = [("sent", "sent"), ("funnel_kernel", "kernel → sw-p1"),
                      ("funnel_bmv2_in", "BMv2 ingress"), ("funnel_bmv2_out", "BMv2 → sw-p2"),
                      ("received", "responder")]
            fig, axes = plt.subplots(1, len(funnel_paths), figsize=(5.5 * len(funnel_paths), 4),
                                     sharey=True)
            axes = list(axes) if len(funnel_paths) > 1 else [axes]
            for ax, path in zip(axes, funnel_paths):
                runs = by_path[path]
                for r, color in zip(runs, ramp(len(runs))):
                    sent = num(r["sent"]) or 1
                    ys = [100 * (num(r.get(k)) or 0) / sent for k, _ in stages]
                    marker(ax, range(len(stages)), ys, color, f"{compact(num(r['offered_pps']))}/s")
                ax.set_xticks(range(len(stages)), [s[1] for s in stages], rotation=20, ha="right")
                ax.set_title(PATH_LABEL[path])
                ax.set_ylim(0, 105)
            axes[0].set_ylabel("% of Sends still alive")
            finish(fig, self.figures / "t6_loss_funnel.png", "T6 where Sends are lost",
                   "darker = higher offered rate", legend_title="offered Sends/s")

        table = [["path", "offered/s", "achieved/s", "delivered/s", "loss %", "gaps", "reordered",
                  "owd p50 µs", "owd p99 µs", "switch CPU %", "controller CPU %"]]
        for path in PATHS:
            for r in by_path.get(path, []):
                table.append([path, r["offered_pps"], r["achieved_pps"], r["delivered_pps"],
                              r["loss_pct"], r["gaps"], r["reordered"], r["owd_p50_us"],
                              r["owd_p99_us"], r.get("cpu_switch"), r.get("cpu_controller")])
        self.add("T5 吞吐量與飽和點", "", "t5_throughput.png", table)
        self.add("T5 負載下的延遲", "", "t5_latency_load.png")
        self.add("T6 亂序與遺失", "responder 不 NAK，只記錄真實 RC 接收端會 NAK 的序號缺口；"
                 "漏斗圖由 bench 送出數、veth 計數、P4 port_rx 計數與 responder 收到數組成。",
                 "t6_order.png")
        if funnel_paths:
            self.add("T6 遺失位置", "", "t6_loss_funnel.png")
        if processes:
            self.add("T9 資源使用", "", "t9_cpu.png")

        coalesce = [r for r in read(self.summary / "throughput.csv") if r["test"] == "T5b"]
        if coalesce:
            table = [["path", "offered/s", "ACK every", "delivered/s", "loss %", "ACK loss %",
                      "controller CPU %"]]
            table += [[r["path"], r["offered_pps"], r["ack_every"], r["delivered_pps"],
                       r["loss_pct"], r["ack_loss_pct"], r.get("cpu_controller")]
                      for r in coalesce]
            self.add("T5b ACK 合併的影響", "ACK 同樣要經過 controller；每 N 個 Send 才回一個 ACK "
                     "可減少 CPU 埠負載（真實 RNIC 也會合併 ACK）。", table=table)

    # T7 -------------------------------------------------------------------

    def scale(self) -> None:
        rows = [r for r in read(self.summary / "scale.csv") if num(r.get("rtt_p50_us")) is not None
                or num(r.get("install_s")) is not None]
        if not rows:
            return
        loaded = [r for r in rows if int(r["connections"]) <= 1024]
        xs = [int(r["connections"]) for r in loaded]
        fig, axes = plt.subplots(1, 3, figsize=(13, 3.8))
        marker(axes[0], xs, [num(r["install_s"]) for r in loaded], SLOTS[0])
        axes[0].set_ylabel("seconds (simple_switch_CLI)")
        axes[0].set_title("rule install time")
        for stat, color in (("rtt_p50_us", SLOTS[0]), ("rtt_p99_us", SLOTS[1])):
            pts = [(int(r["connections"]), num(r.get(stat))) for r in loaded
                   if num(r.get(stat)) is not None]
            if pts:
                marker(axes[1], [p[0] for p in pts], [p[1] for p in pts], color,
                       stat.split("_")[1])
        axes[1].set_ylabel("Send → ACK µs")
        axes[1].set_title("latency, round-robin over connections")
        pts = [(int(r["connections"]), num(r.get("loss_pct"))) for r in loaded
               if num(r.get("loss_pct")) is not None]
        if pts:
            marker(axes[2], [p[0] for p in pts], [p[1] for p in pts], SLOTS[0])
        rate = next((r.get("offered_pps") for r in loaded if r.get("offered_pps")), "?")
        axes[2].set_ylabel("% of Sends lost")
        axes[2].set_title(f"loss at {rate} Sends/s")
        for ax in axes:
            ax.set_xscale("log", base=2)
            ax.xaxis.set_major_formatter(FuncFormatter(lambda x, _: f"{x:g}"))
            ax.set_xlabel("connections (4 rules each, log2)")
            ax.set_ylim(bottom=0)
        finish(fig, self.figures / "t7_scale.png", "T7 scaling with the number of connections",
               "BMv2 + controller; the table holds 4096 rules = 1024 connections")
        table = [["connections", "rules", "added", "TABLE_FULL", "entries", "install s",
                  "rules/s", "rtt p50 µs", "rtt p99 µs", "loss %"]]
        table += [[r["connections"], r["rules"], r["added"], r["table_full"], r["entries"],
                   r["install_s"], r["rules_per_s"], r.get("rtt_p50_us"), r.get("rtt_p99_us"),
                   r.get("loss_pct")] for r in rows]
        self.add("T7 連線數擴展性", "1025 條連線用來確認表滿時其餘規則被拒（TABLE_FULL）。",
                 "t7_scale.png", table)

    def write(self) -> None:
        env_path = self.out / "env.json"
        env = json.loads(env_path.read_text()) if env_path.exists() else {}
        head = ["# P4 RC 轉換器量測報告", "",
                f"- 時間：{env.get('date', '?')}　主機：{env.get('host', '?')}",
                f"- CPU：{env.get('cpu', '?')} × {env.get('cpus', '?')}　kernel：{env.get('kernel', '?')}",
                f"- commit：`{env.get('git_commit', '?')[:12]}`（{env.get('git_branch', '?')}）",
                f"- BMv2：{env.get('simple_switch', '?')}　scapy：{env.get('scapy', '?')}", "",
                "BMv2 是軟體交換器，絕對數值不代表硬體；請看路徑之間的差距、趨勢與比例。"
                "原始數據與彙整表在 `summary/*.csv`。", ""]
        (self.out / "report.md").write_text("\n".join(head) + "\n" + "\n".join(self.sections))


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1:
        sys.exit(__doc__)
    report = Report(Path(argv[0]))
    for step in (report.correctness, report.t1_counters, report.latency, report.stages,
                 report.throughput, report.scale):
        try:
            step()
        except Exception as error:  # one broken figure should not cost the others
            print(f"plot: {step.__name__} failed: {error!r}", file=sys.stderr)
    report.write()
    print(f"plot: {len(report.made)} figures in {report.figures}, report in "
          f"{report.out / 'report.md'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

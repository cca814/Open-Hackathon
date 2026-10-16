#!/usr/bin/env python3
"""Run the measurement plan of stats/README.md on one Linux host.

Builds the P4 program, wires BMv2 and the scapy nodes with veth pairs, runs
every selected test and writes raw data under --out, then summarizes
(analyze.py) and plots (plot.py, needs matplotlib). Needs root.

    sudo python3 stats/suite.py                 # everything, ~15 minutes
    sudo python3 stats/suite.py --quick         # fewer points, ~4 minutes
    sudo python3 stats/suite.py --tests T1,T2   # a subset
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
from contextlib import contextmanager
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PY = sys.executable
THRIFT_PORT = 9090
SERVER_TCP_PORT = 18515  # scapy/server.py
CONTROL_PORT = 18516  # responder.py
SWITCH_IFACES = ("sw-p1", "sw-p2", "sw-cpu")
NODE_IFACES = ("node-a", "node-b", "ctl-cpu")
STAT_NAMES = ["drop_malformed", "drop_unsupported", "drop_bad_ack", "drop_bad_packet_out",
              "packet_out", "drop_ingress", "drop_miss", "drop_egress", "punt_icrc", "fast_path"]
COUNTED_PORTS = (1, 2, 510)

# Paths compared in T2-T6: rules mode and whether BMv2 sits in between.
PATHS = {
    "direct": ("passthrough", False),  # node-a <-> node-b, no switch
    "fast": ("passthrough", True),  # BMv2, ICRC untouched, no controller hop
    "ctl": ("translate", True),  # BMv2 + controller ICRC repair
}
ALL_TESTS = ["T1", "T2", "T3", "T4", "T5", "T7"]  # T6, T8, T9 are analyses of these runs


def log(text: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {text}", flush=True)


def sh(*cmd, check=True, **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=ROOT, check=check, text=True, capture_output=True, **kwargs)


def port_open(port: int) -> bool:
    with socket.socket() as s:
        return s.connect_ex(("127.0.0.1", port)) == 0


def wait_port(port: int, timeout: float, proc=None) -> None:
    deadline = time.monotonic() + timeout
    while not port_open(port):
        if proc is not None and proc.poll() is not None:
            raise RuntimeError(f"process for port {port} exited with {proc.returncode}")
        if time.monotonic() > deadline:
            raise RuntimeError(f"nothing listening on port {port} after {timeout}s")
        time.sleep(0.1)


# --------------------------------------------------------------------------
# Processes, links and the switch


class Lab:
    """Background processes of the current scenario."""

    def __init__(self, out: Path):
        self.logs = out / "logs"
        self.logs.mkdir(parents=True, exist_ok=True)
        self.procs = {}

    def spawn(self, name: str, *cmd, stop_signal=signal.SIGINT) -> subprocess.Popen:
        logfile = open(self.logs / f"{name}.log", "a")
        logfile.write(f"\n$ {' '.join(map(str, cmd))}\n")
        logfile.flush()
        proc = subprocess.Popen([str(c) for c in cmd], cwd=ROOT, stdout=logfile,
                                stderr=subprocess.STDOUT, start_new_session=True)
        self.procs[name] = (proc, logfile, stop_signal)
        return proc

    def pid(self, name: str):
        entry = self.procs.get(name)
        return entry[0].pid if entry and entry[0].poll() is None else None

    def stop(self, name: str) -> None:
        proc, logfile, stop_signal = self.procs.pop(name)
        if proc.poll() is None:
            proc.send_signal(stop_signal)
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        logfile.close()

    def stop_all(self) -> None:
        for name in list(self.procs):
            self.stop(name)


def links_down() -> None:
    sh("bash", "scapy/veth.sh", "down", check=False)
    sh("ip", "link", "del", "node-a", check=False)


def links_switch() -> None:
    links_down()
    sh("bash", "scapy/veth.sh", "up")


def links_direct() -> None:
    links_down()
    sh("ip", "link", "add", "node-a", "type", "veth", "peer", "name", "node-b")
    for dev in ("node-a", "node-b"):
        sh("sysctl", "-qw", f"net.ipv6.conf.{dev}.disable_ipv6=1", check=False)
        sh("ip", "link", "set", "dev", dev, "up")


def cli(commands: str) -> str:
    return subprocess.run(["simple_switch_CLI", "--thrift-port", str(THRIFT_PORT)], input=commands,
                          text=True, capture_output=True, cwd=ROOT).stdout


def install_rules(rules: Path) -> dict:
    start = time.perf_counter()
    output = cli(rules.read_text())
    seconds = time.perf_counter() - start
    entries = re.findall(r"(\d+)", cli("table_num_entries MyIngress.connections\n"))
    return {"rules": len(rules.read_text().splitlines()), "seconds": seconds,
            "added": output.count("Entry has been added"), "table_full": output.count("TABLE_FULL"),
            "entries": int(entries[-1]) if entries else None}


def reset_counters() -> None:
    cli("counter_reset MyIngress.stats\ncounter_reset MyIngress.port_rx\n")


def read_counters() -> dict:
    commands = [f"counter_read MyIngress.stats {i}" for i in range(len(STAT_NAMES))]
    commands += [f"counter_read MyIngress.port_rx {p}" for p in COUNTED_PORTS]
    # BMv2 prints "(B bytes, P packets)"; older releases "BmCounterValue(packets=P, bytes=B)".
    found = {(name, int(index)): int(old or new) for name, index, old, new in re.findall(
        r"MyIngress\.(\w+)\[(\d+)\]=\s*(?:BmCounterValue\(packets=(\d+)|\(\d+ bytes, (\d+) packets\))",
        cli("\n".join(commands) + "\n"))}
    result = {name: found.get(("stats", i)) for i, name in enumerate(STAT_NAMES)}
    result.update({f"port_rx_{p}": found.get(("port_rx", p)) for p in COUNTED_PORTS})
    return result


def iface_stats() -> dict:
    stats = {}
    for dev in SWITCH_IFACES + NODE_IFACES:
        base = Path("/sys/class/net") / dev / "statistics"
        if base.exists():
            stats[dev] = {name: int((base / name).read_text()) for name in
                          ("rx_packets", "tx_packets", "rx_dropped", "tx_dropped")}
    return stats


class CpuSampler:
    """CPU% of named processes (with their children), sampled from /proc."""

    def __init__(self, pids: dict, interval: float = 0.25):
        self.pids = {name: pid for name, pid in pids.items() if pid}
        self.interval = interval
        self.rows = []
        self.tick = os.sysconf("SC_CLK_TCK")
        self.done = threading.Event()
        self.thread = threading.Thread(target=self.loop, daemon=True)

    @staticmethod
    def snapshot():
        procs = {}
        for entry in os.scandir("/proc"):
            if entry.name.isdigit():
                try:
                    fields = Path(entry.path, "stat").read_text().rsplit(")", 1)[1].split()
                except OSError:
                    continue
                procs[int(entry.name)] = (int(fields[1]), int(fields[11]) + int(fields[12]))
        return procs  # pid -> (ppid, utime + stime ticks)

    def busy(self, procs, root: int) -> int:
        total, stack = 0, [root]
        children = {}
        for pid, (ppid, _) in procs.items():
            children.setdefault(ppid, []).append(pid)
        while stack:
            pid = stack.pop()
            total += procs.get(pid, (0, 0))[1]
            stack += children.get(pid, [])
        return total

    def loop(self) -> None:
        start = last_t = time.monotonic()
        procs = self.snapshot()
        last = {name: self.busy(procs, pid) for name, pid in self.pids.items()}
        while not self.done.wait(self.interval):
            now, procs = time.monotonic(), self.snapshot()
            for name, pid in self.pids.items():
                busy = self.busy(procs, pid)
                pct = 100.0 * (busy - last[name]) / self.tick / (now - last_t)
                self.rows.append((round(now - start, 3), name, round(pct, 1)))
                last[name] = busy
            last_t = now

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.done.set()
        self.thread.join()


# --------------------------------------------------------------------------
# Scenario


class Scenario:
    def __init__(self, suite, path_name: str, rules: Path, switch: bool, controller: bool):
        self.suite = suite
        self.path = path_name
        self.rules = rules
        self.switch = switch
        self.controller = controller
        self.install = None

    def bench(self, prefix: Path, mode: str, meta: dict, *extra, cpu=False) -> dict:
        """Run bench.py once with counters, interface stats and CPU samples around it."""
        meta = {"path": self.path, **meta}
        prefix.parent.mkdir(parents=True, exist_ok=True)
        if self.switch:
            reset_counters()
        before = iface_stats()
        cmd = [PY, HERE / "bench.py", mode, "--rules", self.rules, "--out", prefix,
               "--meta", json.dumps(meta), *extra]
        proc = subprocess.Popen([str(c) for c in cmd], cwd=ROOT, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        lab = self.suite.lab
        sampler = CpuSampler({"bench": proc.pid, "switch": lab.pid("switch"),
                              "controller": lab.pid("controller"),
                              "responder": lab.pid("responder")}) if cpu else None
        if sampler:
            sampler.__enter__()
        output, _ = proc.communicate()
        if sampler:
            sampler.__exit__()
            write_rows(f"{prefix}.cpu.csv", ["t", "process", "cpu_pct"], sampler.rows)
        side = {"ifstats_before": before, "ifstats_after": iface_stats(),
                "counters": read_counters() if self.switch else None}
        Path(f"{prefix}.side.json").write_text(json.dumps(side, indent=2))
        log(f"  {prefix.name}: {output.strip().splitlines()[-1] if output.strip() else '(no output)'}")
        if proc.returncode:
            raise RuntimeError(f"bench.py failed:\n{output}")
        return side


def write_rows(path, header, rows) -> None:
    import csv
    with open(path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(header)
        writer.writerows(rows)


class Suite:
    def __init__(self, args):
        self.args = args
        self.out = args.out
        self.lab = Lab(self.out)
        self.rules_dir = self.out / "rules"
        self.rules_dir.mkdir(parents=True, exist_ok=True)

    def rules(self, mode: str, connections: int = 1) -> Path:
        path = self.rules_dir / f"{mode}-{connections}.txt"
        if not path.exists():
            sh(PY, HERE / "gen_rules.py", mode, "-n", str(connections), "-o", str(path))
        return path

    @contextmanager
    def scenario(self, path_name: str, rules: Path, switch: bool, responder: bool = True):
        """Links, switch with `rules`, controller and responder; torn down afterwards."""
        try:
            sc = Scenario(self, path_name, rules, switch, controller=switch)
            if switch:
                links_switch()
                proc = self.lab.spawn("switch", "simple_switch", "-i", "1@sw-p1", "-i", "2@sw-p2",
                                      "-i", "510@sw-cpu", "--thrift-port", THRIFT_PORT,
                                      "build/main.json", stop_signal=signal.SIGTERM)
                wait_port(THRIFT_PORT, 20, proc)
                sc.install = install_rules(rules)
                self.lab.spawn("controller", PY, HERE / "run_controller.py", "-i", "ctl-cpu")
                time.sleep(0.5)
                if self.lab.pid("controller") is None:
                    raise RuntimeError("controller exited; see logs/controller.log")
            else:
                links_direct()
            if responder:
                proc = self.lab.spawn("responder", PY, HERE / "responder.py", "--rules", rules)
                wait_port(CONTROL_PORT, 20, proc)
            yield sc
        finally:
            self.lab.stop_all()
            links_down()

    # T0 -------------------------------------------------------------------

    def environment(self) -> None:
        def run(*cmd):
            try:
                result = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT, timeout=20)
                return (result.stdout + result.stderr).strip()
            except (OSError, subprocess.TimeoutExpired) as error:
                return f"unavailable: {error}"

        cpu = next((line.split(":", 1)[1].strip() for line in
                    Path("/proc/cpuinfo").read_text().splitlines()
                    if line.startswith("model name")), platform.processor())
        env = {
            "date": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "host": platform.node(), "kernel": platform.release(), "cpu": cpu,
            "cpus": os.cpu_count(),
            "memory": run("free", "-h").splitlines()[1:2],
            "python": sys.version.split()[0],
            # -I: the repository's scapy/ directory must not shadow the package.
            "scapy": run(PY, "-I", "-c", "import scapy; print(scapy.VERSION)"),
            "simple_switch": run("simple_switch", "--version"),
            "p4c": run("p4c-bm2-ss", "--version").splitlines()[:1],
            "tcpdump": run("tcpdump", "--version").splitlines()[:1],
            "git_commit": run("git", "rev-parse", "HEAD"),
            "git_branch": run("git", "rev-parse", "--abbrev-ref", "HEAD"),
            "git_dirty": run("git", "status", "--short"),
            "args": {k: str(v) for k, v in vars(self.args).items()},
        }
        (self.out / "env.json").write_text(json.dumps(env, indent=2))
        log(f"T0 environment: {env['cpus']} CPUs, {env['cpu']}, kernel {env['kernel']}")

    # T1 -------------------------------------------------------------------

    def t1_correctness(self) -> None:
        scenarios = [
            ("translate", self.rules("translate"), [], []),
            ("translate-psn-wrap", self.rules("translate"), ["--psn", "3999"],
             ["--psn", "16776215"]),
            ("passthrough", self.rules("passthrough"), [], []),
        ]
        rows, folder = [], self.out / "t1"
        folder.mkdir(exist_ok=True)
        for name, rules, server_args, client_args in scenarios:
            with self.scenario("ctl" if name.startswith("translate") else "fast", rules,
                               switch=True, responder=False):
                proc = self.lab.spawn("server", PY, "scapy/server.py", "--rules", rules,
                                      *server_args)
                wait_port(SERVER_TCP_PORT, 20, proc)
                for rep in range(1, self.args.repeat + 1):
                    reset_counters()
                    result = sh(PY, "scapy/client.py", "--rules", str(rules), *client_args,
                                check=False, timeout=300)
                    counters = read_counters()
                    text = result.stdout + result.stderr
                    (folder / f"{name}-r{rep}.log").write_text(text)
                    (folder / f"{name}-r{rep}.counters.json").write_text(json.dumps(counters))
                    checks = re.findall(r"^\[(PASS|FAIL|SKIP)\] (.+?)(?:  \((.*)\))?$", text, re.M)
                    rows += [(name, rep, i, check, status, note)
                             for i, (status, check, note) in enumerate(checks)]
                    passed = sum(s == "PASS" for s, _, _ in checks)
                    log(f"T1 {name} #{rep}: {passed}/{len(checks)} passed, exit {result.returncode}")
        write_rows(self.out / "correctness.csv",
                   ["scenario", "rep", "index", "check", "result", "note"], rows)

    # T2-T5 ----------------------------------------------------------------

    def per_path(self, tests: set) -> None:
        a = self.args
        for path_name in a.paths:
            mode, switch = PATHS[path_name]
            with self.scenario(path_name, self.rules(mode), switch) as sc:
                log(f"path {path_name}: {mode} rules, switch {'on' if switch else 'off'}")
                if "T2" in tests or "T3" in tests:
                    sizes = a.sizes if "T3" in tests else [a.base_size]
                    for size in sizes:
                        for rep in range(1, a.latency_repeat + 1):
                            sc.bench(self.out / "latency" / f"{path_name}-s{size}-r{rep}",
                                     "latency", {"test": "T2/T3", "size": size, "rep": rep},
                                     "--size", size, "--count", a.latency_count)
                if "T4" in tests and switch:
                    self.t4_capture(sc)
                if "T5" in tests:
                    for rate in a.rates:
                        sc.bench(self.out / "throughput" / f"{path_name}-r{rate}", "throughput",
                                 {"test": "T5", "rate": rate}, "--rate", rate,
                                 "--duration", a.duration, "--size", a.base_size, cpu=True)
                        time.sleep(a.settle)
                    for every in a.ack_every:
                        if every != 1:
                            rate = a.ack_rate
                            sc.bench(self.out / "throughput" / f"{path_name}-r{rate}-ack{every}",
                                     "throughput", {"test": "T5b", "rate": rate, "ack_every": every},
                                     "--rate", rate, "--duration", a.duration, "--ack-every",
                                     every, "--size", a.base_size, cpu=True)
                            time.sleep(a.settle)

    def t4_capture(self, sc: Scenario) -> None:
        if not shutil.which("tcpdump"):
            log("T4 skipped: tcpdump not found")
            return
        folder = self.out / "stages"
        folder.mkdir(exist_ok=True)
        for label, mode, extra in (
                ("latency", "latency", ["--count", self.args.capture_count]),
                ("load", "throughput", ["--rate", self.args.capture_rate, "--duration", "2"])):
            prefix = folder / f"{sc.path}-{label}"
            dumps = []
            for dev in SWITCH_IFACES:
                for direction in ("in", "out"):
                    pcap = f"{prefix}.{dev}.{direction}.pcap"
                    dumps.append(subprocess.Popen(
                        # --immediate-mode: otherwise packets wait in a ring block for up to
                        # a second and the last ones are lost when tcpdump is stopped.
                        ["tcpdump", "-i", dev, "-Q", direction, "-w", pcap, "-s", "128", "-n",
                         "-B", "65536", "-Z", "root", "--time-stamp-precision=nano",
                         "--immediate-mode"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
            time.sleep(1.5)
            try:
                sc.bench(prefix, mode, {"test": "T4", "capture": label}, *extra)
            finally:
                time.sleep(1.5)
                for dump in dumps:
                    dump.send_signal(signal.SIGINT)
                for dump in dumps:
                    dump.wait(10)

    # T7 -------------------------------------------------------------------

    def t7_scale(self) -> None:
        a = self.args
        rows = []
        for k in a.scale:
            rules = self.rules("translate", k)
            with self.scenario("ctl", rules, switch=True, responder=k <= 1024) as sc:
                info = {"connections": k, **sc.install}
                rows.append(info)
                log(f"T7 {k} connections: {info['added']}/{info['rules']} rules in "
                    f"{info['seconds']:.2f}s, table has {info['entries']}, "
                    f"{info['table_full']} TABLE_FULL")
                if k > 1024:
                    continue
                sc.bench(self.out / "scale" / f"k{k}-latency", "latency",
                         {"test": "T7", "connections": k}, "--count", a.scale_count)
                sc.bench(self.out / "scale" / f"k{k}-load", "throughput",
                         {"test": "T7", "connections": k, "rate": a.scale_rate},
                         "--rate", a.scale_rate, "--duration", a.duration, cpu=True)
        (self.out / "scale").mkdir(exist_ok=True)
        (self.out / "scale" / "install.json").write_text(json.dumps(rows, indent=2))


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    ints = lambda text: [int(x) for x in text.split(",")]  # noqa: E731
    parser.add_argument("--out", type=Path,
                        default=ROOT / "result" / "stats" / time.strftime("%Y%m%d-%H%M%S"))
    parser.add_argument("--tests", default=",".join(ALL_TESTS),
                        help="comma-separated subset of %(default)s")
    parser.add_argument("--paths", default="direct,fast,ctl",
                        help="paths for T2-T5: direct, fast (BMv2 only), ctl (BMv2 + controller)")
    parser.add_argument("--quick", action="store_true", help="fewer points and shorter runs")
    parser.add_argument("--no-build", action="store_true", help="use the existing build/main.json")
    parser.add_argument("--no-plot", action="store_true")
    parser.add_argument("--repeat", type=int, help="T1 repetitions per scenario")
    parser.add_argument("--sizes", type=ints, help="T3 payload sizes")
    parser.add_argument("--base-size", type=int, default=64, help="payload for T2/T5")
    parser.add_argument("--latency-count", type=int, help="messages per latency run")
    parser.add_argument("--latency-repeat", type=int, help="latency runs per point")
    parser.add_argument("--rates", type=ints, help="T5 offered rates (Sends/s)")
    parser.add_argument("--duration", type=float, help="seconds per throughput run")
    parser.add_argument("--ack-every", type=ints, default=[1, 8], help="T5b ACK coalescing")
    parser.add_argument("--ack-rate", type=int, help="T5b offered rate")
    parser.add_argument("--settle", type=float, default=2.0, help="pause after a throughput run")
    parser.add_argument("--capture-count", type=int, default=500, help="T4 latency messages")
    parser.add_argument("--capture-rate", type=int, default=2000, help="T4 load rate")
    parser.add_argument("--scale", type=ints, help="T7 connection counts")
    parser.add_argument("--scale-count", type=int, help="T7 latency messages")
    parser.add_argument("--scale-rate", type=int, default=1000,
                        help="T7 offered rate, below saturation")
    args = parser.parse_args(argv)

    # BMv2 saturates around 1-3K Sends/s, so the rates are dense below 5K; the high
    # rates show the collapse and, on the direct path, the generator's own ceiling.
    full = dict(repeat=3, sizes=[1, 16, 64, 128, 256], latency_count=2000, latency_repeat=3,
                rates=[250, 500, 1000, 1500, 2000, 3000, 5000, 10000, 20000, 50000],
                duration=5.0, ack_rate=2000, scale=[1, 16, 64, 256, 1024, 1025],
                scale_count=1000)
    quick = dict(repeat=1, sizes=[1, 64, 256], latency_count=500, latency_repeat=1,
                 rates=[500, 1000, 2000, 5000, 20000], duration=2.0, ack_rate=2000,
                 scale=[1, 64, 1024, 1025], scale_count=300)
    for name, value in (quick if args.quick else full).items():
        if getattr(args, name) is None:
            setattr(args, name, value)
    args.out = args.out.resolve()
    args.tests = {t.strip().upper() for t in args.tests.split(",")}
    args.paths = [p.strip() for p in args.paths.split(",")]
    unknown = (args.tests - set(ALL_TESTS)) | (set(args.paths) - set(PATHS))
    if unknown:
        parser.error(f"unknown tests/paths: {sorted(unknown)}")
    return args


def main(argv=None) -> int:
    args = parse_args(argv)
    if os.geteuid() != 0:
        sys.exit("run as root: sudo python3 stats/suite.py")
    for tool in ("simple_switch", "simple_switch_CLI") + (() if args.no_build else ("p4c-bm2-ss",)):
        if not shutil.which(tool):
            sys.exit(f"{tool} not found in PATH")
    busy = [p for p in (THRIFT_PORT, SERVER_TCP_PORT, CONTROL_PORT) if port_open(p)]
    if busy:
        sys.exit(f"ports {busy} are in use: stop the running switch/server/responder first")

    args.out.mkdir(parents=True, exist_ok=True)
    suite = Suite(args)
    log(f"results in {args.out}")
    status = 0
    try:
        suite.environment()
        if not args.no_build:
            log("building build/main.json")
            sh("make", "-f", "p4/Makefile", "build")
        if "T1" in args.tests:
            suite.t1_correctness()
        if args.tests & {"T2", "T3", "T4", "T5"}:
            suite.per_path(args.tests)
        if "T7" in args.tests:
            suite.t7_scale()
    except KeyboardInterrupt:
        log("interrupted; summarizing what was collected")
        status = 130
    except Exception as error:  # keep the partial results usable
        detail = error.stderr if isinstance(error, subprocess.CalledProcessError) else ""
        log(f"FAILED: {error!r} {detail}; summarizing what was collected")
        status = 1
    finally:
        suite.lab.stop_all()
        links_down()

    log("summarizing")
    result = sh(PY, HERE / "analyze.py", str(args.out), check=False, timeout=1800)
    log(result.stdout.strip() or result.stderr.strip()[-1000:])
    if not args.no_plot:
        result = sh(PY, HERE / "plot.py", str(args.out), check=False, timeout=600)
        log(result.stdout.strip() or result.stderr.strip()[-1000:])
    owner = os.environ.get("SUDO_UID"), os.environ.get("SUDO_GID")
    if all(owner):
        sh("chown", "-R", f"{owner[0]}:{owner[1]}", str(args.out), check=False)
    log(f"done: {args.out}")
    return status


if __name__ == "__main__":
    sys.exit(main())

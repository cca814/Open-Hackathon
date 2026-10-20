#!/usr/bin/env python3
"""Read-only Node B inventory; JSON on stdout is evidence, never a readiness verdict."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re
import subprocess


def probe(argv, timeout=15):
    try:
        result = subprocess.run(argv, capture_output=True, text=True,
                                errors="replace", timeout=timeout, check=False)
        return {"argv": argv, "status": "ok" if result.returncode == 0 else "error",
                "returncode": result.returncode,
                "stdout": result.stdout, "stderr": result.stderr}
    except FileNotFoundError:
        return {"argv": argv, "status": "missing"}
    except subprocess.TimeoutExpired:
        return {"argv": argv, "status": "timeout"}
    except OSError as exc:
        return {"argv": argv, "status": "error", "error": str(exc)}


def read_file(path):
    try:
        return {"path": str(path), "text": path.read_text(errors="replace")}
    except OSError as exc:
        return {"path": str(path), "error": str(exc)}


def sdk_inventory(root):
    """Include matching installed source for subsequent API review, with hashes."""
    records = []
    patterns = ("include/doca_flow*.h", "include/doca_eth*.h",
                "include/doca_buf*.h", "include/doca_mmap.h",
                "include/doca_dev.h", "include/doca_dpdk.h", "include/doca_pe.h", "include/doca_ctx.h", "include/doca_task.h",
                "samples/doca_eth/**/*.c", "samples/doca_eth/**/*.h",
                "samples/doca_eth/**/meson.build", "samples/doca_flow/**/*.c",
                "samples/doca_flow/**/*.h", "samples/doca_flow/**/meson.build",
                "applications/common/dpdk_utils.*", "samples/common.*")
    paths = sorted({p for pattern in patterns for p in root.glob(pattern) if p.is_file()})
    for path in paths:
        try:
            data = path.read_bytes()
            records.append({"path": str(path), "sha256": hashlib.sha256(data).hexdigest(),
                            "text": data.decode(errors="replace")})
        except OSError as exc:
            records.append({"path": str(path), "error": str(exc)})
    return {"root": str(root), "exists": root.is_dir(), "files": records}


def collect(sdk_root, sys_root, interfaces, pci):
    commands = [["uname", "-a"], ["lscpu"], ["lspci", "-Dnnk"],
                ["ip", "-details", "link", "show"], ["rdma", "link", "show"],
                ["ibv_devinfo", "-v"], ["devlink", "dev", "show"],
                ["devlink", "port", "show"], ["devlink", "dev", "info"],
                ["findmnt", "-t", "hugetlbfs"],
                ["pkg-config", "--list-all"],
                ["dpkg-query", "-W", "doca*", "libdpdk*", "mlnx*"],
                ["rpm", "-qa", "*doca*", "*dpdk*", "*mlnx*"]]
    for module in ("doca-flow", "doca-eth", "doca-dpdk-bridge", "doca-common", "libdpdk"):
        for option in ("--modversion", "--cflags", "--libs", "--variable=includedir"):
            commands.append(["pkg-config", option, module])
    for interface in interfaces:
        for option in (None, "-i", "-k", "-l", "-S"):
            commands.append(["ethtool"] + ([option] if option else []) + [interface])
    for address in pci:
        commands.extend([["devlink", "dev", "eswitch", "show", "pci/" + address],
                         ["mlxconfig", "-d", address, "q"]])
    net = []
    for interface in sorted((sys_root / "class/net").glob("*")):
        net.append({"name": interface.name,
                    "device": str((interface / "device").resolve()),
                    "attributes": [read_file(interface / name) for name in
                                   ("address", "phys_port_name", "phys_switch_id", "operstate")]})
    return {"schema_version": 1,
            "collected_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "milestone_1_passed": False,
            "note": "Inventory only. Queue ownership, port mapping and wire RX/TX require target tests.",
            "commands": [probe(command) for command in commands],
            "network": net, "os_release": read_file(Path("/etc/os-release")),
            "memory": read_file(Path("/proc/meminfo")),
            "sdk": sdk_inventory(sdk_root)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-root", type=Path, default=Path("/opt/mellanox/doca"))
    parser.add_argument("--interface", action="append", default=[],
                        help="Verified netdev name; repeat for RX and TX")
    parser.add_argument("--pci", action="append", default=[],
                        help="Verified PCI BDF; repeat for read-only mode queries")
    args = parser.parse_args()
    for name in args.interface:
        if not re.fullmatch(r"[a-zA-Z0-9_][a-zA-Z0-9_.:-]{0,14}", name):
            parser.error("invalid interface name")
    for address in args.pci:
        if not re.fullmatch(r"[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]", address):
            parser.error("PCI address must use domain:bus:device.function format")
    print(json.dumps(collect(args.sdk_root, Path("/sys"), args.interface, args.pci), indent=2))


if __name__ == "__main__":
    main()

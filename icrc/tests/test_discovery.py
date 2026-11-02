import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "discovery", Path(__file__).resolve().parents[1] / "tools/discover_node_b.py")
discovery = importlib.util.module_from_spec(spec)
spec.loader.exec_module(discovery)


class DiscoveryTests(unittest.TestCase):
    def test_probe_distinguishes_missing_timeout_and_failure(self):
        for error, status in [(FileNotFoundError(), "missing"),
                              (subprocess.TimeoutExpired("tool", 1), "timeout")]:
            with patch.object(discovery.subprocess, "run", side_effect=error):
                self.assertEqual(discovery.probe(["tool"])["status"], status)
        result = subprocess.CompletedProcess(["tool"], 3, "", "permission denied")
        with patch.object(discovery.subprocess, "run", return_value=result):
            self.assertEqual(discovery.probe(["tool"])["returncode"], 3)

    def test_inventory_collects_source_and_never_claims_readiness(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            header = root / "include/doca_flow.h"
            header.parent.mkdir()
            header.write_text("/* fixture header */\n")
            with patch.object(discovery, "probe", side_effect=lambda argv: {"argv": argv}):
                report = discovery.collect(root, root, ["p0"], ["0000:03:00.0"])
            self.assertFalse(report["milestone_1_passed"])
            self.assertEqual(report["sdk"]["files"][0]["text"], header.read_text())
            self.assertEqual(len(report["sdk"]["files"][0]["sha256"]), 64)
            commands = [entry["argv"] for entry in report["commands"]]
            self.assertIn(["mlxconfig", "-d", "0000:03:00.0", "q"], commands)
            self.assertIn(["ethtool", "-S", "p0"], commands)
            self.assertEqual(discovery.sdk_inventory(root / "missing")["files"], [])


if __name__ == "__main__":
    unittest.main()

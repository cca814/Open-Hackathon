import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


PATH = Path(__file__).resolve().parents[1] / "tools" / "m4_perftest_config.py"
SPEC = importlib.util.spec_from_file_location("m4_perftest_config", PATH)
CONFIG = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CONFIG)


class M4PerftestConfigTests(unittest.TestCase):
    def test_single_sender_metadata_generates_only_best_effort_stage_a(self):
        with tempfile.TemporaryDirectory() as directory:
            metadata = Path(directory) / "sender.meta"
            output = Path(directory) / "groups.json"
            metadata.write_text("mlx5_2,3645,1050,13909287,9134564\n",
                                encoding="ascii")
            arguments = [str(PATH), "--best-effort", directory, str(output)]
            with mock.patch.object(sys, "argv", arguments), contextlib.redirect_stdout(io.StringIO()):
                CONFIG.main()
            group = json.loads(output.read_text(encoding="ascii"))["groups"][0]
            self.assertEqual(group["match"], {"dst_ipv4": "10.0.2.1",
                                               "dst_qpn": 1050})
            self.assertEqual(group["members"][0]["dst_qpn"], 1050)
            self.assertNotIn("initial_psn", group["match"])
            self.assertNotIn("sender", group)

    def test_reliable_shortcut_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "groups.json"
            arguments = [str(PATH), directory, str(output)]
            with mock.patch.object(sys, "argv", arguments), self.assertRaises(SystemExit):
                CONFIG.main()


if __name__ == "__main__":
    unittest.main()

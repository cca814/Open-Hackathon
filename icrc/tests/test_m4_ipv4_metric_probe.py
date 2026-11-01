from pathlib import Path
import sys
import unittest


TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
import m4_ipv4_metric_probe as PROBE  # noqa: E402
import roce_wire as WIRE  # noqa: E402


class M4IPv4MetricProbeTests(unittest.TestCase):
    def test_positive_ack_has_valid_wire_checksums_and_domain_psn(self):
        frame = PROBE.build_ack(
            "02:7e:6d:fd:79:7c", "02:96:42:f8:5e:f2",
            "192.0.2.1", "192.0.2.10", 100, 200000, 7)
        version, end, _, bth = WIRE.packet_layout(frame)
        self.assertEqual((version, end), (4, len(frame)))
        self.assertEqual(frame[bth], PROBE.RC_ACK_OPCODE)
        self.assertEqual(int.from_bytes(frame[bth + 5:bth + 8], "big"), 100)
        self.assertEqual(int.from_bytes(frame[bth + 9:bth + 12], "big"), 200000)
        self.assertEqual(frame[bth + 12:bth + 16], b"\0" * 4)
        self.assertEqual(WIRE.checksum(frame[14:34]), 0)
        self.assertEqual(WIRE.udp_checksum(frame), 0)
        self.assertEqual(frame[-4:], WIRE.icrc(frame))

    def test_ack_builder_rejects_non_ipv4_and_out_of_range_psn(self):
        arguments = ("02:00:00:00:00:01", "02:00:00:00:00:02",
                     "192.0.2.1", "192.0.2.10", 100)
        with self.assertRaises(ValueError):
            PROBE.build_ack(*arguments, 0x1000000)
        with self.assertRaises(ValueError):
            PROBE.build_ack(arguments[0], arguments[1], "2001:db8::1",
                            "2001:db8::2", 100, 1)


if __name__ == "__main__":
    unittest.main()

import importlib.util
from pathlib import Path
import socket
import struct
import unittest


spec = importlib.util.spec_from_file_location(
    "raw_udp4791", Path(__file__).resolve().parents[1] / "tools/raw_udp4791.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class RawUdp4791Tests(unittest.TestCase):
    def test_frame_has_valid_ipv4_checksum_and_round_trips(self):
        frame = probe.build_frame("02:00:00:00:00:01", "ff:ff:ff:ff:ff:ff",
                                  "192.0.2.1", "192.0.2.2", 49152, 4791,
                                  "unit-test", 7)
        self.assertEqual(frame[:6], b"\xff" * 6)
        self.assertEqual(probe.internet_checksum(frame[14:34]), 0)
        parsed = probe.parse_probe(frame, "unit-test", 4791)
        self.assertEqual(parsed["sequence"], 7)
        self.assertEqual(parsed["length"], len(frame))

    def test_parser_rejects_wrong_port_tag_and_truncation(self):
        frame = probe.build_frame("02:00:00:00:00:01", "ff:ff:ff:ff:ff:ff",
                                  "198.51.100.1", "198.51.100.2", 1234, 4791,
                                  "expected", 1)
        self.assertIsNone(probe.parse_probe(frame, "wrong", 4791))
        self.assertIsNone(probe.parse_probe(frame, "expected", 9999))
        self.assertIsNone(probe.parse_probe(frame[:-1], "expected", 4791))

    def test_frame_network_fields(self):
        frame = probe.build_frame("02:00:00:00:00:01", "02:00:00:00:00:02",
                                  "203.0.113.1", "203.0.113.2", 10000, 4791,
                                  "fields", 0x12345)
        self.assertEqual(socket.inet_ntoa(frame[26:30]), "203.0.113.1")
        self.assertEqual(socket.inet_ntoa(frame[30:34]), "203.0.113.2")
        self.assertEqual(struct.unpack_from("!HH", frame, 34), (10000, 4791))
        self.assertEqual(struct.unpack_from("!H", frame, 18)[0], 0x2345)

    def test_source_port_changes_flow_hash_input(self):
        first = probe.build_frame("02:00:00:00:00:01", "ff:ff:ff:ff:ff:ff",
                                  "192.0.2.1", "192.0.2.2", 49152, 4791,
                                  "flows", 0)
        second = probe.build_frame("02:00:00:00:00:01", "ff:ff:ff:ff:ff:ff",
                                   "192.0.2.1", "192.0.2.2", 49153, 4791,
                                   "flows", 1)
        self.assertEqual(struct.unpack_from("!H", first, 34)[0], 49152)
        self.assertEqual(struct.unpack_from("!H", second, 34)[0], 49153)
        self.assertNotEqual(first, second)


if __name__ == "__main__":
    unittest.main()

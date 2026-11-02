import importlib.util
from pathlib import Path
import unittest


PATH = Path(__file__).resolve().parents[1] / "tools" / "roce_wire.py"
SPEC = importlib.util.spec_from_file_location("roce_wire", PATH)
WIRE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(WIRE)


class RoceWireTests(unittest.TestCase):
    def test_input_and_rewrite_are_independently_valid(self):
        incoming = WIRE.build_frame("02:00:00:00:00:0a", "ff:ff:ff:ff:ff:ff",
                                    "192.0.2.10", "192.0.2.100", 12345,
                                    9000, "oracle", 7, 63)
        self.assertEqual(WIRE.checksum(incoming[14:34]), 0)
        self.assertEqual(WIRE.udp_checksum(incoming), 0)
        self.assertEqual(incoming[-4:], WIRE.icrc(incoming))
        outgoing = WIRE.rewrite_expected(
            incoming, ("02:00:00:00:00:01", "192.0.2.1", 101))
        self.assertEqual(outgoing[:6], bytes.fromhex("020000000001"))
        self.assertEqual(outgoing[30:34], bytes([192, 0, 2, 1]))
        self.assertEqual(outgoing[47:50], (101).to_bytes(3, "big"))
        self.assertEqual(WIRE.checksum(outgoing[14:34]), 0)
        self.assertEqual(WIRE.udp_checksum(outgoing), 0)
        self.assertEqual(outgoing[-4:], WIRE.icrc(outgoing))
        allowed = (set(range(6)) | set(range(24, 26)) | set(range(30, 34)) |
                   {40, 41, 47, 48, 49} |
                   set(range(len(outgoing) - 4, len(outgoing))))
        self.assertTrue(all(a == b or i in allowed
                            for i, (a, b) in enumerate(zip(incoming, outgoing))))

    def test_member_validation(self):
        self.assertEqual(WIRE.parse_member("02:00:00:00:00:01,192.0.2.1,101")[2], 101)
        for value in ("bad", "02:00:00:00:00:01,999.1.1.1,1",
                      "02:00:00:00:00:01,192.0.2.1,16777216"):
            with self.assertRaises(Exception):
                WIRE.parse_member(value)

    def test_ipv6_input_and_rewrite_are_independently_valid(self):
        incoming = WIRE.build_frame("02:00:00:00:00:0a", "ff:ff:ff:ff:ff:ff",
                                    "2001:db8::10", "2001:db8::100", 12345,
                                    9000, "oracle6", 7, 63)
        self.assertEqual(incoming[12:14], b"\x86\xdd")
        self.assertEqual(WIRE.udp_checksum(incoming), 0)
        self.assertEqual(incoming[-4:], WIRE.icrc(incoming))
        outgoing = WIRE.rewrite_expected(
            incoming, ("02:00:00:00:00:01", "2001:db8::1", 101))
        self.assertEqual(outgoing[:6], bytes.fromhex("020000000001"))
        self.assertEqual(outgoing[38:54], bytes.fromhex("20010db8000000000000000000000001"))
        self.assertEqual(outgoing[67:70], (101).to_bytes(3, "big"))
        self.assertEqual(WIRE.udp_checksum(outgoing), 0)
        self.assertEqual(outgoing[-4:], WIRE.icrc(outgoing))
        allowed = (set(range(6)) | set(range(38, 54)) | {60, 61, 67, 68, 69} |
                   set(range(len(outgoing) - 4, len(outgoing))))
        self.assertTrue(all(a == b or i in allowed
                            for i, (a, b) in enumerate(zip(incoming, outgoing))))

    def test_ipv6_member_validation(self):
        self.assertEqual(WIRE.parse_member("02:00:00:00:00:01,2001:db8::1,101")[1],
                         "2001:db8::1")


if __name__ == "__main__":
    unittest.main()

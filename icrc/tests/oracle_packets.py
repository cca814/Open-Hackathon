"""Independent zlib/Internet-checksum oracle; no NIC acceptance is implied."""
import copy
import ctypes as C
import json
from pathlib import Path
import random
import socket
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
LIB, CHECK, FANOUT = (str(Path(p).resolve()) for p in sys.argv[1:4])
sys.argv = sys.argv[:1]
lib = C.CDLL(LIB)

class Address(C.Structure):
    _fields_ = [('family', C.c_uint8), ('bytes', C.c_uint8 * 16)]
class Parsed(C.Structure):
    _fields_ = ([(n, C.c_size_t) for n in ('ip', 'udp', 'bth', 'icrc', 'end')] +
                [('destination', Address), ('dst_qpn', C.c_uint32)])
class Destination(C.Structure):
    _fields_ = [('mac', C.c_uint8 * 6), ('address', Address), ('qpn', C.c_uint32)]
for name in ('roce_parse', 'roce_rewrite'):
    getattr(lib, name).argtypes = [C.c_void_p, C.c_size_t, C.c_void_p]
lib.roce_check_icrc.argtypes = [C.c_void_p, C.c_size_t]

def checksum(data):
    if len(data) % 2: data += b'\0'
    total = sum(struct.unpack('!' + 'H' * (len(data) // 2), data))
    while total >> 16: total = (total & 65535) + (total >> 16)
    return ~total & 65535

def icrc(frame):
    end = 14 + int.from_bytes(frame[16:18], 'big')
    ip = bytearray(frame[14:34]); udp = bytearray(frame[34:42]); bth = bytearray(frame[42:54])
    ip[1] = ip[8] = 255; ip[10:12] = b'\xff\xff'
    udp[6:8] = b'\xff\xff'; bth[4] = 255
    canonical = b'\xff' * 8 + ip + udp + bth + frame[54:end - 4]
    return struct.pack('<I', zlib.crc32(canonical))

def icrc6(frame):
    end = 14 + 40 + int.from_bytes(frame[18:20], 'big')
    ip = bytearray(frame[14:54]); udp = bytearray(frame[54:62]); bth = bytearray(frame[62:74])
    ip[0] = (ip[0] & 0xf0) | 0x0f; ip[1:4] = b'\xff' * 3; ip[7] = 0xff
    udp[6:8] = b'\xff\xff'; bth[4] = 0xff
    canonical = b'\xff' * 8 + ip + udp + bth + frame[74:end - 4]
    return struct.pack('<I', zlib.crc32(canonical))

def udp_checksum(frame):
    end = 14 + int.from_bytes(frame[16:18], 'big')
    pseudo = bytes(frame[26:34]) + b'\0\x11' + struct.pack('!H', end - 34)
    return checksum(pseudo + bytes(frame[34:end]))

def udp_checksum6(frame):
    length = int.from_bytes(frame[58:60], 'big')
    pseudo = bytes(frame[22:54]) + struct.pack('!I3xB', length, 17)
    return checksum(pseudo + bytes(frame[54:54 + length]))

def make_frame(opcode=4, pad=0, payload_size=16, udp=True, seed=0):
    rng = random.Random(seed)
    payload_size += (-payload_size - pad) % 4
    payload = bytes(rng.randrange(256) for _ in range(payload_size))
    extension = b'\x12\x34\x56\x78' if opcode in (3, 5) else b''
    bth = bytes([opcode, pad << 4]) + b'\xff\xff\x80' + (9000).to_bytes(3, 'big') + b'\x80\x01\x02\x03'
    transport = bth + extension + payload + bytes(range(pad)) + b'\0' * 4
    length = 28 + len(transport)
    ip = bytearray(struct.pack('!BBHHHBBHII', 0x45, 0x2a, length, 123, 0x4000, 64, 17, 0, 0xc000020a, 0xc0000264))
    ip[10:12] = checksum(ip).to_bytes(2, 'big')
    frame = bytearray(bytes.fromhex('02000000006402000000000a0800') + ip + struct.pack('!HHHH', 12345, 4791, length - 20, 0) + transport)
    frame[-4:] = icrc(frame)
    if udp: frame[40:42] = (udp_checksum(frame) or 65535).to_bytes(2, 'big')
    return frame

def make_frame6(opcode=4, pad=0, payload_size=16, seed=0):
    rng = random.Random(seed)
    payload_size += (-payload_size - pad) % 4
    payload = bytes(rng.randrange(256) for _ in range(payload_size))
    extension = b'\x12\x34\x56\x78' if opcode in (3, 5) else b''
    bth = bytes([opcode, pad << 4]) + b'\xff\xff\x80' + (9000).to_bytes(3, 'big') + b'\x80\x01\x02\x03'
    transport = bth + extension + payload + bytes(range(pad)) + b'\0' * 4
    udp_length = 8 + len(transport)
    ip = struct.pack('!IHBB16s16s', (6 << 28) | (0x2a << 20) | 0x12345,
                     udp_length, 17, 64,
                     socket.inet_pton(socket.AF_INET6, '2001:db8::10'),
                     socket.inet_pton(socket.AF_INET6, '2001:db8::100'))
    frame = bytearray(bytes.fromhex('02000000006402000000000a86dd') + ip +
                      struct.pack('!HHHH', 12345, 4791, udp_length, 0) + transport)
    frame[-4:] = icrc6(frame)
    frame[60:62] = (udp_checksum6(frame) or 65535).to_bytes(2, 'big')
    return frame

def buffer(frame): return (C.c_uint8 * len(frame)).from_buffer_copy(frame)

def destination(mac, ipv4, qpn):
    packed = bytes(map(int, ipv4.split('.')))
    return Destination((C.c_uint8 * 6)(*mac), Address(4, (C.c_uint8 * 16)(*packed)), qpn)

def destination6(mac, ipv6, qpn):
    packed = socket.inet_pton(socket.AF_INET6, ipv6)
    return Destination((C.c_uint8 * 6)(*mac), Address(6, (C.c_uint8 * 16)(*packed)), qpn)

def rewrite(frame, destination):
    data = buffer(frame)
    status = lib.roce_rewrite(data, len(frame), C.byref(destination))
    return status, bytes(data)

class PacketTests(unittest.TestCase):
    def test_all_send_opcodes_padding_and_random_payloads(self):
        rng = random.Random(1234)
        for i in range(400):
            op = i % 6; pad = 0 if op < 2 else rng.randrange(4)
            frame = make_frame(op, pad, rng.randrange(0, 1300), bool(i % 2), i)
            self.assertEqual(lib.roce_check_icrc(buffer(frame), len(frame)), 0)
            ipv4 = '.'.join(str(rng.randrange(256)) for _ in range(4))
            dst = destination((2, 0, 0, 0, 0, 1), ipv4, rng.randrange(1 << 24))
            result, out = rewrite(frame, dst)
            self.assertEqual(result, 0)
            self.assertEqual(out[47:50], dst.qpn.to_bytes(3, 'big'))
            self.assertEqual(out[30:34], bytes(dst.address.bytes[:4]))
            self.assertEqual(out[:6], bytes(dst.mac))
            self.assertEqual(out[-4:], icrc(out))
            self.assertEqual(checksum(out[14:34]), 0)
            if i % 2: self.assertEqual(udp_checksum(out), 0)
            else: self.assertEqual(out[40:42], b'\0\0')
            allowed = set(range(6)) | set(range(24, 26)) | set(range(30, 34)) | {40, 41, 47, 48, 49} | set(range(len(out) - 4, len(out)))
            self.assertTrue(all(a == b or j in allowed for j, (a, b) in enumerate(zip(frame, out))))

    def test_qpn_only_and_ethernet_padding(self):
        frame = make_frame(payload_size=0, udp=False)
        frame += b'\xaa\xbb'  # 58-byte IPv4 SEND frame padded to 60 bytes.
        dst = destination(tuple(frame[:6]), '192.0.2.100', 0xffffff)
        status, out = rewrite(frame, dst)
        self.assertEqual(status, 0)
        self.assertEqual(out[-2:], b'\xaa\xbb')
        self.assertEqual(out[54:58], icrc(out))
        self.assertEqual(lib.roce_check_icrc(buffer(out), len(out)), 0)
        dst.qpn = 0x1000000
        status, unchanged = rewrite(frame, dst)
        self.assertNotEqual(status, 0); self.assertEqual(unchanged, frame)

    def test_ipv6_send_rewrite_and_checksums(self):
        for opcode in range(6):
            pad = 0 if opcode < 2 else opcode % 4
            frame = make_frame6(opcode, pad, 31 + opcode, 100 + opcode)
            self.assertEqual(lib.roce_check_icrc(buffer(frame), len(frame)), 0)
            dst = destination6((2, 0, 0, 0, 0, 1), '2001:db8::1', 0xabcdef)
            status, out = rewrite(frame, dst)
            self.assertEqual(status, 0)
            self.assertEqual(out[38:54], socket.inet_pton(socket.AF_INET6, '2001:db8::1'))
            self.assertEqual(out[67:70], b'\xab\xcd\xef')
            self.assertEqual(out[-4:], icrc6(out))
            self.assertEqual(udp_checksum6(out), 0)
            allowed = set(range(6)) | set(range(38, 54)) | {60, 61, 67, 68, 69} | set(range(len(out) - 4, len(out)))
            self.assertTrue(all(a == b or i in allowed for i, (a, b) in enumerate(zip(frame, out))))

        wrong_family = destination((2, 0, 0, 0, 0, 1), '192.0.2.1', 1)
        status, unchanged = rewrite(frame, wrong_family)
        self.assertNotEqual(status, 0)
        self.assertEqual(unchanged, frame)

    def test_truncation_and_malformed_headers(self):
        frame = make_frame(opcode=5, pad=3)
        for n in range(len(frame)):
            self.assertNotEqual(lib.roce_parse(buffer(frame[:n]), n, C.byref(Parsed())), 0)
        for offset, value in [(12, 0x81), (14, 0x46), (20, 0x20), (23, 6), (36, 0), (39, 1), (42, 0x11), (43, 1), (16, 255)]:
            bad = frame.copy(); bad[offset] = value
            self.assertNotEqual(lib.roce_parse(buffer(bad), len(bad), C.byref(Parsed())), 0)
        minimal = make_frame(payload_size=0, udp=False)
        for opcode, flags in ((5, 0), (4, 0x30), (0, 0x10)):
            bad = minimal.copy(); bad[42] = opcode; bad[43] = flags
            self.assertNotEqual(lib.roce_parse(buffer(bad), len(bad), C.byref(Parsed())), 0)
        for offset in (26, 47, 54):
            bad = frame.copy(); bad[offset] ^= 1
            self.assertEqual(lib.roce_check_icrc(buffer(bad), len(bad)), 3)
        for offset in (15, 22, 24, 40, 46):
            masked = frame.copy(); masked[offset] ^= 1
            self.assertEqual(lib.roce_check_icrc(buffer(masked), len(masked)), 0)

class GroupTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / 'groups.json'
        self.config = json.loads((ROOT / 'config/groups.example.json').read_text())
    def tearDown(self): self.tmp.cleanup()
    def check(self, config, expected):
        self.path.write_text(config if isinstance(config, str) else json.dumps(config))
        result = subprocess.run([CHECK, str(self.path)], capture_output=True)
        self.assertEqual(result.returncode == 0, expected, result.stderr.decode())
    def test_valid_and_invalid_files(self):
        self.check(self.config, True)
        for qpn in (-1, 16777216, 1.5, True, '101', None):
            bad = copy.deepcopy(self.config); bad['groups'][0]['members'][0]['dst_qpn'] = qpn
            self.check(bad, False)
        for edit in ('empty', 'duplicate_id', 'duplicate_match', 'duplicate_member', 'too_many', 'ip', 'mac', 'unknown', 'missing'):
            bad = copy.deepcopy(self.config); g = bad['groups'][0]
            if edit == 'empty': g['members'] = []
            elif edit == 'duplicate_id':
                bad['groups'].append(copy.deepcopy(g)); bad['groups'][1]['match']['dst_qpn'] = 1
            elif edit == 'duplicate_match':
                bad['groups'].append(copy.deepcopy(g)); bad['groups'][1]['id'] = 'other'
            elif edit == 'duplicate_member': g['members'].append(g['members'][0])
            elif edit == 'too_many': g['members'] *= 9
            elif edit == 'ip': g['match']['dst_ipv4'] = '999.1.1.1'
            elif edit == 'mac': g['members'][0]['dst_mac'] = '2:00:00:00:00:01'
            elif edit == 'unknown': g['bogus'] = 1
            elif edit == 'missing': del g['match']
            self.check(bad, False)
        text = json.dumps(self.config)
        for bad in ('', '{}', '{"groups":[]}', text + 'x', text + '\0', text.replace('"dst_qpn": 101', '"dst_qpn": 101, "dst_qpn": 102'), text.replace('9000', '09000')):
            self.check(bad, False)
        many = copy.deepcopy(self.config)
        many['groups'][0]['members'] = [dict(self.config['groups'][0]['members'][0], dst_qpn=i) for i in range(17)]
        self.check(many, False)
        many['groups'][0]['members'].pop()
        self.check(many, True)
        many['groups'] = [dict(copy.deepcopy(many['groups'][0]), id=str(i), match={'dst_ipv4': '192.0.2.100', 'dst_qpn': i}) for i in range(65)]
        self.check(many, False)
        many['groups'].pop()
        self.check(many, True)
        ipv6 = copy.deepcopy(self.config)
        ipv6['groups'][0]['match'] = {'dst_ipv6': '2001:db8::100', 'dst_qpn': 9000}
        for i, member in enumerate(ipv6['groups'][0]['members'], 1):
            member['dst_ipv6'] = f'2001:db8::{i}'
            del member['dst_ipv4']
        self.check(ipv6, True)
        mixed = copy.deepcopy(ipv6)
        mixed['groups'][0]['members'][0]['dst_ipv4'] = '192.0.2.1'
        del mixed['groups'][0]['members'][0]['dst_ipv6']
        self.check(mixed, False)
        bad6 = copy.deepcopy(ipv6)
        bad6['groups'][0]['match']['dst_ipv6'] = '2001:db8:::1'
        self.check(bad6, False)
        for n in range(len(text)):
            self.check(text[:n], False)
    def test_worker_fanout_wire_and_pressure(self):
        self.check(self.config, True)
        frame = make_frame(opcode=5, pad=3, payload_size=127, udp=True)
        incoming = Path(self.tmp.name) / 'input.bin'; outgoing = Path(self.tmp.name) / 'output.bin'
        incoming.write_bytes(frame)
        subprocess.run([FANOUT, str(self.path), str(incoming), str(outgoing)], check=True)
        raw = outgoing.read_bytes(); self.assertEqual(len(raw), 2 * len(frame))
        for i in range(2):
            out = raw[i * len(frame):(i + 1) * len(frame)]
            self.assertEqual(out[47:50], (101 + i).to_bytes(3, 'big'))
            self.assertEqual(out[30:34], (0xc0000201 + i).to_bytes(4, 'big'))
            self.assertEqual(out[-4:], icrc(out)); self.assertEqual(udp_checksum(out), 0)
            self.assertEqual(checksum(out[14:34]), 0)

if __name__ == '__main__': unittest.main()

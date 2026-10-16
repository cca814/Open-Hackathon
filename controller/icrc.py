"""RoCEv2 ICRC, as in rdma_packet_format.md section 4 and Linux rxe_icrc.c.

ICRC = CRC-32 (IEEE 802.3, i.e. zlib.crc32) over
    0xFF x 8 | IPv4 | UDP | BTH | rest of packet up to the ICRC
with IPv4 TOS/TTL/checksum, UDP checksum and BTH resv8a set to all ones.
"""

import struct
import zlib

ETH_LEN = 14
UDP_LEN = 8
BTH_LEN = 12
ICRC_LEN = 4
ROCE_UDP_DPORT = 4791

ICRC_LITTLE = "little"  # rxe on x86
ICRC_BIG = "big"


def _ip_header_len(frame, eth_len=ETH_LEN):
    return (frame[eth_len] & 0x0F) * 4


def build_icrc_input(frame, eth_len=ETH_LEN):
    """Masked bytes fed to the CRC. `frame` starts at Ethernet and ends with the ICRC."""
    ip_off = eth_len
    ip_len = _ip_header_len(frame, eth_len)
    udp_off = ip_off + ip_len
    bth_off = udp_off + UDP_LEN

    ip = bytearray(frame[ip_off:ip_off + ip_len])
    ip[1] = 0xFF             # TOS
    ip[8] = 0xFF             # TTL
    ip[10:12] = b"\xff\xff"  # header checksum

    udp = bytearray(frame[udp_off:udp_off + UDP_LEN])
    udp[6:8] = b"\xff\xff"   # checksum

    bth = bytearray(frame[bth_off:bth_off + BTH_LEN])
    bth[4] = 0xFF            # FECN/BECN/resv6

    rest = frame[bth_off + BTH_LEN:-ICRC_LEN]
    return b"\xff" * 8 + bytes(ip) + bytes(udp) + bytes(bth) + bytes(rest)


def compute_icrc(frame, eth_len=ETH_LEN, byte_order=ICRC_LITTLE):
    """The 4 ICRC bytes for `frame`, ready to write at its end."""
    crc = zlib.crc32(build_icrc_input(frame, eth_len)) & 0xFFFFFFFF
    return struct.pack("<I" if byte_order == ICRC_LITTLE else ">I", crc)


def rewrite_icrc(frame, eth_len=ETH_LEN, byte_order=ICRC_LITTLE):
    out = bytearray(frame)
    out[-ICRC_LEN:] = compute_icrc(frame, eth_len, byte_order)
    return bytes(out)


def is_rocev2(frame, eth_len=ETH_LEN):
    """IPv4 / UDP / dst port 4791."""
    if len(frame) < eth_len + 20 + UDP_LEN + BTH_LEN + ICRC_LEN:
        return False
    if frame[12:14] != b"\x08\x00":
        return False
    ip_off = eth_len
    if (frame[ip_off] >> 4) != 4 or frame[ip_off + 9] != 17:
        return False
    udp_off = ip_off + _ip_header_len(frame, eth_len)
    return struct.unpack("!H", frame[udp_off + 2:udp_off + 4])[0] == ROCE_UDP_DPORT


def verify_icrc(frame, eth_len=ETH_LEN):
    """(little_ok, big_ok): whether the stored ICRC matches in either byte order."""
    stored = bytes(frame[-ICRC_LEN:])
    return (compute_icrc(frame, eth_len, ICRC_LITTLE) == stored,
            compute_icrc(frame, eth_len, ICRC_BIG) == stored)


if __name__ == "__main__":
    assert zlib.crc32(b"123456789") & 0xFFFFFFFF == 0xCBF43926, "CRC-32 engine mismatch"

    sample = bytearray(64)
    sample[12:14] = b"\x08\x00"
    sample[14] = 0x45
    sample[14 + 9] = 17
    sample[34 + 2:34 + 4] = struct.pack("!H", ROCE_UDP_DPORT)
    fixed = rewrite_icrc(bytes(sample))
    assert rewrite_icrc(fixed)[-4:] == fixed[-4:], "ICRC not idempotent"
    assert is_rocev2(fixed), "sample should be detected as RoCEv2"
    print("icrc self-test OK:", fixed[-4:].hex())

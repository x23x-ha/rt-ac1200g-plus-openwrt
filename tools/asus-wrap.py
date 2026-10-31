#!/usr/bin/env python3
"""Add an ASUS product-ID tail to an OpenWrt TRX so stock Asuswrt accepts it.

Same result as OpenWrt's asustrx tool, except that if the TRX doesn't end in
64 empty bytes (OpenWrt's serial-flash TRX ends with a jffs2 marker), 64 bytes
are appended and the TRX length is updated, instead of failing.

Usage: asus-wrap.py input.trx output.trx [PRODUCTID]
"""
import struct
import sys
import zlib

TRX_MAGIC = 0x30524448
TAIL_LEN = 64


def wrap(data: bytes, productid: str) -> bytes:
    magic, length, _crc = struct.unpack_from("<III", data, 0)
    if magic != TRX_MAGIC:
        sys.exit("not a TRX image (bad magic)")
    if length != len(data):
        sys.exit(f"TRX length field {length} != file size {len(data)}")
    pid = productid.encode()
    if len(pid) > 12:
        sys.exit("product ID longer than 12 bytes")

    buf = bytearray(data)
    if any(buf[-TAIL_LEN:]):
        buf += bytes(TAIL_LEN)
    tail = bytes(4) + pid.ljust(12, b"\0") + bytes(48)
    buf[-TAIL_LEN:] = tail

    struct.pack_into("<I", buf, 4, len(buf))
    crc = zlib.crc32(bytes(buf[12:])) ^ 0xFFFFFFFF
    struct.pack_into("<I", buf, 8, crc)
    return bytes(buf)


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    productid = sys.argv[3] if len(sys.argv) == 4 else "RT-AC1200G+"
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    out = wrap(data, productid)
    with open(sys.argv[2], "wb") as f:
        f.write(out)
    print(f"{sys.argv[2]}: {len(out)} bytes, product ID {productid}")


if __name__ == "__main__":
    main()

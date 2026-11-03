#!/usr/bin/env python3
"""Turn an OpenWrt bcm53xx *Tenda AC9* TRX into one that works on the ASUS RT-AC1200G+.

Why: the stock bcm53573.dtsi wires switch port 5 -> gmac1 and port 8 -> gmac0,
and Linux DSA picks port 5 (eth1) as CPU port. On the RT-AC1200G+ only port 8
(gmac0/eth0) is wired, like the stock ASUS firmware (et0, vlan ports "... 8*").
Result without this patch: switch links up, but no Ethernet traffic at all.

DT changes (nothing else is touched):
  .../switch@1e/ports/port@5 : status = "disabled"
  .../switch@1e/ports/port@8 : fixed-link { speed = <1000>; full-duplex; }
Board changes (GPIOs from the ASUS nvram; +4096 there = active low):
  model = "ASUS RT-AC1200G+"  (compatible stays "tenda,ac9": board_name, sysupgrade)
  leds-0 : power LED GPIO 10 (low, = WPS LED), USB LED GPIO 15 (low, usbport on
           USB ports 1+2); drops the Tenda's GPIO 1/11 LEDs and the GPIO 15 blinker
  leds-1 : dropped (radio GPIO 3 is the 2.4 GHz LED, b43 drives it via 0:ledbh3)
  gpio-keys : reset GPIO 5 (low), WPS GPIO 9 (low); drops the Tenda's GPIO 3/7
  usb2@4000 : vcc-gpios = GPIO 8 (high), USB port power, read by bcma-hcd
  aliases : led-boot/-failsafe/-running/-upgrade = power LED

Input: the Tenda AC9 TRX, or an image already carrying only the port fix (the
port fix is then skipped).

How: TRX = lzma(zImage + appended DTB) at offset[0], squashfs rootfs at offset[1].
The kernel blob is decompressed, the DTB patched, re-compressed with the same
LZMA1 properties as OpenWrt's lzma-d16 (header with known size, no end marker),
and everything from the rootfs offset on is kept byte-for-byte. Fails if the new
kernel would no longer fit before the original rootfs offset.

Usage: patch-trx-dtb.py input.trx output.trx
       patch-trx-dtb.py --zimage NEW_ZIMAGE input.trx output.trx
Needs: python3, `pip install fdt`, liblzma >= 5.4 (Linux: liblzma.so.5;
macOS: Homebrew xz). Verifies its own output before exiting.
"""
import argparse
import ctypes
import ctypes.util
import hashlib
import lzma
import struct
import sys
import zlib

import fdt

TRX_MAGIC = 0x30524448
SW = "/axi@18000000/ethernet@5000/mdio/switch@1e/ports"
USB = "/axi@18000000/usb2@4000"
MODEL = "ASUS RT-AC1200G+"
ACTIVE_LOW = 1


# ---------------------------------------------------------------- liblzma (LZMA1, no EOPM)
class LzmaOptions(ctypes.Structure):
    _fields_ = [
        ("dict_size", ctypes.c_uint32), ("preset_dict", ctypes.c_void_p),
        ("preset_dict_size", ctypes.c_uint32), ("lc", ctypes.c_uint32),
        ("lp", ctypes.c_uint32), ("pb", ctypes.c_uint32), ("mode", ctypes.c_int),
        ("nice_len", ctypes.c_uint32), ("mf", ctypes.c_int), ("depth", ctypes.c_uint32),
        ("ext_flags", ctypes.c_uint32), ("ext_size_low", ctypes.c_uint32),
        ("ext_size_high", ctypes.c_uint32), ("reserved_int4", ctypes.c_uint32),
        ("reserved_enum1", ctypes.c_int), ("reserved_enum2", ctypes.c_int),
        ("reserved_enum3", ctypes.c_int), ("reserved_enum4", ctypes.c_int),
        ("reserved_ptr1", ctypes.c_void_p), ("reserved_ptr2", ctypes.c_void_p),
    ]


class LzmaFilter(ctypes.Structure):
    _fields_ = [("id", ctypes.c_uint64), ("options", ctypes.c_void_p)]


LZMA_FILTER_LZMA1EXT = 0x4000000000000002
LZMA_VLI_UNKNOWN = 0xFFFFFFFFFFFFFFFF


def load_liblzma():
    names = ["liblzma.so.5", ctypes.util.find_library("lzma"),
             "/opt/homebrew/opt/xz/lib/liblzma.5.dylib",
             "/usr/local/opt/xz/lib/liblzma.5.dylib", "liblzma.5.dylib"]
    for n in filter(None, names):
        try:
            lib = ctypes.CDLL(n)
        except OSError:
            continue
        lib.lzma_version_number.restype = ctypes.c_uint32
        if lib.lzma_version_number() >= 50040002:  # 5.4.0: LZMA_FILTER_LZMA1EXT
            return lib
    sys.exit("need liblzma >= 5.4 (Linux: xz-utils; macOS: brew install xz)")


def lzma_alone(data: bytes, lc: int, lp: int, pb: int, dict_size: int) -> bytes:
    lib = load_liblzma()
    opts = LzmaOptions()
    if lib.lzma_lzma_preset(ctypes.byref(opts), ctypes.c_uint32(9)):
        sys.exit("lzma_lzma_preset failed")
    opts.dict_size, opts.lc, opts.lp, opts.pb = dict_size, lc, lp, pb
    opts.ext_flags = 0
    opts.ext_size_low = len(data) & 0xFFFFFFFF      # known size -> no end marker
    opts.ext_size_high = len(data) >> 32
    filters = (LzmaFilter * 2)(LzmaFilter(LZMA_FILTER_LZMA1EXT, ctypes.addressof(opts)),
                               LzmaFilter(LZMA_VLI_UNKNOWN, None))
    out_size = len(data) + len(data) // 2 + 65536
    out = ctypes.create_string_buffer(out_size)
    out_pos = ctypes.c_size_t(0)
    ret = lib.lzma_raw_buffer_encode(filters, None, data, ctypes.c_size_t(len(data)),
                                     out, ctypes.byref(out_pos), ctypes.c_size_t(out_size))
    if ret != 0:
        sys.exit(f"lzma_raw_buffer_encode failed: {ret}")
    props = (pb * 5 + lp) * 9 + lc
    return struct.pack("<BIQ", props, dict_size, len(data)) + out.raw[:out_pos.value]


# ---------------------------------------------------------------- TRX helpers
def parse_trx(d: bytes):
    magic, length, crc, _fl, ver, o0, o1, o2 = struct.unpack_from("<IIIHH3I", d, 0)
    if magic != TRX_MAGIC:
        sys.exit("not a TRX image")
    if length != len(d):
        sys.exit(f"TRX length {length} != file size {len(d)} (truncated or not a plain TRX?)")
    if crc != zlib.crc32(d[12:]) ^ 0xFFFFFFFF:
        sys.exit("TRX CRC mismatch")
    if o2 != 0:
        sys.exit("unexpected third TRX partition")
    return ver, o0, o1


def split_kernel(k: bytes):
    props, dict_size = k[0], struct.unpack_from("<I", k, 1)[0]
    lc, lp, pb = props % 9, (props // 9) % 5, props // 45
    raw = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(k)
    zstart, zend = struct.unpack_from("<II", raw, 0x28)
    zlen = zend - zstart
    if struct.unpack_from(">I", raw, zlen)[0] != 0xD00DFEED:
        sys.exit("no appended DTB after the zImage")
    return raw[:zlen], raw[zlen:], (lc, lp, pb, dict_size)


def patch_ports(t):
    p5, p8 = t.get_node(f"{SW}/port@5"), t.get_node(f"{SW}/port@8")
    if p5.exist_property("status") or any(n.name == "fixed-link" for n in p8.nodes):
        print("port fix already present, skipped")
        return
    p5.set_property("status", "disabled")
    fl = fdt.Node("fixed-link")
    fl.append(fdt.PropWords("speed", 1000))
    fl.append(fdt.Property("full-duplex"))
    p8.append(fl)


def gpio_led(name, label, cc, gpio, flags, **extra):
    n = fdt.Node(name)
    n.append(fdt.PropStrings("label", label))
    n.append(fdt.PropWords("gpios", cc, gpio, flags))
    for k, v in extra.items():
        k = k.replace("_", "-").replace("linux-", "linux,")
        n.append(fdt.PropWords(k, *v) if isinstance(v, tuple) else fdt.PropStrings(k, v))
    return n


def gpio_key(name, label, code, cc, gpio, flags):
    n = fdt.Node(name)
    n.append(fdt.PropStrings("label", label))
    n.append(fdt.PropWords("linux,code", code))
    n.append(fdt.PropWords("gpios", cc, gpio, flags))
    return n


def patch_board(t):
    cc = t.get_node("/axi@18000000/chipcommon@0").get_property("phandle").value
    t.set_property("model", MODEL)

    # USB port power; trigger sources: port 1 and 2 of EHCI and OHCI
    t.get_node(USB).append(fdt.PropWords("vcc-gpios", cc, 8, 0))
    next_ph = 1 + max(p.value for _, _, props in t.walk()
                      for p in props if p.name == "phandle")
    usb_ports = []
    for hcd in ("usb@4000", "usb@d000"):
        for port in ("port@1", "port@2"):
            pn = t.get_node(f"{USB}/{hcd}/{port}")
            if not pn.exist_property("phandle"):
                pn.append(fdt.PropWords("phandle", next_ph))
                next_ph += 1
            usb_ports.append(pn.get_property("phandle").value)

    root = t.get_node("/")
    for name in ("leds-0", "leds-1", "gpio-keys"):
        root.remove_subnode(name)

    leds = fdt.Node("leds")
    leds.append(fdt.PropStrings("compatible", "gpio-leds"))
    leds.append(gpio_led("led-power", "bcm53xx:blue:power", cc, 10, ACTIVE_LOW,
                         default_state="on"))
    leds.append(gpio_led("led-usb", "bcm53xx:blue:usb", cc, 15, ACTIVE_LOW,
                         trigger_sources=tuple(usb_ports), linux_default_trigger="usbport"))
    root.append(leds)

    keys = fdt.Node("gpio-keys")
    keys.append(fdt.PropStrings("compatible", "gpio-keys"))
    keys.append(gpio_key("button-restart", "Reset", 0x198, cc, 5, ACTIVE_LOW))   # KEY_RESTART
    keys.append(gpio_key("button-wps", "WPS", 0x211, cc, 9, ACTIVE_LOW))         # KEY_WPS_BUTTON
    root.append(keys)

    aliases = t.get_node("/aliases")
    for a in ("led-boot", "led-failsafe", "led-running", "led-upgrade"):
        aliases.append(fdt.PropStrings(a, "/leds/led-power"))


def main():
    if len(sys.argv) == 3:
        args = argparse.Namespace(zimage=None, src=sys.argv[1], dst=sys.argv[2])
    elif len(sys.argv) == 5 and sys.argv[1] == "--zimage":
        args = argparse.Namespace(zimage=sys.argv[2], src=sys.argv[3], dst=sys.argv[4])
    else:
        sys.exit(__doc__)
    d = open(args.src, "rb").read()
    ver, o0, o1 = parse_trx(d)
    zimage, dtb, (lc, lp, pb, dict_size) = split_kernel(d[o0:o1])

    # Use provided zImage if --zimage was given
    if args.zimage:
        zimage = open(args.zimage, "rb").read()
        print(f"Replaced zImage: {len(zimage)} bytes (from {args.zimage})")

    t = fdt.parse_dtb(dtb)
    model = t.get_property("model").value
    if model == MODEL:
        # DTB already patched — only zImage replacement needed
        if args.zimage:
            print("DTB already patched, skipping DTB patch (zImage replacement only)")
            new_dtb = dtb
        else:
            sys.exit("DTB already patched")
    elif model != "Tenda AC9":
        sys.exit(f"input DTB model is {model!r}, expected 'Tenda AC9'")
    else:
        patch_ports(t)
        patch_board(t)
        new_dtb = t.to_dtb(version=17)

    new_k = lzma_alone(zimage + new_dtb, lc, lp, pb, dict_size)
    kpad = (-(28 + len(new_k))) % 1024
    if 28 + len(new_k) + kpad > o1:
        sys.exit(f"patched kernel no longer fits before rootfs at {o1:#x}")
    body = new_k + bytes(o1 - 28 - len(new_k)) + d[o1:]
    img = bytearray(struct.pack("<IIIHH3I", TRX_MAGIC, 28 + len(body), 0, 0, ver, 28, o1, 0) + body)
    struct.pack_into("<I", img, 8, zlib.crc32(bytes(img[12:])) ^ 0xFFFFFFFF)

    # self-check: valid TRX, rootfs untouched, kernel decompresses to zImage + patched DTB
    img = bytes(img)
    parse_trx(img)
    z2, dtb2, _ = split_kernel(img[28:o1])
    assert z2 == zimage and dtb2 == new_dtb and img[o1:] == d[o1:]
    t2 = fdt.parse_dtb(dtb2)
    assert t2.get_node(f"{SW}/port@5").get_property("status").value == "disabled"
    assert t2.get_property("model").value == MODEL
    assert t2.get_property("compatible").data[0] == "tenda,ac9"
    assert t2.get_node(USB).get_property("vcc-gpios").data[1:] == [8, 0]

    open(args.dst, "wb").write(img)
    print(f"{args.dst}: {len(img)} bytes, sha256 {hashlib.sha256(img).hexdigest()}")


if __name__ == "__main__":
    main()

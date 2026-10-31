#!/usr/bin/env python3
"""Extract the 2.4 GHz EPA TX gain table for BCM43217 (N-PHY rev 17,
radio 2057 rev 14) from the stock RT-AC1200G+ wl.ko and print it as C.

Usage: wl-extract-epa-table.py wl.ko > table.inc      (pip install pyelftools)

wl.ko comes from the stock rootfs (TRX at 0x40000 in fullflash.bin, squashfs
at TRX+0x1a2d90): lib/modules/2.6.36.4brcmarm/kernel/drivers/net/wl.ko,
sha256 02c16a42ad4e1791c9d40e60c009239227664b5f218c4c1615a3d0418468ed8f.

How the table was located: wlc_phy_get_epa_gaintbl_nphy(), 2 GHz branch,
phy_rev == 17, chip 0xa8d1 (43217) or 0xa87b -> .data+0x273c4; every other
rev-17 chip -> .data+0x271c4. The script re-checks that literal and checks
that tables known from b43/brcmsmac sit where expected, so a wrong offset
fails loudly instead of producing a plausible-looking table.
"""
import struct
import sys

from elftools.elf.elffile import ELFFile

SELECT_FN = "wlc_phy_get_epa_gaintbl_nphy"
SELECT_LIT = 0x3A0  # literal-pool word inside SELECT_FN holding the 43217 table
TABLE = 0x273C4
KNOWN = {  # .data offset -> first words of a table also in b43/brcmsmac
    0x22EA0: (0x50DF002E, 0x50CF002D),  # b43 tx_gain_ipa_2057_rev14_2g
    0x26FC4: (0x80F90040, 0x80E10040),  # brcmsmac txgain_epa_2057rev3
    0x291C4: (0x10F90040, 0x10E10040),  # brcmsmac txgain_epa_2057rev5
    0x27BC4: (0x1F410044, 0x1F410042),  # b43 tx_gain_epa_rev3_2g
}


def main(path):
    raw = open(path, "rb").read()
    elf = ELFFile(open(path, "rb"))
    data = elf.get_section_by_name(".data")
    text = elf.get_section_by_name(".text")

    def word(sec, off):
        return struct.unpack_from("<I", raw, sec["sh_offset"] + off)[0]

    for off, first in KNOWN.items():
        got = (word(data, off), word(data, off + 4))
        if got != first:
            sys.exit(f"layout check failed at .data+{off:#x}: {got}")

    fn = next(s for s in elf.get_section_by_name(".symtab").iter_symbols()
              if s.name == SELECT_FN)
    lit = word(text, (fn["st_value"] & ~1) + SELECT_LIT)
    if lit != TABLE:
        sys.exit(f"{SELECT_FN} literal is {lit:#x}, expected {TABLE:#x}")

    tbl = [word(data, TABLE + 4 * i) for i in range(128)]
    print("/* BCM43217 (N-PHY rev 17, radio 2057 rev 14) 2 GHz EPA TX gain table.")
    print(" * From the ASUS RT-AC1200G+ stock wl.ko (Asuswrt 3.0.0.4.382_52272). */")
    print("static const u32 b43_ntab_tx_gain_epa_2057_rev14_2g[] = {")
    for i in range(0, 128, 4):
        print("\t" + ", ".join(f"0x{w:08x}" for w in tbl[i:i + 4]) + ",")
    print("};")


if __name__ == "__main__":
    main(sys.argv[1])

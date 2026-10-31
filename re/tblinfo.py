# Dump a wl phytbl_info[] array: {ptr, len, id, offset, width}, resolving ptr relocations.
import sys, struct
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
fn, secname, off, n = sys.argv[1], sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4])
raw = open(fn, 'rb').read(); e = ELFFile(open(fn, 'rb'))
secs = list(e.iter_sections()); sym = e.get_section_by_name('.symtab')
sec = e.get_section_by_name(secname); si = secs.index(sec)
rel = {}
for rs in e.iter_sections():
    if isinstance(rs, RelocationSection) and rs['sh_info'] == si:
        for r in rs.iter_relocations():
            s = sym.get_symbol(r['r_info_sym'])
            nm = s.name or secs[s['st_shndx']].name
            rel[r['r_offset']] = (nm, s)
objs = {}
for s in sym.iter_symbols():
    if s.entry['st_info']['type'] == 'STT_OBJECT' and isinstance(s['st_shndx'], int):
        objs[(secs[s['st_shndx']].name, s['st_value'])] = s.name
for i in range(n):
    o = off + 20 * i
    ptr, ln, tid, toff, w = struct.unpack_from('<5I', raw, sec['sh_offset'] + o)
    nm, s = rel.get(o, ('?', None))
    tgt = nm
    if s is not None and s.name == '' and isinstance(s['st_shndx'], int):
        tgt = f"{nm}+{ptr:#x}" + (f" ({objs.get((nm, ptr), '')})" if objs.get((nm, ptr)) else '')
    print(f"[{i:2d}] id {tid:3d} off {toff:#06x} len {ln:4d} width {w:2d}  data {tgt}")

# Find instructions using given immediates (e.g. chip ids) in .text; print containing function.
import sys, bisect
from elftools.elf.elffile import ELFFile
import capstone
fn = sys.argv[1]; imms = [int(x, 16) for x in sys.argv[2:]]
raw = open(fn,'rb').read(); e = ELFFile(open(fn,'rb'))
secs = list(e.iter_sections()); sym = e.get_section_by_name('.symtab')
text = e.get_section_by_name('.text'); ti = secs.index(text); base = text['sh_offset']
funcs = sorted((s['st_value'] & ~1, s['st_size'], s.name) for s in sym.iter_symbols()
               if s.entry['st_info']['type']=='STT_FUNC' and s['st_shndx']==ti)
starts = [f[0] for f in funcs]
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM); md.skipdata = True
from collections import Counter
c = Counter()
for i in md.disasm(raw[base: base + text['sh_size']], 0):
    if i.mnemonic in ('movw', 'cmp', 'cmn', 'mov') and any(f"#{hex(v)}" == i.op_str.split(', ')[-1] for v in imms):
        k = bisect.bisect_right(starts, i.address) - 1
        f = funcs[k] if k >= 0 else (0, 0, '?')
        c[f[2] if i.address < f[0] + f[1] else f"{f[2]}+(static after)"] += 1
for k, v in sorted(c.items(), key=lambda x: x[0]): print(f"{v:3d}  {k}")

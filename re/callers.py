# Find calls to a function (R_ARM_CALL/JUMP24 relocs) and show the preceding instructions that set r1 (table id).
import sys, struct, bisect
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
import capstone
fn, target, want = sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None
raw = open(fn,'rb').read(); e = ELFFile(open(fn,'rb'))
secs = list(e.iter_sections()); sym = e.get_section_by_name('.symtab')
text = e.get_section_by_name('.text'); ti = secs.index(text); base = text['sh_offset']
funcs = sorted((s['st_value'] & ~1, s['st_size'], s.name) for s in sym.iter_symbols()
               if s.entry['st_info']['type']=='STT_FUNC' and s['st_shndx']==ti)
starts = [f[0] for f in funcs]
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
for rs in e.iter_sections():
    if not (isinstance(rs, RelocationSection) and rs['sh_info'] == ti): continue
    for r in rs.iter_relocations():
        s = sym.get_symbol(r['r_info_sym'])
        if s.name != target or r['r_info_type'] not in (28, 29): continue
        a = r['r_offset']; k = bisect.bisect_right(starts, a) - 1; f = funcs[k]
        lo = max(f[0], a - 40)
        ins = list(md.disasm(raw[base+lo: base+a], lo))
        r1 = [i for i in ins if i.op_str.startswith('r1,')]
        r1s = r1[-1].mnemonic + ' ' + r1[-1].op_str if r1 else '?'
        if want and want not in r1s: continue
        print(f"{a:6x} in {f[2]}+{a-f[0]:#x}: {r1s}")

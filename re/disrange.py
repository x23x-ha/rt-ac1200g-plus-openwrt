# Disassemble .text range with relocations resolved (calls, literal pool words -> symbol+addend).
import sys, struct, bisect
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
import capstone
fn, lo, hi = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
raw = open(fn,'rb').read(); e = ELFFile(open(fn,'rb'))
secs = list(e.iter_sections()); sym = e.get_section_by_name('.symtab')
text = e.get_section_by_name('.text'); ti = secs.index(text); base = text['sh_offset']
objs = {}
for s in sym.iter_symbols():
    if isinstance(s['st_shndx'], int) and s['st_shndx'] > 0 and s.name:
        objs.setdefault(secs[s['st_shndx']].name, []).append((s['st_value'] & ~1, s['st_size'], s.name))
for k in objs: objs[k].sort()
def name_at(sec, v):
    L = objs.get(sec, []); i = bisect.bisect_right(L, (v, 1 << 40, '')) - 1
    if i >= 0 and L[i][0] <= v < L[i][0] + max(L[i][1], 1):
        return L[i][2] + (f"+{v - L[i][0]:#x}" if v != L[i][0] else '')
    return f"{sec}+{v:#x}"
rel = {}
for rs in e.iter_sections():
    if isinstance(rs, RelocationSection) and rs['sh_info'] == ti:
        for r in rs.iter_relocations():
            if lo <= r['r_offset'] < hi:
                s = sym.get_symbol(r['r_info_sym']); rel[r['r_offset']] = (r['r_info_type'], s)
fsyms = {v: n for v, _, n in objs.get('.text', [])}
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
a = lo
while a < hi:
    if a in fsyms: print(f"\n<{fsyms[a]}>:")
    w = struct.unpack_from('<I', raw, base + a)[0]
    if a in rel:
        t, s = rel[a]
        if t == 2:
            tgt = name_at(secs[s['st_shndx']].name, w) if not s.name else f"{s.name}+{w:#x}"
            print(f"  {a:6x}: .word {tgt}"); a += 4; continue
        ins = list(md.disasm(raw[base+a: base+a+4], a))
        print(f"  {a:6x}: {ins[0].mnemonic} {s.name or '?'}"); a += 4; continue
    ins = list(md.disasm(raw[base+a: base+a+4], a))
    print(f"  {a:6x}: {ins[0].mnemonic:8s} {ins[0].op_str}" if ins else f"  {a:6x}: .word {w:#010x}")
    a += 4

import sys, struct, bisect, collections
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
import capstone
fn = sys.argv[1]; want = sys.argv[2:]
data = open(fn,'rb').read(); e = ELFFile(open(fn,'rb'))
secs = list(e.iter_sections()); symtab = e.get_section_by_name('.symtab')
text = e.get_section_by_name('.text'); tidx = secs.index(text)
relmap = {}
for rs in e.iter_sections():
    if isinstance(rs, RelocationSection) and rs['sh_info'] == tidx:
        for r in rs.iter_relocations():
            sym = symtab.get_symbol(r['r_info_sym'])
            nm = sym.name or (secs[sym['st_shndx']].name if isinstance(sym['st_shndx'],int) else '?')
            relmap[r['r_offset']] = (r['r_info_type'], nm, sym)
fsyms = {s.name:(s['st_value'], s['st_size']) for s in symtab.iter_symbols() if s.entry['st_info']['type']=='STT_FUNC' and s['st_shndx']==tidx}
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
for w in want:
    v, sz = fsyms[w]; thumb = v & 1; v &= ~1
    if thumb: md.mode = capstone.CS_MODE_THUMB
    code = data[text['sh_offset']+v: text['sh_offset']+v+sz]
    print(f"==== {w} @ {v:x} size {sz} {'thumb' if thumb else 'arm'}")
    off = 0
    while off < sz:
        ins = list(md.disasm(code[off:off+4], v+off))
        a = v+off
        if a in relmap:
            t, nm, sym = relmap[a]
            word = struct.unpack('<I', code[off:off+4])[0]
            if t == 2:
                print(f"  {a:6x}:  .word {nm}+0x{word:x}"); off += 4; continue
            if ins:
                print(f"  {a:6x}:  {ins[0].mnemonic} {nm}   ; rel{t}"); off += ins[0].size; continue
        if ins:
            i = ins[0]; print(f"  {a:6x}:  {i.mnemonic:8s} {i.op_str}"); off += i.size
        else:
            print(f"  {a:6x}:  .word 0x{struct.unpack('<I', code[off:off+4])[0]:08x}"); off += 4

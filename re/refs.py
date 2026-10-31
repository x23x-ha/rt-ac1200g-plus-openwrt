import sys, struct, bisect, collections
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
fn = sys.argv[1]
f = open(fn,'rb'); e = ELFFile(f); data = open(fn,'rb').read()
symtab = e.get_section_by_name('.symtab')
secidx = {i:s for i,s in enumerate(e.iter_sections())}
# functions per section
funcs = collections.defaultdict(list)
dobjs = collections.defaultdict(list)
for s in symtab.iter_symbols():
    t = s.entry['st_info']['type']
    if isinstance(s['st_shndx'],int) and s['st_shndx']>0:
        sn = secidx[s['st_shndx']].name
        if t=='STT_FUNC': funcs[sn].append((s['st_value'] & ~1, s['st_size'], s.name))
        if t=='STT_OBJECT': dobjs[sn].append((s['st_value'], s['st_size'], s.name))
for k in funcs: funcs[k].sort()
for k in dobjs: dobjs[k].sort()
def func_at(sec, off):
    L = funcs.get(sec, []); i = bisect.bisect_right(L, (off, 1<<40, '')) - 1
    if i>=0 and L[i][0] <= off < L[i][0]+L[i][1]: return L[i][2]
    return '?'
refs = []  # (target .data offset, func, relsec)
for rs in e.iter_sections():
    if not isinstance(rs, RelocationSection): continue
    tgt = secidx[rs['sh_info']]
    if not tgt.name.startswith('.text'): continue
    for r in rs.iter_relocations():
        sym = symtab.get_symbol(r['r_info_sym'])
        if not isinstance(sym['st_shndx'],int) or sym['st_shndx']==0: continue
        if secidx[sym['st_shndx']].name != '.data': continue
        typ = r['r_info_type']
        if typ != 2: continue  # R_ARM_ABS32
        off = r['r_offset']
        addend = struct.unpack('<I', data[tgt['sh_offset']+off: tgt['sh_offset']+off+4])[0]
        refs.append((sym['st_value']+addend, func_at(tgt.name, off), sym.name))
refs.sort()
lo, hi = int(sys.argv[2],16), int(sys.argv[3],16)
seen=set()
for t, fn_, sn in refs:
    if lo <= t < hi:
        k=(t,fn_)
        if k in seen: continue
        seen.add(k)
        print(f"{t:08x} {fn_} ({sn})")
print('--- named .data objects in range')
for v,sz,n in dobjs['.data']:
    if lo-0x2000 <= v < hi+0x2000: print(f"{v:08x} {sz:6d} {n}")

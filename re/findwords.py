import sys, struct
from elftools.elf.elffile import ELFFile
fn = sys.argv[1]
data = open(fn,'rb').read()
e = ELFFile(open(fn,'rb'))
secs = [(s.name, s['sh_offset'], s['sh_size']) for s in e.iter_sections() if s['sh_type']!='SHT_NOBITS']
syms = []
for s in e.get_section_by_name('.symtab').iter_symbols():
    if s.entry['st_info']['type']=='STT_OBJECT' and isinstance(s['st_shndx'],int):
        syms.append((e.get_section(s['st_shndx']).name, s['st_value'], s['st_size'], s.name))
def where(off):
    for n,o,sz in secs:
        if o<=off<o+sz:
            v = off-o
            best = [x for x in syms if x[0]==n and x[1]<=v<x[1]+max(x[2],1)]
            return n, v, best
    return None,None,[]
pats = {
 'ipa_2057rev14_2g': [0x50df002e,0x50cf002d,0x50bf002c,0x50b7002b],
 'epa_2057rev3': [0x80f90040,0x80e10040,0x80e1003c,0x80c9003d],
 'epa_2057rev5': [0x10f90040,0x10e10040,0x10e1003c,0x10c9003d],
}
for name, w in pats.items():
    b = struct.pack('<4I', *w)
    i = data.find(b); hits=[]
    while i!=-1:
        hits.append(i); i = data.find(b, i+1)
    print(name, [ (hex(h),)+where(h)[:2]+(where(h)[2],) for h in hits])

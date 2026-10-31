import sys
from elftools.elf.elffile import ELFFile
e = ELFFile(open(sys.argv[1],'rb'))
st = e.get_section_by_name('.symtab')
out = []
for s in st.iter_symbols():
    shn = s['st_shndx']
    sec = e.get_section(shn).name if isinstance(shn,int) else shn
    out.append((sec, s['st_value'], s['st_size'], s.entry['st_info']['type'], s.name))
for o in out: print(f"{o[0]:20s} {o[1]:08x} {o[2]:6d} {o[3]:12s} {o[4]}")

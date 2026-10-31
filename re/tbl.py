import sys, struct
from elftools.elf.elffile import ELFFile
fn=sys.argv[1]; d=open(fn,'rb').read(); e=ELFFile(open(fn,'rb'))
ds=e.get_section_by_name('.data'); base=ds['sh_offset']
def tbl(off): return struct.unpack('<128I', d[base+off: base+off+512])
offs=[0x26dc4+0x200*i for i in range(20)]
import hashlib
seen={}
for o in offs:
    t=tbl(o); h=hashlib.sha1(struct.pack('<128I',*t)).hexdigest()[:8]
    print(f"{o:06x} {h} {' '.join('%08x'%x for x in t[:4])} ... {' '.join('%08x'%x for x in t[-2:])}" + (f"  == {seen[h]:x}" if h in seen else ''))
    seen.setdefault(h,o)

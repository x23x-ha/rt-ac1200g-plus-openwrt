import sys, struct, re
from elftools.elf.elffile import ELFFile
ko, b43c, smac = sys.argv[1:4]
d=open(ko,'rb').read(); e=ELFFile(open(ko,'rb')); base=e.get_section_by_name('.data')['sh_offset']
def wl(off,n=128): return list(struct.unpack(f'<{n}I', d[base+off: base+off+4*n]))
def csrc(path, name):
    s=open(path).read(); m=re.search(re.escape(name)+r'\[\]\s*=\s*\{(.*?)\};', s, re.S)
    return [int(x,16) for x in re.findall(r'0x[0-9a-fA-F]+', m.group(1))]
checks=[(b43c,'b43_ntab_tx_gain_ipa_2057_rev14_2g',0x22ea0),
        (b43c,'b43_ntab_tx_gain_epa_rev3_2g',0x27bc4),
        (b43c,'b43_ntab_tx_gain_epa_rev3_hi_pwr_2g',0x279c4),
        (smac,'nphy_tpc_txgain_epa_2057rev3',0x26fc4),
        (smac,'nphy_tpc_txgain_epa_2057rev5',0x291c4)]
for p,n,o in checks:
    a=csrc(p,n); b=wl(o,len(a))
    diff=[i for i in range(len(a)) if a[i]!=b[i]]
    print(f"{n:40s} len={len(a)} wl@{o:x} {'IDENTICAL' if not diff else 'DIFF at '+str(diff[:10])}")

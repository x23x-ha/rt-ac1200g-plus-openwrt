import struct, sys
f = open(sys.argv[1],'rb').read()
t = f[0x40000:]
magic, ln, crc, fv = struct.unpack('<4sIIH', t[:14]); flags=fv
v = struct.unpack('<H', t[14:16])[0]
offs = struct.unpack('<3I', t[16:28])
print('TRX', magic, hex(ln), hex(crc), 'flags',hex(flags),'ver',v, [hex(o) for o in offs])
for o in offs:
    if o: print(hex(o), t[o:o+8])
open(sys.argv[2]+'/trx.bin','wb').write(t[:ln])
# nvram
n = f[0xff0000:]
print('NVRAM magic', n[:4], 'len', hex(struct.unpack('<I', n[4:8])[0]))
ln2 = struct.unpack('<I', n[4:8])[0]
body = n[20:ln2]
open(sys.argv[2]+'/nvram.bin','wb').write(n[:ln2])
open(sys.argv[2]+'/nvram.txt','w').write('\n'.join(x.decode('latin1') for x in body.split(b'\0') if x)+'\n')

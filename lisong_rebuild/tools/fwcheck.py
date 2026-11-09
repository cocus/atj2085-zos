import struct,sys
def cs32(b):
    b=b+b'\0'*(-len(b)%4); return sum(struct.unpack('<%dI'%(len(b)//4),b))&0xffffffff
def cs16(b):
    b=b+b'\0'*(-len(b)%2); return sum(struct.unpack('<%dH'%(len(b)//2),b))&0xffff
def cs8(b): return sum(b)&0xffffffff
afi=open(sys.argv[1],'rb').read()
print('AFI hdr', afi[:16])
for i in range(0x20,0x200,0x20):
    e=afi[i:i+0x20]
    if e[:4]==b'\0\0\0\0': break
    name=e[:8].decode().strip()+'.'+e[8:11].decode().strip(); typ=e[11:12]
    a,b,off,ln=struct.unpack_from('<HHII',e,12)
    sub=e[24:28]; ck=struct.unpack_from('<I',e,28)[0]
    blob=afi[off:off+ln]
    print(f'{name:14s} type={typ} {a:04x} {b:04x} off={off:#x} len={ln:#x} sub={sub} ck={ck:08x} cs32={cs32(blob):08x} cs16={cs16(blob):04x}')
fw=afi[0xaa00:0xaa00+0x608c00]
print('FW hdr ck words', fw[0x1f0:0x200].hex())
ents=[]
for i in range(0x200,0x2000,0x20):
    e=fw[i:i+0x20]
    if e[0]==0: break
    off,ln=struct.unpack_from('<II',e,16); ck=struct.unpack_from('<I',e,28)[0]
    blob=fw[off*512:off*512+ln]
    ents.append((e[:11].decode(),off,ln,ck,cs32(blob)))
bad=[x for x in ents if x[3]!=x[4]]
print(len(ents),'fw entries;', 'checksum mismatches (cs32):', len(bad)); print(bad[:5])
print('dir cs32', hex(cs32(fw[0x200:0x2000])), 'hdr stored?', fw[0x1f0:0x200].hex())

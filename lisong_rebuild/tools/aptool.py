"""ACTOS 3.0 AP container (.AP) parse / build.

Header (0x800 bytes, little endian):
  00  'P\\0WG' 19 97 03 00         magic + version
  08  u32 text_off  u16 text_len  u16 text_addr     (common code: RCODE, API30 ...)
  10  u32 data_off  u16 data_len  u16 data_addr     (initialised data/const)
  18  u16 bss_len   u16 bss_addr
  1C  u16 entry     u8 entry_bank  u8 nbanks
  20+8*bank: u32 off  u16 len  u16 addr             (one slot per bank number 0..0xFE)

usage:
  aptool.py info  X.AP
  aptool.py unpack X.AP outdir      -> outdir/ap.json + seg_*.bin
  aptool.py pack   outdir X.AP      -> rebuilds, byte exact if layout unchanged
"""
import struct, sys, os, json

MAGIC = bytes.fromhex('50005747199703 00'.replace(' ', ''))


def parse(d):
    assert d[:8] == MAGIC, 'not an AP'
    toff, tlen, taddr = struct.unpack_from('<IHH', d, 8)
    doff, dlen, daddr = struct.unpack_from('<IHH', d, 16)
    bss_len, bss_addr, entry = struct.unpack_from('<HHH', d, 24)
    segs = []
    if tlen:
        segs.append(dict(name='text', bank=None, off=toff, addr=taddr, len=tlen))
    if dlen:
        segs.append(dict(name='data', bank=None, off=doff, addr=daddr, len=dlen))
    for b in range(0, (0x800 - 0x20) // 8):
        o = 0x20 + 8 * b
        off, ln, ad = struct.unpack_from('<IHH', d, o)
        if off or ln:
            segs.append(dict(name=f'bank{b:02x}', bank=b, off=off, addr=ad, len=ln))
    for s in segs:
        s['bytes'] = d[s['off']:s['off'] + s['len']]
    # filler byte used by the AP builder between/after segments ('v' = 0x76 in ACTOS tools)
    ends = sorted((x['off'] + x['len']) for x in segs)
    pad = 0
    for e in ends:
        if e < len(d) and e % 512:
            pad = d[e]; break
    return dict(pad=pad, entry=entry, entry_bank=d[0x1E], nbanks=d[0x1F], bss_len=bss_len,
                bss_addr=bss_addr, size=len(d), segs=segs,
                )


def build(ap):
    """Rebuild from parsed dict. Layout: header 0x800 then segments in file-offset
    order, each padded to 512 bytes (that is what APBuilder does)."""
    pad = ap.get('pad', 0)
    out = bytearray(0x800)
    out[:8] = MAGIC
    segs = sorted(ap['segs'], key=lambda s: s['off'])
    pos = 0x800
    for s in segs:
        if s.get('keep_off', True) and s['off'] >= pos:
            pos = s['off']
        s['off'] = pos
        if len(out) < pos:
            out += bytes([pad]) * (pos - len(out))
        out[pos:pos + len(s['bytes'])] = s['bytes']
        s['len'] = len(s['bytes'])
        pos += s['len']
    total = ap.get('size') or ((len(out) + 511) // 512 * 512)
    out += bytes([pad]) * max(0, total - len(out))
    for s in ap['segs']:
        if s['name'] == 'text':
            struct.pack_into('<IHH', out, 8, s['off'], s['len'], s['addr'])
        elif s['name'] == 'data':
            struct.pack_into('<IHH', out, 16, s['off'], s['len'], s['addr'])
        else:
            struct.pack_into('<IHH', out, 0x20 + 8 * s['bank'], s['off'], s['len'], s['addr'])
    struct.pack_into('<HHH', out, 24, ap['bss_len'], ap['bss_addr'], ap['entry'])
    out[0x1E] = ap['entry_bank']
    out[0x1F] = ap['nbanks']
    return bytes(out)


def main():
    cmd = sys.argv[1]
    if cmd == 'info':
        for p in sys.argv[2:]:
            ap = parse(open(p, 'rb').read())
            print(f"{os.path.basename(p)} entry={ap['entry']:04x} bank={ap['entry_bank']:02x} nb={ap['nbanks']} bss={ap['bss_addr']:04x}+{ap['bss_len']:04x}")
            for s in ap['segs']:
                print(f"   {s['name']:7s} file@{s['off']:06x} load@{s['addr']:04x} len={s['len']:04x}")
    elif cmd == 'unpack':
        p, od = sys.argv[2], sys.argv[3]
        os.makedirs(od, exist_ok=True)
        ap = parse(open(p, 'rb').read())
        meta = {k: v for k, v in ap.items() if k != 'segs'}
        meta['segs'] = []
        for s in ap['segs']:
            fn = f"seg_{s['name']}_{s['addr']:04x}.bin"
            open(os.path.join(od, fn), 'wb').write(s['bytes'])
            meta['segs'].append({k: s[k] for k in ('name', 'bank', 'off', 'addr')} | {'file': fn})
        # record 'slack' bytes (stale buffer data the original packer left in padding)
        orig = open(p, 'rb').read()
        tmp = parse(orig)
        rb = build(tmp)
        slack, i = [], 0
        while i < len(orig):
            if rb[i] != orig[i]:
                j = i
                while j < len(orig) and rb[j] != orig[j]:
                    j += 1
                slack.append([i, orig[i:j].hex()]); i = j
            else:
                i += 1
        meta['slack'] = slack
        json.dump(meta, open(os.path.join(od, 'ap.json'), 'w'), indent=1)
    elif cmd == 'pack':
        od, outp = sys.argv[2], sys.argv[3]
        meta = json.load(open(os.path.join(od, 'ap.json')))
        for s in meta['segs']:
            s['bytes'] = open(os.path.join(od, s['file']), 'rb').read()
        out = bytearray(build(meta))
        for off, hx in meta.get('slack', []):
            b = bytes.fromhex(hx); out[off:off + len(b)] = b
        open(outp, 'wb').write(out)


if __name__ == '__main__':
    main()

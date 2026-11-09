"""ACTOS firmware image tool (AFI outer container + FWIMAGE.FW inner file system).

AFI (.bin as flashed by the updater):
  0x000  'AFI\\0' + 12 bytes (vendor tag, ver)  + 16 bytes
  0x020  32-byte entries until zero:  name[8] ext[3] type[1] u16 a u16 b u32 off u32 len sub[4] u32 cs32
FWIMAGE.FW:
  0x000  header (0x200): 55 AA F0 0F, ver, date, VID/PID, u32 dir_cs32 @0x10, strings, u16 hdr_cs @0x1FE
  0x200  32-byte dir entries: name[8] ext[3] attr[5] u32 sector u32 len u32 ? u32 cs32
  data   files at sector*512, each padded to 512
All checksums = sum of little-endian u32 words (mod 2^32).

usage:
  fwtool.py unpack FW.bin outdir     -> outdir/afi.json, afi parts, fwimage/ files + fw.json
  fwtool.py pack   outdir FW.bin     -> rebuild (recomputes every checksum)
"""
import struct, sys, os, json


def cs32(b):
    b = b + b'\0' * (-len(b) % 4)
    return sum(struct.unpack('<%dI' % (len(b) // 4), b)) & 0xFFFFFFFF


def cs16(b):
    b = b + b'\0' * (-len(b) % 2)
    return sum(struct.unpack('<%dH' % (len(b) // 2), b)) & 0xFFFF


def name83(e):
    n = e[:8].decode('latin1').rstrip(' \0'); x = e[8:11].decode('latin1').rstrip(' \0')
    return n + ('.' + x if x else '')


def diff_slack(orig, rebuilt):
    slack, i = [], 0
    n = len(orig)
    while i < n:
        if i >= len(rebuilt) or rebuilt[i] != orig[i]:
            j = i
            while j < n and (j >= len(rebuilt) or rebuilt[j] != orig[j]):
                j += 1
            slack.append([i, orig[i:j].hex()]); i = j
        else:
            i += 1
    return slack


def apply_slack(buf, slack):
    for off, hx in slack:
        b = bytes.fromhex(hx)
        if len(buf) < off + len(b):
            buf += bytes(off + len(b) - len(buf))
        buf[off:off + len(b)] = b
    return buf


# ---------------- FWIMAGE.FW ----------------
def fw_parse(fw):
    ents = []
    for i in range(0x200, 0x2000, 0x20):
        e = fw[i:i + 0x20]
        if e[0] == 0:
            break
        sec, ln, unk, ck = struct.unpack_from('<IIII', e, 16)
        ents.append(dict(name=name83(e), raw=e[:16].hex(), sector=sec, len=ln, unk=unk))
    return ents


def fw_build(hdr, ents, files, total_len, dir_end=0x2000):
    out = bytearray(hdr) + bytearray(dir_end - len(hdr))
    for k, e in enumerate(ents):
        data = files[e['name']]
        off = e['sector'] * 512
        if len(out) < off:
            out += bytes(off - len(out))
        out[off:off + len(data)] = data
        de = bytes.fromhex(e['raw']) + struct.pack('<IIII', e['sector'], len(data), e['unk'], cs32(data))
        out[0x200 + 0x20 * k:0x220 + 0x20 * k] = de
        end = off + len(data)
        pad = (-end) % 512
        if len(out) < end + pad:
            out += bytes(end + pad - len(out))
    if total_len and len(out) < total_len:
        out += bytes(total_len - len(out))
    struct.pack_into('<I', out, 0x10, cs32(bytes(out[0x200:dir_end])))
    struct.pack_into('<H', out, 0x1FE, 0)
    struct.pack_into('<H', out, 0x1FE, (-cs16(bytes(out[0:0x200]))) & 0xFFFF if False else hdr_cs(out))
    return out


def hdr_cs(out):
    # header checksum: u16 sum of words 0..0x1FD
    return cs16(bytes(out[0:0x1FE]))


# ---------------- AFI ----------------
def afi_parse(d):
    ents = []
    for i in range(0x20, 0x200, 0x20):
        e = d[i:i + 0x20]
        if e[:4] == b'\0\0\0\0':
            break
        off, ln = struct.unpack_from('<II', e, 16)
        ents.append(dict(name=name83(e), raw=e[:16].hex(), sub=e[24:28].hex(), off=off, len=ln))
    return ents


def unpack(path, od):
    d = open(path, 'rb').read()
    os.makedirs(os.path.join(od, 'fwimage'), exist_ok=True)
    ents = afi_parse(d)
    for e in ents:
        blob = d[e['off']:e['off'] + e['len']]
        if e['name'] == 'FWIMAGE.FW':
            fents = fw_parse(blob)
            files = {}
            for fe in fents:
                data = blob[fe['sector'] * 512: fe['sector'] * 512 + fe['len']]
                files[fe['name']] = data
                open(os.path.join(od, 'fwimage', fe['name']), 'wb').write(data)
            hdr = blob[:0x200]
            open(os.path.join(od, 'fwimage_header.bin'), 'wb').write(hdr)
            rb = fw_build(hdr, fents, files, len(blob))
            fw_meta = dict(entries=fents, total_len=len(blob), slack=diff_slack(blob, rb))
            json.dump(fw_meta, open(os.path.join(od, 'fw.json'), 'w'), indent=1)
        else:
            open(os.path.join(od, e['name']), 'wb').write(blob)
    meta = dict(header=d[:0x20].hex(), entries=ents, total_len=len(d))
    rb = afi_build(od, meta, d_for_slack=None)
    meta['slack'] = diff_slack(d, rb)
    json.dump(meta, open(os.path.join(od, 'afi.json'), 'w'), indent=1)
    print(f"unpacked {len(ents)} AFI parts; FW slack runs={len(fw_meta['slack'])}, AFI slack runs={len(meta['slack'])}")


def build_fwimage(od):
    m = json.load(open(os.path.join(od, 'fw.json')))
    hdr = open(os.path.join(od, 'fwimage_header.bin'), 'rb').read()
    files = {e['name']: open(os.path.join(od, 'fwimage', e['name']), 'rb').read() for e in m['entries']}
    out = fw_build(hdr, m['entries'], files, m['total_len'])
    out = apply_slack(out, m['slack'])
    # slack may touch checksummed areas only if layout changed; recompute to be safe
    struct.pack_into('<I', out, 0x10, cs32(bytes(out[0x200:0x2000])))
    return bytes(out)


def afi_build(od, meta, d_for_slack=None):
    out = bytearray(bytes.fromhex(meta['header']))
    out += bytes(0x200 - len(out))
    for k, e in enumerate(meta['entries']):
        if e['name'] == 'FWIMAGE.FW':
            blob = build_fwimage(od)
        else:
            blob = open(os.path.join(od, e['name']), 'rb').read()
        off = e['off']
        if len(out) < off:
            out += bytes(off - len(out))
        out[off:off + len(blob)] = blob
        de = bytes.fromhex(e['raw']) + struct.pack('<II', off, len(blob)) + bytes.fromhex(e['sub']) + struct.pack('<I', cs32(blob))
        out[0x20 + 0x20 * k: 0x40 + 0x20 * k] = de
    if len(out) < meta['total_len']:
        out += bytes(meta['total_len'] - len(out))
    struct.pack_into('<I', out, 0xFFC, cs32(bytes(out[:0xFFC])))  # AFI header checksum
    return out


def pack(od, outp):
    meta = json.load(open(os.path.join(od, 'afi.json')))
    out = afi_build(od, meta)
    out = apply_slack(out, meta.get('slack', []))
    open(outp, 'wb').write(out)


if __name__ == '__main__':
    if sys.argv[1] == 'unpack':
        unpack(sys.argv[2], sys.argv[3])
    elif sys.argv[1] == 'pack':
        pack(sys.argv[2], sys.argv[3])

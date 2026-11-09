"""zkern: firmware in which BREC starts zos directly (the stock firmware stays inside, MENU = stock).

usage: python zkern/build.py [--zos DIR] [--build-zos] [--check FW ...]
  --zos DIR     zos build output with ZOS.AP (default zos/out/atj2085)
  --build-zos   run `zos/build.py --no-deploy` first
  --check FW    only verify firmware file(s) against the BREC rules (no build)
-> s1mp3/build_v04/zos-native-v04.fw          the zos-native firmware (flash with s1res)
   s1mp3/build_v04/zos-native-control-v04.fw  the base firmware passed through the same packer
                                              unmodified (must be byte-identical to the base)
   s1mp3/build_v04/zkern/                     stub listing/map/symbols, SYSCFG.SYS + SYSCFG.STK,
                                              zos snapshot (disk/), TEST-ONLY-badzos-do-not-flash.fw
Emulator proof: zkern/emu_native.py (see analysis/zos_native_boot.md).

FWIMAGE layout (all stock files kept, only SYSCFG.SYS changed, one file added at the end):
  SYSCFG.SYS (same size 0x3800, same sectors as stock), as BREC 0x0216 loads it:
     file 0x0000-0x01FF -> RAM 0x0600  stage-0 stub (zkern/stub.s, _CODE)
     file 0x0200-...    -> RAM 0x0800  zos core image (ZOS.AP text, < 0x2400 bytes)
     file 0x3700-0x377F -> RAM 0x0000  vector page (stub.s, _VEC)
  SYSCFG.STK = the stock SYSCFG.SYS, byte-identical, appended after the last file; the stub
     chain-loads it with BREC's own loader (name patched in RAM) when MENU is held.
Checksums: per-file u32 word sum (dir +0x1C), dir sum @0x10, header u16 sum @0x1FE, AFI entry
sums and AFI header sum @0xFFC - all recomputed here and re-verified by check_fw().
"""
import os, sys, struct, hashlib, shutil, subprocess, importlib.util, json
HERE = os.path.dirname(os.path.abspath(__file__))
HB = os.path.dirname(HERE)
ROOT = os.path.dirname(HB)                       # lisong_rebuild
TOP = os.path.dirname(ROOT)                      # s1mp3
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from asm_util import SDAS, SDLD, parse_ihx      # noqa: E402

BASE_FW = os.path.join(TOP, 'my-ximpro.fw')
OUTDIR = os.path.join(TOP, 'build_v04')          # s1mp3/build_v04 (as requested)
ZOS = os.path.join(TOP, 'zos')

SYS_NAME = b'SYSCFG  SYS'
STK_NAME = b'SYSCFG  STK'
SYS_LEN = 0x3800                 # stock SYSCFG.SYS size (BREC reads up to file 0x377F)
STUB_ORG, STUB_MAX = 0x0600, 0x200
ZOS_ORG, ZOS_MAXLEN = 0x0800, 0x2400
BREC_MAIN_LEN = 0x2D00           # BREC 0x02F2..0x0333: 22 sectors + 0x100 bytes -> 0x0600-0x32FF
VEC_FILE_OFF, VEC_LEN = 0x3700, 0x80   # BREC 0x0342-0x0357: file +0xB00 more, 0x80 bytes -> 0x0000
FILL = 0x76                       # stock filler (halt)


# ------------------------------------------------------------------------------- checksums
def cs32(b):
    b = bytes(b) + b'\0' * (-len(b) % 4)
    return sum(struct.unpack('<%dI' % (len(b) // 4), b)) & 0xFFFFFFFF


def cs16(b):
    b = bytes(b) + b'\0' * (-len(b) % 2)
    return sum(struct.unpack('<%dH' % (len(b) // 2), b)) & 0xFFFF


# ------------------------------------------------------------------------------- AFI / FWIMAGE
def afi_parts(afi):
    assert afi[:4] == b'AFI\0', 'not an AFI image'
    parts = []
    for i in range(0x20, 0x200, 0x20):
        e = afi[i:i + 0x20]
        if e[:4] == b'\0\0\0\0':
            break
        off, ln = struct.unpack_from('<II', e, 0x10)
        parts.append(dict(slot=i, name=e[:11], off=off, len=ln, cs=struct.unpack_from('<I', e, 0x1C)[0]))
    return parts


def afi_fwimage(afi):
    for p in afi_parts(afi):
        if p['name'] == b'FWIMAGE FW ':
            return p
    raise ValueError('no FWIMAGE.FW in the AFI')


def fw_dir(fw):
    ents = []
    for k in range(240):
        o = 0x200 + 0x20 * k
        if fw[o] == 0:
            break
        sec, ln, unk, ck = struct.unpack_from('<IIII', fw, o + 0x10)
        ents.append(dict(k=k, o=o, name=bytes(fw[o:o + 11]), sector=sec, len=ln, cs=ck))
    return ents


def fw_find(fw, name):
    for e in fw_dir(fw):
        if e['name'] == name:
            return e
    return None


def fw_replace(fw, name, data):
    """same-size replacement in place"""
    e = fw_find(fw, name)
    assert e and e['len'] == len(data), (name, e and e['len'], len(data))
    fw[e['sector'] * 512:e['sector'] * 512 + len(data)] = data
    struct.pack_into('<I', fw, e['o'] + 0x1C, cs32(data))


def fw_append(fw, name, data):
    """new directory entry + data after the last file (sector aligned)"""
    assert fw_find(fw, name) is None, name
    ents = fw_dir(fw)
    assert len(ents) < 240, 'directory full'
    end = max(e['sector'] * 512 + e['len'] for e in ents)
    end = max(end, len(fw))
    sec = -(-end // 512)
    fw.extend(bytes(sec * 512 - len(fw)))
    fw.extend(data + bytes(-len(data) % 512))
    o = 0x200 + 0x20 * len(ents)
    fw[o:o + 0x20] = name + bytes(5) + struct.pack('<IIII', sec, len(data), 0, cs32(data))
    return sec


def fw_seal(fw):
    struct.pack_into('<I', fw, 0x10, cs32(fw[0x200:0x2000]))
    struct.pack_into('<H', fw, 0x1FE, cs16(fw[0:0x1FE]))


def afi_with_fwimage(afi, fw):
    """replace the FWIMAGE part (the last part of the AFI), fix its entry and the header sum"""
    p = afi_fwimage(afi)
    # other parts lie before it or alias files inside it (FWSCF641.BIN, ADFUS.BIN are FWIMAGE
    # files): nothing may extend past its end. Existing FWIMAGE files never move (fw_append).
    assert all(q['off'] + q['len'] <= p['off'] + p['len'] for q in afi_parts(afi)), \
        'an AFI part lies behind FWIMAGE.FW'
    tail = afi[p['off'] + p['len']:]
    assert not tail.strip(b'\0'), 'data after FWIMAGE.FW'
    out = bytearray(afi[:p['off']]) + fw
    out += bytes(max(0, len(afi) - len(out)))
    struct.pack_into('<II', out, p['slot'] + 0x10, p['off'], len(fw))
    struct.pack_into('<I', out, p['slot'] + 0x1C, cs32(fw))
    struct.pack_into('<I', out, 0xFFC, cs32(out[:0xFFC]))
    return out


# ------------------------------------------------------------------------------- verification
def brec_verified(name):
    """BREC 0x0836-0x084C: entries with ext[0] == '$' or ext = 'R?S' are not summed"""
    return not (name[8:9] == b'$' or (name[8:9] == b'R' and name[10:11] == b'S'))


def check_fw(path, quiet=False):
    """the rules BREC (0x06DB-0x0875, 0x0216) and s1res (FirmwareIO::write) apply; returns (ok, info)"""
    afi = open(path, 'rb').read()
    errs = []
    for p in afi_parts(afi):
        if p['len'] and cs32(afi[p['off']:p['off'] + p['len']]) != p['cs']:
            errs.append('AFI part %s: sum' % p['name'])
    if struct.unpack_from('<I', afi, 0xFFC)[0] != cs32(afi[:0xFFC]):
        errs.append('AFI header sum @0xFFC')
    p = afi_fwimage(afi)
    fw = afi[p['off']:p['off'] + p['len']]
    if fw[:4] != b'\x55\xaa\xf0\x0f':
        errs.append('FW magic')
    if struct.unpack_from('<H', fw, 0x1FE)[0] != cs16(fw[:0x1FE]):
        errs.append('FW header sum16')
    if struct.unpack_from('<I', fw, 0x10)[0] != cs32(fw[0x200:0x2000]):
        errs.append('FW directory sum32')
    ents = fw_dir(fw)
    nver = 0
    spans = []
    for e in ents:
        d = fw[e['sector'] * 512:e['sector'] * 512 + e['len']]
        if len(d) != e['len']:
            errs.append('%s: beyond the image' % e['name'])
        if brec_verified(e['name']):
            nver += 1
        if cs32(d) != e['cs']:
            errs.append('%s: file sum%s' % (e['name'], ' (BREC-verified!)' if brec_verified(e['name']) else ''))
        if e['len']:
            spans.append((e['sector'] * 512, e['sector'] * 512 + e['len'], e['name']))
    spans.sort()
    for a, b in zip(spans, spans[1:]):
        if b[0] < a[1]:
            errs.append('overlap %s / %s' % (a[2], b[2]))
    if fw_find(fw, SYS_NAME) is None:
        errs.append('no SYSCFG  SYS')
    info = dict(files=len(ents), brec_verified=nver, fw_len=len(fw), afi_len=len(afi),
                md5=hashlib.md5(afi).hexdigest())
    if not quiet:
        print('%s: %s  (%d files, %d BREC-verified, FWIMAGE %d bytes, AFI %d bytes, md5 %s)' % (
            os.path.basename(path), 'OK' if not errs else 'FAIL: ' + '; '.join(errs),
            info['files'], nver, len(fw), len(afi), info['md5']))
    return not errs, info


def brec_load_view(sysfile):
    """RAM contents BREC 0x0216 produces from a SYSCFG.SYS: {addr: bytes}"""
    return {0x0600: sysfile[:BREC_MAIN_LEN], 0x0000: sysfile[VEC_FILE_OFF:VEC_FILE_OFF + VEC_LEN]}


# ------------------------------------------------------------------------------- stub + zos
def run(cmd, cwd):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(' '.join(cmd) + '\n' + r.stdout + r.stderr)
    return r.stdout + r.stderr


def zos_build_module():
    spec = importlib.util.spec_from_file_location('zosbuild', os.path.join(ZOS, 'build.py'))
    zb = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(zb)
    return zb


def gen_regs_inc(out):
    """the same regs.inc zos' build generates from targets/atj2085/regs.h"""
    zos_build_module().gen_regs_inc(os.path.join(ZOS, 'targets', 'atj2085'), out)


def build_stub(out):
    gen_regs_inc(out)
    rel = os.path.join(out, 'stub.rel')
    run([SDAS, '-g', '-l', '-I' + out, '-I' + HERE, '-o', rel, os.path.join(HERE, 'stub.s')], out)
    run([SDLD, '-n', '-m', '-i', 'stub.ihx', '-b', '_CODE=0x%04x' % STUB_ORG, rel], out)
    mem = parse_ihx(open(os.path.join(out, 'stub.ihx')).read())
    code = [a for a in mem if a >= STUB_ORG]
    vec = [a for a in mem if a < VEC_LEN]
    assert len(code) + len(vec) == len(mem), 'stub bytes outside _CODE/_VEC'
    end = max(code) + 1
    if end > STUB_ORG + STUB_MAX:
        sys.exit('stub too large: ends at %#06x' % end)
    stub = bytearray(mem.get(a, 0) for a in range(STUB_ORG, STUB_ORG + STUB_MAX))
    vecs = bytearray(mem.get(a, FILL) for a in range(0, VEC_LEN))
    syms = {}                    # global labels -> stub_syms.json (for emu_native.py milestones)
    for ln in open(os.path.join(out, 'stub.map'), errors='replace'):
        for col in ln.split('|'):          # "     00000600  stub      |    00000635  go_stock  | ..."
            p = col.split()
            if len(p) == 2 and len(p[0]) == 8 and not p[1].startswith(('s_', 'l_', '.')):
                try:
                    syms[p[1]] = int(p[0], 16)
                except ValueError:
                    pass
    json.dump(syms, open(os.path.join(out, 'stub_syms.json'), 'w'), indent=1)
    print('stub: %d bytes at %#06x (%d free), vectors %d bytes' % (end - STUB_ORG, STUB_ORG,
                                                                     STUB_ORG + STUB_MAX - end, max(vec) + 1))
    return stub, vecs


def zos_image(zdir):
    ap = open(os.path.join(zdir, 'ZOS.AP'), 'rb').read()
    ln, addr = struct.unpack_from('<HH', ap, 12)
    assert addr == ZOS_ORG, 'ZOS.AP not linked at 0x0800'
    text = ap[0x800:0x800 + ln]
    assert len(text) == ln and text[0] == 0xC3, 'ZOS.AP: bad image'
    if ln > ZOS_MAXLEN:
        sys.exit('zos image %d bytes > %d' % (ln, ZOS_MAXLEN))
    return text


def make_syscfg(stub, vecs, ztext):
    assert stub[2:6] == b'ZKS1', 'stub parameter block moved'
    stub = bytearray(stub)
    struct.pack_into('<HH', stub, 6, len(ztext), sum(ztext) & 0xFFFF)
    f = bytearray([FILL]) * SYS_LEN
    f[0:STUB_MAX] = stub
    f[STUB_MAX:STUB_MAX + len(ztext)] = ztext
    f[VEC_FILE_OFF:VEC_FILE_OFF + VEC_LEN] = vecs
    assert STUB_MAX + len(ztext) <= BREC_MAIN_LEN
    return bytes(f)


def snapshot_zos(zdir, dst):
    """the zos files the native firmware was built with (copy them to the player's disk root)"""
    if os.path.exists(dst):
        shutil.rmtree(dst, ignore_errors=True)
    os.makedirs(dst, exist_ok=True)
    for f in os.listdir(zdir):
        s = os.path.join(zdir, f)
        if f.upper().endswith(('.AP', '.ZM', '.DSP')):
            shutil.copy(s, dst)
        elif f.upper() == 'SKIN' and os.path.isdir(s):
            shutil.copytree(s, os.path.join(dst, 'SKIN'))


def build(zdir):
    out = os.path.join(OUTDIR, 'zkern')
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        p = os.path.join(out, f)
        if os.path.isfile(p):
            os.remove(p)
    base = open(BASE_FW, 'rb').read()
    pf = afi_fwimage(base)
    fw0 = bytearray(base[pf['off']:pf['off'] + pf['len']])

    # control: unmodified files through the same packer
    ctl = bytearray(fw0)
    fw_seal(ctl)
    ctl_afi = afi_with_fwimage(base, ctl)
    ctl_path = os.path.join(OUTDIR, 'zos-native-control-v04.fw')
    open(ctl_path, 'wb').write(ctl_afi)
    same = bytes(ctl_afi) == base
    print('control: %s (%s the base %s)' % (ctl_path, 'byte-identical to' if same else 'DIFFERS from',
                                            os.path.basename(BASE_FW)))
    if not same:
        sys.exit('packer does not round-trip the base firmware')

    # native
    stub, vecs = build_stub(out)
    got = {}

    def take():                  # under zos' build lock: no half-written out/ from a parallel build
        got['z'] = zos_image(zdir)
        snapshot_zos(zdir, os.path.join(out, 'disk'))
    if os.path.abspath(zdir) == os.path.abspath(os.path.join(ZOS, 'out', 'atj2085')):
        zos_build_module().locked(take)
    else:
        take()
    ztext = got['z']
    stock_sys = bytes(fw0[fw_find(fw0, SYS_NAME)['sector'] * 512:][:fw_find(fw0, SYS_NAME)['len']])
    assert len(stock_sys) == SYS_LEN
    new_sys = make_syscfg(stub, vecs, ztext)
    fw = bytearray(fw0)
    fw_replace(fw, SYS_NAME, new_sys)
    stk_sec = fw_append(fw, STK_NAME, stock_sys)
    fw_seal(fw)
    nat_afi = afi_with_fwimage(base, fw)
    nat_path = os.path.join(OUTDIR, 'zos-native-v04.fw')
    open(nat_path, 'wb').write(nat_afi)
    open(os.path.join(out, 'SYSCFG.SYS'), 'wb').write(new_sys)
    open(os.path.join(out, 'SYSCFG.STK'), 'wb').write(stock_sys)
    print('zos: %d bytes (0x0800-%#06x), sum %#06x, md5 %s' % (len(ztext), 0x0800 + len(ztext),
                                                              sum(ztext) & 0xFFFF, hashlib.md5(ztext).hexdigest()))
    print('SYSCFG.STK (stock kernel) appended at sector %d; FWIMAGE %d -> %d bytes' % (stk_sec, len(fw0), len(fw)))
    print('native: %s' % nat_path)

    # verify both + cross-checks against the base
    ok1, _ = check_fw(ctl_path)
    ok2, _ = check_fw(nat_path)
    chk = afi_fwimage(nat_afi)
    nfw = nat_afi[chk['off']:chk['off'] + chk['len']]
    for e in fw_dir(fw0):
        n = fw_find(nfw, e['name'])
        a = fw0[e['sector'] * 512:e['sector'] * 512 + e['len']]
        b = nfw[n['sector'] * 512:n['sector'] * 512 + n['len']]
        assert n['sector'] == e['sector'], e['name']
        assert (a == b) == (e['name'] != SYS_NAME), e['name']
    s = fw_find(nfw, STK_NAME)
    assert nfw[s['sector'] * 512:s['sector'] * 512 + s['len']] == stock_sys
    for p in afi_parts(base):
        if p['name'] != b'FWIMAGE FW ':
            assert nat_afi[p['off']:p['off'] + p['len']] == base[p['off']:p['off'] + p['len']], p['name']
    view = brec_load_view(new_sys)
    assert view[0x0600][0x200:0x200 + len(ztext)] == ztext
    print('cross-check: every stock file unchanged at its sector, SYSCFG.STK == stock SYSCFG.SYS, '
          'BREC/FWSC/ADFUS AFI parts unchanged')
    if not (ok1 and ok2):
        sys.exit('verification failed')

    # emulator-only test image: valid BREC checksums, but the stub's zos sum is wrong -> must chain
    # to stock. Written into zkern/ (not next to the flashable images): NOT for flashing.
    bad = bytearray(new_sys)
    struct.pack_into('<H', bad, 8, (struct.unpack_from('<H', bad, 8)[0] + 1) & 0xFFFF)
    tfw = bytearray(fw)
    fw_replace(tfw, SYS_NAME, bytes(bad))
    fw_seal(tfw)
    tpath = os.path.join(out, 'TEST-ONLY-badzos-do-not-flash.fw')
    open(tpath, 'wb').write(afi_with_fwimage(base, tfw))
    check_fw(tpath)


def main():
    a = sys.argv[1:]
    if '--check' in a:
        ok = all(check_fw(p)[0] for p in a[a.index('--check') + 1:])
        sys.exit(0 if ok else 1)
    if '--build-zos' in a:
        print(run([sys.executable, os.path.join(ZOS, 'build.py'), '--no-deploy'], ZOS).strip().splitlines()[0])
    zdir = a[a.index('--zos') + 1] if '--zos' in a else os.path.join(ZOS, 'out', 'atj2085')
    build(zdir)


if __name__ == '__main__':
    main()

"""ACTOS 3.0 driver (.DRV) container.

Header (512 bytes, from SDK driver.h / ky_hard.msa):
  00 'D'  01 type (0 VM, 1 STG, 2 KY, 3 UI, 4 FS)
  02 u16 RcodeStartAddr  04 u16 RcodeLength   06 u16 InitAPI  08 u16 ExitAPI
  0A u32 BnkAOffset      0E u32 BnkBOffset    ... 1FE 55 AA
Resident code: file 0x200, loaded at RcodeStartAddr.
Bank A: 0x200-byte slots from BnkAOffset (API low byte = type base + slot, e.g. UI 0xA0+k),
Bank B: 0x800-byte slots from BnkBOffset (API = 0x1000 | base + slot).
Bank header (at its load address): u16 total_len, 8 x u16 entry, u8 data_len  (19 bytes)
API word: bit12 = bank B, bits 8..11 = entry index, low byte = slot number.

usage: drvtool.py info X.DRV [...]           print structure
       drvtool.py split X.DRV outdir          -> drv.json + rcode.bin + bankA_xx.bin / bankB_xx.bin
"""
import struct, sys, os, json
sys.path.insert(0, os.path.dirname(__file__))
import z80dis

TYPES = {0: 'VM', 1: 'STG', 2: 'KY', 3: 'UI', 4: 'FS'}
SLOT_A, SLOT_B = 0x200, 0x800


def parse(d):
    assert d[0:1] == b'D', 'not a driver'
    typ = d[1]
    rc_addr, rc_len, init_api, exit_api, ba, bb = struct.unpack_from('<HHHHII', d, 2)
    drv = dict(type=typ, type_name=TYPES.get(typ, '?'), rcode_addr=rc_addr, rcode_len=rc_len,
               init_api=init_api, exit_api=exit_api, bnka_off=ba, bnkb_off=bb, size=len(d),
               trailer=d[0x1FE:0x200].hex())
    drv['rcode'] = d[0x200:0x200 + rc_len]
    banks = []
    for kind, off, end, slot in (('A', ba, bb if bb > ba else len(d), SLOT_A), ('B', bb, len(d), SLOT_B)):
        if not off or off >= len(d):
            continue
        n = (end - off) // slot
        for k in range(n):
            o = off + k * slot
            blob = d[o:o + slot]
            ln = struct.unpack_from('<H', blob, 0)[0]
            if ln == 0 or ln > slot or blob[:2] == b'vv':
                continue
            ents = [0 if e == 0x7676 else e for e in struct.unpack_from('<8H', blob, 2)]   # 'vv' = unused
            dlen = blob[18]
            banks.append(dict(kind=kind, slot=k, file_off=o, len=ln, entries=ents, data_len=dlen,
                              bytes=blob[:ln]))
    drv['banks'] = banks
    return drv


def infer_base(bank):
    """the load address = the base that makes the most entry points + absolute jumps land in the bank"""
    b = bank['bytes']; ln = bank['len']
    cands = {}
    for base in range(0, 0x10000, 0x100):
        ents = [e for e in bank['entries'] if e]
        if not ents or not all(base + 19 <= e < base + ln for e in ents):
            continue
        score = 0
        for e in ents:
            pc = e; steps = 0
            while base <= pc < base + ln and steps < 64:
                ins = z80dis.decode(b, pc, base)
                if ins.kind == 'bad':
                    score -= 5; break
                if ins.kind in ('jp', 'jpcc', 'call', 'callcc') and ins.target is not None and base <= ins.target < base + ln:
                    score += 2
                if ins.kind in ('ret', 'jp', 'jr', 'jpind'):
                    score += 1; break
                pc += ins.size; steps += 1
        cands[base] = score
    if not cands:
        return None, {}
    best = max(cands, key=lambda k: cands[k])
    return best, cands


def api_word(drv, bank, idx):
    base = {0: 0x90, 1: 0x80, 2: 0xB0, 3: 0xA0, 4: 0xC0}.get(drv['type'], 0)
    lo = (base + bank['slot']) & 0xFF
    return (0x1000 if bank['kind'] == 'B' else 0) | (idx << 8) | lo


def info(p):
    d = open(p, 'rb').read()
    drv = parse(d)
    print(f"{os.path.basename(p)}: type {drv['type']} ({drv['type_name']}) size {drv['size']}  "
          f"rcode @{drv['rcode_addr']:04x}+{drv['rcode_len']:04x}  init API {drv['init_api']:#06x} "
          f"exit API {drv['exit_api']:#06x}  bankA@file {drv['bnka_off']:#x} bankB@file {drv['bnkb_off']:#x} trailer {drv['trailer']}")
    for bk in drv['banks']:
        base, _ = infer_base(bk)
        ents = [f'{api_word(drv, bk, i):#06x}->{e:04x}' for i, e in enumerate(bk['entries']) if e]
        print(f"   bank{bk['kind']}{bk['slot']:02d} file@{bk['file_off']:#06x} len={bk['len']:#05x} data={bk['data_len']:3d} "
              f"load={base:#06x}  " + ' '.join(ents) if base is not None else
              f"   bank{bk['kind']}{bk['slot']:02d} file@{bk['file_off']:#06x} len={bk['len']:#05x} (no entry)")
    return drv


def split(p, od):
    d = open(p, 'rb').read()
    drv = parse(d)
    os.makedirs(od, exist_ok=True)
    open(os.path.join(od, 'rcode.bin'), 'wb').write(drv['rcode'])
    meta = {k: v for k, v in drv.items() if k not in ('rcode', 'banks')}
    meta['banks'] = []
    for bk in drv['banks']:
        base, _ = infer_base(bk)
        fn = f"bank{bk['kind']}_{bk['slot']:02d}.bin"
        open(os.path.join(od, fn), 'wb').write(bk['bytes'])
        meta['banks'].append({k: v for k, v in bk.items() if k != 'bytes'} | dict(
            file=fn, load=base, apis={f'{api_word(drv, bk, i):#06x}': e for i, e in enumerate(bk['entries']) if e}))
    json.dump(meta, open(os.path.join(od, 'drv.json'), 'w'), indent=1)


if __name__ == '__main__':
    if sys.argv[1] == 'info':
        for p in sys.argv[2:]:
            info(p)
    elif sys.argv[1] == 'split':
        split(sys.argv[2], sys.argv[3])

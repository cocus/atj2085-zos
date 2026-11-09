"""Dump the ATJ2085's internal ROMs through the zos USB debug console (USB module, CDC serial).

usage: python romdump.py COMx [--bank 2] [--start 8000] [--len 5400] [-o trom_21k.bin]
       defaults: TROM (bank 2, 8000-D3FF = 21 KB). BROM: --bank 0 --len 2000 (compare with
       lisong_rebuild/analysis/brom_8k.bin to check the method).
Needs pyserial (pip install pyserial). Run the USB module on the player first (SHELL -> USB).
It sends "rom BANK ADDR 200" per 512-byte chunk and parses the hexdump lines "AAAA: xx xx ..". """
import re, sys, time, argparse
import serial

LINE = re.compile(r'^([0-9A-Fa-f]{4}): ((?:[0-9A-Fa-f]{2} ){1,16})')


def chunk(port, bank, addr, n):
    port.reset_input_buffer()
    port.write(f'rom {bank:x} {addr:x} {n:x}\r'.encode())
    got, buf, t0 = {}, b'', time.time()
    while len(got) < n and time.time() - t0 < 5:
        buf += port.read(4096)
        *lines, buf = buf.split(b'\n')
        for ln in lines:
            m = LINE.match(ln.decode('ascii', 'replace').strip())
            if m:
                a = int(m.group(1), 16)
                for i, h in enumerate(m.group(2).split()):
                    got[a + i] = int(h, 16)
    if any(addr + i not in got for i in range(n)):
        raise IOError(f'incomplete reply at {addr:04x} ({len(got)}/{n} bytes)')
    return bytes(got[addr + i] for i in range(n))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('port')
    ap.add_argument('--bank', type=lambda s: int(s, 16), default=2)
    ap.add_argument('--start', type=lambda s: int(s, 16), default=0x8000)
    ap.add_argument('--len', type=lambda s: int(s, 16), default=0x5400)
    ap.add_argument('-o', '--out', default=None)
    a = ap.parse_args()
    out = a.out or ('trom_21k.bin' if a.bank == 2 else f'rom_bank{a.bank}.bin')
    data = bytearray()
    with serial.Serial(a.port, 115200, timeout=0.2) as port:
        for addr in range(a.start, a.start + a.len, 512):
            n = min(512, a.start + a.len - addr)
            for tries in range(3):
                try:
                    data += chunk(port, a.bank, addr, n)
                    break
                except IOError as e:
                    if tries == 2:
                        sys.exit(str(e))
            print(f'\r{len(data)}/{a.len} bytes', end='', flush=True)
    open(out, 'wb').write(data)
    distinct = len(set(data))
    print(f'\n{out}: {len(data)} bytes, {distinct} distinct byte values'
          + ('  (looks empty/unmapped!)' if distinct < 3 else ''))


if __name__ == '__main__':
    main()

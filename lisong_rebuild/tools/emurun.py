"""Scripted run of the (snapshotted) ATJ2085 emulator.

usage: emurun.py FW OUTDIR "boot:320 key:NEXT png:menu wait:40 key:PLAY wait:80 png:list ..."
  boot:N / wait:N   run N million T-states
  key:NAME[:ms]     press a key (PLAY NEXT PREV MENU K06 ...), then run 30M
  png:name          save LCD to OUTDIR/name.png
Uses work/emu_snapshot (copy of atj2085emu) so concurrent edits there don't interfere.
"""
import sys, os
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'work', 'emu_snapshot'))
from emu.fwimage import FwImage
from emu.soc import ATJ2085
from emu import pngout


def main():
    fw, od, script = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(od, exist_ok=True)
    logf = open(os.path.join(od, 'run.log'), 'w')
    emu = ATJ2085(FwImage(fw), log=lambda s: logf.write(s + '\n'))
    M = 1_000_000

    def run(n):
        end = emu.ticks + n
        while emu.ticks < end:
            r = emu.run(min(M, end - emu.ticks))
            if r:
                print(f'stopped: {r} at t={emu.ticks / M:.1f}M sp={emu.m.sp:04x} hl={emu.m.hl:04x} de={emu.m.de:04x}')
                return False
        return True
    for tok in script.split():
        op, *a = tok.split(':')
        if op in ('boot', 'wait'):
            run(int(a[0]) * M)
        elif op == 'key':
            emu.press(a[0], int(a[1]) if len(a) > 1 else 150)
            run(30 * M)
        elif op == 'sys':           # sys:FROM_M:TO_M  dump syscalls in that tick window
            lo, hi = int(a[0]) * M, int(a[1]) * M
            for t, v, A, HL, DE, BC, c in list(emu.syscalls):
                if lo <= t <= hi:
                    print('  %9d rst%02x A=%02x HL=%04x DE=%04x BC=%04x from %04x' % (t, v, A, HL, DE, BC, c))
        elif op == 'fakefs':        # fakefs:FOLDER  serve FOLDER as the user disk (launcher calls only)
            import fakefs
            ffs = fakefs.FakeFS(emu, a[0])
            print('   fakefs:', ffs.files)
        elif op == 'fscalls':
            print('   fs calls:', ffs.calls)
        elif op == 'bp':            # bp:ADDR  stop and report when PC reaches ADDR
            a0 = int(a[0], 16); emu.user_bps.add(a0); emu.m.set_breakpoint(a0)
        elif op == 'mem':           # mem:ADDR:LEN  hex dump
            a0, n = int(a[0], 16), int(a[1])
            print('   mem', hex(a0), bytes(emu.m.memory[a0:a0 + n]).hex(' '))
        elif op == 'state':         # interrupt flag and OS di-nesting counter at 0x0037
            print(f'   state: iff1={emu.m.iff1} [0x0037]={emu.m.memory[0x37]} pc={emu.m.pc:04x}')
        elif op == 'png':
            pngout.save_lcd(emu.lcd, os.path.join(od, a[0] + '.png'))
        print(f'{tok:14s} t={emu.ticks / M:8.1f}M pc={emu.m.pc:04x}')
    calls = list(emu.syscalls)[-25:]
    for t, v, A, HL, DE, BC, c in calls:
        logf.write('  %9d rst%02x A=%02x HL=%04x DE=%04x BC=%04x from %04x\n' % (t, v, A, HL, DE, BC, c))


if __name__ == '__main__':
    main()

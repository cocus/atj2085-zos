"""Compare the sound effects in a DAC capture with the effect tables.

usage: snd_sfx_check.py RUNDIR   (RUNDIR from emu_game.py with 'EVT name T' lines in run.log: scenario blocks_sfx.py)

For every event the captured samples after T are cut into 10 ms frames; each frame's dominant frequency
(parabolic FFT peak, 8 kHz) is grouped into note segments (frequency within 4 %, at least 20 ms) and
printed next to the notes of the table (modules/quadfall/tunes.c fx_*). Also: peak amplitude, whether the
output returns to exact zero after the effect, and the click check (largest step between consecutive samples
at the start / end of a segment).
"""
import os, re, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
FS = 8000
NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']


def nn(n):                                   # note number -> (name, frequency)
    m = n + 35
    return '%s%d' % (NAMES[m % 12], m // 12 - 1), 440.0 * 2 ** ((m - 69) / 12)


def load_fx(path):
    txt = open(path).read()
    fx = {}
    for m in re.finditer(r'const uint8_t fx_(\w+)\[\] = \{(.*?)\};', txt, re.S):
        v = [int(x) for x in m.group(2).replace('\n', ' ').split(',') if x.strip()]
        trip = [(v[i], v[i + 1], v[i + 2] - 256 if v[i + 2] > 127 else v[i + 2]) for i in range(1, len(v) - 1, 3) if v[i] != 255]
        fx[m.group(1)] = (v[0], trip)
    return fx


def track(x, hop=80, win=160):
    out = []
    w = np.hanning(win)
    for i in range(0, len(x) - win, hop):
        seg = x[i:i + win]
        rms = np.sqrt(np.mean(seg.astype(np.float64) ** 2))
        if rms < 150:
            out.append((i, 0.0, rms))
            continue
        sp = np.abs(np.fft.rfft((seg - seg.mean()) * w, 8192))
        fr = np.fft.rfftfreq(8192, 1 / FS)
        sp[fr < 90] = 0
        k = int(np.argmax(sp))
        d = 0.0
        if 0 < k < len(sp) - 1:
            a, b, c = sp[k - 1], sp[k], sp[k + 1]
            d = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) else 0
        out.append((i, (k + d) * FS / 8192, rms))
    return out


def segments(tr):
    segs, cur = [], None
    for i, f, r in tr:
        if f == 0:
            cur = None
            continue
        if cur and abs(f / cur[1] - 1) < 0.04:
            cur[2] = i
            cur[3].append(f)
        else:
            cur = [i, f, i, [f]]
            segs.append(cur)
    return [(s[0], float(np.median(s[3])), s[2] - s[0] + 80, len(s[3])) for s in segs if len(s[3]) >= 2]


def main():
    run = sys.argv[1]
    fx = load_fx(os.path.join(HERE, '..', 'modules', 'quadfall', 'tunes.c'))
    d = np.load(os.path.join(run, 'dac_out.npz'))
    t, l = d['t'], d['l'].astype(np.int32)
    cpu = float(sys.argv[2]) * 1e6 if len(sys.argv) > 2 else 24.576e6
    ev = []
    for ln in open(os.path.join(run, 'run.log'), errors='replace'):
        m = re.match(r'EVT (\w+) (\d+)', ln)
        if m:
            ev.append((m.group(1), int(m.group(2))))
    want = {'move': 'move', 'rot': 'rot', 'lock': 'lock', 'hard': 'drop+lock', 'clear1': 'clear1', 'clear2': 'clear2',
            'clear3': 'clear3', 'clear4': 'clear4', 'levelup': 'clear1+level', 'gameover': 'over', 'menu_key': 'key'}
    key_fx = {'key': (0x24, [(60, 3, 0)])}
    out = []
    for i, (name, T) in enumerate(ev):
        if name not in want:
            continue
        T1 = ev[i + 1][1] if i + 1 < len(ev) else T + int(3 * cpu)
        lo = np.searchsorted(t, T)
        hi = np.searchsorted(t, min(T1, T + int(5 * cpu)))
        x = l[lo:hi]
        nz = np.nonzero(x)[0]
        out.append('== %s (expected: %s)' % (name, want[name]))
        if not len(nz):
            out.append('   silent!')
            continue
        on = nz[0]
        last = nz[-1]
        seg = x[on:last + 1]
        out.append('   first sound %.0f ms after the key, length %.0f ms, peak %d (%.1f dBFS)' % (
            (t[lo + on] - T) / cpu * 1e3, len(seg) / FS * 1e3, np.abs(seg).max(), 20 * np.log10(np.abs(seg).max() / 32768)))
        # click check: the largest step between neighbouring samples at the very start / end vs the peak
        out.append('   starts at %d, ends at %d, then output is %s; first |sample| %d, last |sample| %d' % (
            seg[0], seg[-1], 'exactly 0' if not x[last + 1:last + 400].any() else 'NOT zero', abs(seg[0]), abs(seg[-1])))
        ss = segments(track(seg.astype(np.float64)))
        got = ' '.join('%s@%dms/%dms' % ('%d Hz' % round(f), round(s / FS * 1e3), round(n / FS * 1e3)) for s, f, n, _ in ss)
        out.append('   measured: ' + got)
        if len(seg) < 700 or not ss:                      # a short blip / a slide: zero-crossing frequency of the first and last 12 ms
            def zc(y):
                sg = np.sign(y); sg[sg == 0] = 1
                return 0.5 * np.count_nonzero(np.diff(sg)) / (len(y) / FS)
            n12 = int(0.012 * FS)
            body = seg[8:]
            out.append('   zero crossings: first 12 ms %d Hz, middle %d Hz, last 12 ms %d Hz' % (
                zc(body[:n12]), zc(body[len(body) // 2 - n12 // 2:len(body) // 2 + n12 // 2]), zc(body[-n12:])))
        for tag in want[name].replace('+', ' ').split():
            if tag in fx or tag == 'key':
                amp, trip = fx.get(tag, key_fx['key'])
                exp = ' '.join('%s(%dHz)x%dms%s' % (nn(n)[0], round(nn(n)[1]), c * 10, '' if not sl else ' slide%+d' % (sl * 16)) if n else 'rest%dms' % (c * 10)
                               for n, c, sl in trip)
                out.append('   table %-7s amp %d: %s' % (tag, amp, exp))
    print('\n'.join(out))
    open(os.path.join(run, 'sfx_check.txt'), 'w').write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()

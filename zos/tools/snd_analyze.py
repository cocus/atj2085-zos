"""Analyse a DAC capture made with emu_game.py (dac_out.npz, pump_times.npy) against the tune data.

usage: snd_analyze.py RUNDIR TUNE(a|b|c) T0_TSTATES [--level N] [--cpu-mhz 24.576] [--tunes zos/modules/quadfall/tunes.c]

Prints: DAC FIFO stats (underruns / holds), longest gap between snd_pump() calls, amplitude, per-note pitch
error of melody and bass against the score, loop length. The note timeline is fitted to the capture
(start offset + sixteenth length) and the fitted sixteenth is compared with the programmed one.
"""
import os, re, sys, argparse
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
FS = 8000


def load_tunes(path):
    txt = open(path).read()
    arrs = {m.group(1): [int(x) for x in m.group(2).replace('\n', ' ').split(',') if x.strip()]
            for m in re.finditer(r'const uint8_t (\w+)\[\] = \{(.*?)\};', txt, re.S)}
    units = [int(x) for x in re.findall(r'\{ t_\w_mel, t_\w_bass, (\d+) \}', txt)]
    return arrs, units


def make_resolver(mdir, mod='quadfall'):
    """address -> 'function+off' for a page module's own source file (linker map + the object's .sym)"""
    mp = open(os.path.join(mdir, mod + '.map')).read()
    base = int(re.search(r'([0-9A-F]{8})\s+_beep', mp).group(1), 16)
    syms = []
    sp = os.path.join(mdir, '%s_%s_c.sym' % (mod, mod))     # (only present when the build compiled the file, not from its cache)
    for ln in (open(sp) if os.path.exists(sp) else []):
        m = re.match(r'\s+0 +(_[A-Za-z]\w*) +([0-9A-F]{8})', ln)
        if m and not m.group(1).startswith('__') and not m.group(1) in ('_zs_tab',):
            syms.append((int(m.group(2), 16), m.group(1)[1:]))
    syms.sort()

    def res(addr):
        o = addr - base
        best = None
        for off, nm in syms:
            if off <= o:
                best = (off, nm)
        if o < 0 or best is None:
            return '%04x' % addr
        return '%s+%d' % (best[1], o - best[0])
    return res


def f_of(n):
    return 440.0 * 2 ** ((n + 35 - 69) / 12)


def timeline(data, unit_s, t_start):
    """[(t0, t1, f)] of a voice list (pairs note, dur); one loop"""
    ev, t = [], t_start
    for i in range(0, len(data) - 1, 2):
        n, d = data[i], data[i + 1]
        if n == 255:
            break
        if n:
            ev.append((t, t + d * unit_s, f_of(n)))
        t += d * unit_s
    return ev, t


def peak_near(x, f, tol=0.06):
    """(frequency, prominence) of the strongest FFT peak within f*(1 +- tol) (parabolic interpolation);
    prominence = peak magnitude / median magnitude of the neighbourhood 0.7 f .. 1.4 f outside f * (1 +- 0.06)"""
    w = np.hanning(len(x))
    nfft = 1 << 15
    sp = np.abs(np.fft.rfft((x - x.mean()) * w, nfft))
    fr = np.fft.rfftfreq(nfft, 1 / FS)
    m = (fr > f * (1 - tol)) & (fr < f * (1 + tol))
    i = int(np.argmax(np.where(m, sp, 0)))
    if 0 < i < len(sp) - 1:
        a, b, c = sp[i - 1], sp[i], sp[i + 1]
        d = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) else 0.0
    else:
        d = 0.0
    fq = (i + d) * FS / nfft
    nb = (fr > 0.7 * f) & (fr < 1.4 * f) & ((fr < f * 0.94) | (fr > f * 1.06))
    return fq, sp[i] / max(np.median(sp[nb]), 1e-9), 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run'); ap.add_argument('tune'); ap.add_argument('t0', type=int)
    ap.add_argument('--level', type=int, default=0)
    ap.add_argument('--mapdir', default=os.path.join(HERE, '..', 'out', 'atj2085', 'mod_quadfall'))
    ap.add_argument('--cpu-mhz', type=float, default=24.576)
    ap.add_argument('--tunes', default=os.path.join(HERE, '..', 'modules', 'quadfall', 'tunes.c'))
    a = ap.parse_args()
    cpu = a.cpu_mhz * 1e6
    d = np.load(os.path.join(a.run, 'dac_out.npz'))
    t, l, r, u = d['t'], d['l'].astype(np.int32), d['r'].astype(np.int32), d['u']
    out = []
    P = out.append
    P('== DAC model: %d sample periods (%.2f s), %d with FIFO underrun' % (len(t), len(t) / FS, int(u.sum())))
    # underruns: runs of u==1; classify by the level just before (held nonzero = audible)
    runs, i = [], 0
    while i < len(u):
        if u[i]:
            j = i
            while j < len(u) and u[j]:
                j += 1
            runs.append((i, j))
            i = j
        else:
            i += 1
    bad = [(i, j) for i, j in runs if abs(l[i - 1] if i else 0) > 0 and i > 0]
    P('   underrun runs: %d; of them after nonzero output (audible hold): %d' % (len(runs), len(bad)))
    for i, j in bad[:15]:
        P('     at %.3f s (T=%d): %d sample periods held at %d' % (t[i] / cpu, t[i], j - i, l[i - 1]))
    P('   max |sample| = %d (%.1f dBFS); mono (L == R): %s' % (np.abs(l).max(), 20 * np.log10(max(1, np.abs(l).max()) / 32768),
                                                          bool((l == r).all())))
    res = make_resolver(a.mapdir)
    pt = os.path.join(a.run, 'pump_times.npy')
    if os.path.exists(pt):
        pa = np.load(pt)                                   # rows: T-state, LCD stores so far, caller of snd_pump
        pa = pa[pa[:, 0] > a.t0]
        p = pa[:, 0]
        # real device: every LCD store costs 3 wait states (dma_clock.md) that the emulator does not count
        ex = pa[:, 3] if pa.shape[1] > 3 else p            # end of each call: the FIFO is full then
        ok = (ex[:-1] > 0)
        g = (p[1:] - ex[:-1])[ok]                          # pump end -> next pump start: what the FIFO has to bridge
        ga = (p[1:] - ex[:-1] + 3 * (pa[1:, 1] - pa[:-1, 1]))[ok]          # + the LCD wait states in between
        pa = pa[:-1][ok]
        p = pa[:, 0]
        pn = np.load(pt)
        pn = pn[pn[:, 0] > a.t0]
        nxt = pn[1:][ok]
        P('== snd_pump() calls after T0: %d; gap from the end of one call to the start of the next: median %d, p99 %d, max %d T-states '
          '(%.3f ms at %.3f MHz)' % (len(p), np.median(g), np.percentile(g, 99), g.max(), g.max() / cpu * 1e3, a.cpu_mhz))
        fifo = 4 * cpu / FS
        P('   FIFO holds 8 words = 4 sample periods = %d T-states; gaps longer than that: %d (%.3f %%)' % (
            fifo, int((g > fifo).sum()), 100.0 * (g > fifo).mean()))
        P('   with the LCD wait states (+3 T per LCD store): max %d T-states (%.3f ms); gaps over the FIFO: %d' % (
            ga.max(), ga.max() / cpu * 1e3, int((ga > fifo).sum())))
        big = np.argsort(ga)[-8:][::-1]
        for k in big:
            P('     gap %d T-states (%.2f ms, adjusted %.2f ms) at T=%d: pump called from %s, next from %s' % (
                g[k], g[k] / cpu * 1e3, ga[k] / cpu * 1e3, p[k], res(pa[k, 2]), res(nxt[k, 2])))
    arrs, units = load_tunes(a.tunes)
    ti = 'abc'.index(a.tune)
    unit_q4 = units[ti]
    unit_s0 = (unit_q4 * 16 / (16 + a.level)) / 16 / 100.0        # sixteenth in seconds
    mel, bass = arrs['t_%s_mel' % a.tune], arrs['t_%s_bass' % a.tune]
    # music starts with the first nonzero sample at/after T0
    idx = np.nonzero((t >= a.t0) & ((l != 0) | (r != 0)))[0]
    i0 = idx[0]
    ts = t[i0] / cpu
    sig = l.astype(np.float64)
    P('== music: first nonzero sample at %.3f s (T=%d); programmed sixteenth %.4f s' % (ts, t[i0], unit_s0))
    # fit: sixteenth length and start offset maximising the melody match
    tsec = t / cpu
    t_rel = lambda s: np.searchsorted(tsec, s)

    def score(unit_s, off, voice, nnotes=64):
        ev, _ = timeline(voice, unit_s, ts + off)
        sc, n, ok = 0.0, 0, 0
        for (a0, a1, f) in ev[:nnotes]:
            if a1 > tsec[-1]:
                break
            lo, hi = t_rel(a0 + 0.35 * (a1 - a0)), t_rel(a0 + 0.65 * (a1 - a0))
            if hi - lo < 40:
                continue
            fq, mg, _ = peak_near(sig[lo:hi], f, 0.03)
            sc += min(np.log(max(mg, 1.0)), 4.0)
            ok += mg > 6
            n += 1
        return sc, ok, n

    best = (-1, None, None, 0, 0)
    for us in np.linspace(unit_s0 * 0.94, unit_s0 * 1.06, 61):
        for off in np.linspace(-0.04, 0.04, 9):
            sc, ok, n = score(us, off, mel, 56)
            if sc > best[0]:
                best = (sc, us, off, ok, n)
    P('   best fit: sixteenth %.5f s (%.2f %% of programmed), start offset %+.0f ms, melody notes with the programmed pitch (first %d): %d' % (
        best[1], 100 * best[1] / unit_s0, best[2] * 1e3, best[4], best[3]))
    us, off = best[1], best[2]
    for name, voice in (('melody', mel), ('bass', bass)):
        ev, tot = timeline(voice, us, ts + off)
        errs, miss = [], 0
        for (a0, a1, f) in ev:
            if a1 > tsec[-1]:
                break
            lo, hi = t_rel(a0 + 0.25 * (a1 - a0)), t_rel(a0 + 0.75 * (a1 - a0))
            if hi - lo < 64:
                continue
            fq, mg, med = peak_near(sig[lo:hi], f, 0.03)
            if mg < 6 or abs(fq / f - 1) > 0.015:
                miss += 1
                continue
            errs.append(1200 * np.log2(fq / f))
        errs = np.array(errs)
        P('== %s: %d notes checked, %d without a tone at the programmed pitch; pitch error vs the score: mean %+.1f cents, '
          'max |%.1f| cents' % (name, len(errs) + miss, miss, errs.mean(), np.abs(errs).max()))
        P('   loop length %.3f s' % (tot - (ts + off)))
    print('\n'.join(out))
    open(os.path.join(a.run, 'analysis.txt'), 'w').write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()

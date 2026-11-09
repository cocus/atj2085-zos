"""compare_wav.py OUT.wav REF.mp3 [REF2.mp3 ...]
Decode each reference MP3 with libsndfile (soundfile; independent of the pymp3 decoder used by the
DSP model), bring it to the WAV's rate/stereo, locate it in the emulator WAV by cross-correlation
and report offset, length match and SNR (dB) over the overlapping part (minus the last 60 ms)."""
import sys
import numpy as np
import soundfile as sf


def load_ref(path, rate):
    d, sr = sf.read(path, dtype='float64', always_2d=True)
    if d.shape[1] == 1:
        d = np.repeat(d, 2, axis=1)
    if sr != rate:
        n2 = int(round(len(d) * rate / sr))
        x = np.arange(n2) * (sr / rate)
        d = np.stack([np.interp(x, np.arange(len(d)), d[:, c]) for c in (0, 1)], 1)
    return d, sr


def main():
    w, rate = sf.read(sys.argv[1], dtype='float64', always_2d=True)
    print('%s: %d frames @ %d Hz (%.2f s)' % (sys.argv[1], len(w), rate, len(w) / rate))
    for ref in sys.argv[2:]:
        r, sr0 = load_ref(ref, rate)
        a = w[:, 0]
        b = r[:, 0]                                  # whole reference as probe (periodic test tones)
        n = 1 << int(np.ceil(np.log2(len(a) + len(b))))
        c = np.fft.irfft(np.fft.rfft(a, n) * np.conj(np.fft.rfft(b, n)), n)[:len(a)]
        off = int(np.argmax(c))
        seg = w[off:off + len(r)]
        # libsndfile (mpg123) and pymp3 differ by one frame at the end of a stream (the decoder
        # flush), and the emulator WAV continues with the next track there: skip the last 60 ms
        m = min(len(seg), len(r)) - int(0.06 * rate)
        err = seg[:m] - r[:m]
        snr = 10 * np.log10(np.sum(r[:m] ** 2) / max(np.sum(err ** 2), 1e-20))
        # allow a few samples of decoder delay difference: best SNR over +-1200 sample shifts
        best = (snr, 0)
        for d in range(-1200, 1201, 1):
            o = off + d
            if o < 0:
                continue
            s2 = w[o:o + m]
            if len(s2) < m:
                continue
            e2 = s2 - r[:m]
            v = 10 * np.log10(np.sum(r[:m] ** 2) / max(np.sum(e2 ** 2), 1e-20))
            if v > best[0]:
                best = (v, d)
        print('  %s (%d Hz): found at %.3f s, ref %.3f s, overlap %.3f s, SNR %.1f dB (best %.1f dB at %+d samples)' % (
            ref, sr0, off / rate, len(r) / rate, m / rate, snr, best[0], best[1]))


if __name__ == '__main__':
    main()

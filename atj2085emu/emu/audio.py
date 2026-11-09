"""PCM sink for the DSP model: optional WAV file and optional live output (sounddevice).

All PCM arrives as int16 numpy arrays shaped (n, 2) at the stream's sample rate. The WAV is
always written at `wav_rate` (default 44100 Hz) stereo; other rates are resampled linearly
(only the 22.05/11.025/... kHz streams are affected). Live output opens a stream at the
stream's own rate and is re-opened if the rate changes.
"""
import collections
import threading
import wave

import numpy as np


class AudioSink:
    def __init__(self, wav_path=None, live=False, wav_rate=44100, log=print):
        self.log = log
        self.wav_rate = wav_rate
        self.wav = None
        if wav_path:
            self.wav = wave.open(wav_path, 'wb')
            self.wav.setnchannels(2)
            self.wav.setsampwidth(2)
            self.wav.setframerate(wav_rate)
        self.frames_written = 0          # at wav_rate
        self.live = live
        self.stream = None
        self.rate = None
        self.q = collections.deque()
        self.q_frames = 0
        self.played = 0                  # frames consumed by the device (live)
        self.lock = threading.Lock()
        self.segments = []               # (start_frame_in_wav, rate, nframes) for tests

    # ------------------------------------------------------------------ live output
    def _open(self, rate):
        import sounddevice as sd
        if self.stream is not None:
            self.stream.stop(); self.stream.close()
        self.rate = rate
        self.stream = sd.OutputStream(samplerate=rate, channels=2, dtype='int16',
                                      callback=self._cb, blocksize=1024, latency='low')
        self.stream.start()

    def _cb(self, out, frames, t, status):
        with self.lock:
            i = 0
            while i < frames and self.q:
                blk = self.q[0]
                k = min(frames - i, len(blk))
                out[i:i + k] = blk[:k]
                if k == len(blk):
                    self.q.popleft()
                else:
                    self.q[0] = blk[k:]
                i += k
            self.q_frames -= i
            self.played += i
            if i < frames:
                out[i:] = 0

    def buffered_seconds(self):
        if not self.live or not self.rate:
            return 0.0
        return self.q_frames / self.rate

    # ------------------------------------------------------------------ input
    def write(self, pcm, rate):
        """pcm: int16 array (n, 2)."""
        if len(pcm) == 0:
            return
        if self.wav is not None:
            if rate != self.wav_rate:
                pcm2 = self._resample(pcm, rate)
            else:
                self._rs = None
                pcm2 = pcm
            self.segments.append((self.frames_written, rate, len(pcm2)))
            self.wav.writeframes(np.ascontiguousarray(pcm2, dtype='<i2').tobytes())
            self.frames_written += len(pcm2)
        if self.live:
            if rate != self.rate:
                self._open(rate)
            with self.lock:
                self.q.append(np.ascontiguousarray(pcm, dtype=np.int16))
                self.q_frames += len(pcm)

    def _resample(self, pcm, rate):
        """Continuous linear resampler (state kept across calls so chunk edges are seamless)."""
        st = getattr(self, '_rs', None)
        if st is None or st[0] != rate:
            st = [rate, 0.0, None]            # rate, next output position in source samples, last frame
        src = pcm.astype(np.float64)
        if st[2] is not None:
            src = np.vstack([st[2][None, :], src])
            base = -1.0
        else:
            base = 0.0
        step = rate / self.wav_rate
        pos = st[1]
        last = base + len(src) - 1             # last source index (relative to this chunk)
        n = int(np.floor((last - pos) / step)) + 1 if last >= pos else 0
        x = pos + np.arange(n) * step
        out = np.stack([np.interp(x - base, np.arange(len(src)), src[:, c]) for c in (0, 1)], 1)
        st[1] = pos + n * step - (len(pcm))     # carry into the next chunk's coordinates
        st[2] = pcm[-1].astype(np.float64)
        self._rs = st
        return np.round(out).astype(np.int16)

    def close(self):
        if self.wav is not None:
            self.wav.close()
            self.wav = None
        if self.stream is not None:
            self.stream.stop(); self.stream.close()
            self.stream = None

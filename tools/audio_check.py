"""Compare two renders of the same music, sample-aligned (music_stems.c
-audiocheck: trackNN_mix.wav from the RSP path, trackNN_dspmix.wav from
dsp_synth.c): overall and per-band level, and the residual.

    py -3 tools/audio_check.py A.wav B.wav
"""
import os, sys, wave
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '.tools', 'pylib'))
import numpy as np

def load(p):
    w = wave.open(p)
    x = np.frombuffer(w.readframes(w.getnframes()), np.int16).reshape(-1, w.getnchannels()).astype(float) / 32768
    return w.getframerate(), x

sr, a = load(sys.argv[1])
_, b = load(sys.argv[2])
n = min(len(a), len(b))
a, b = a[:n], b[:n]
db = lambda v: 10 * np.log10(np.mean(v * v) + 1e-20)
print('level  A %.2f dB  B %.2f dB   (L %.2f/%.2f, R %.2f/%.2f)' % (db(a), db(b), db(a[:, 0]), db(b[:, 0]), db(a[:, 1]), db(b[:, 1])))
print('residual B-A %.2f dB below A' % (db(a) - db(b - a)))
# best lag (B late by k samples?)
m = a.mean(1)[: sr * 10]; q = b.mean(1)[: sr * 10]
c = np.fft.irfft(np.fft.rfft(m, 2 * len(m)) * np.conj(np.fft.rfft(q, 2 * len(q))))
k = int(np.argmax(np.abs(c)))
k = k if k < len(m) else k - 2 * len(m)
print('lag of best match:', -k, 'samples')
edges = [20, 60, 120, 250, 500, 1000, 2000, 4000, 8000, 11025]
nf = 8192
def spec(x):
    x = x.mean(1)
    segs = [x[i:i + nf] * np.hanning(nf) for i in range(0, len(x) - nf, nf // 2)]
    return np.mean([np.abs(np.fft.rfft(s)) ** 2 for s in segs], 0)
f = np.fft.rfftfreq(nf, 1 / sr)
sa, sb = spec(a), spec(b)
for lo, hi in zip(edges, edges[1:]):
    sel = (f >= lo) & (f < hi)
    print('%5d-%5d Hz: %+.2f dB' % (lo, hi, 10 * np.log10(sb[sel].sum() / sa[sel].sum())))

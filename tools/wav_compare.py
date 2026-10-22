"""Compare WAV recordings: level, stereo balance, band energies.

    py -3 tools/wav_compare.py A.wav B.wav ...
"""
import os, sys, wave
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '.tools', 'pylib'))
import numpy as np

bands = [(0, 200), (200, 800), (800, 3000), (3000, 8000), (8000, 20000)]
for path in sys.argv[1:]:
    w = wave.open(path)
    sr, ch = w.getframerate(), w.getnchannels()
    x = np.frombuffer(w.readframes(w.getnframes()), np.int16).reshape(-1, ch).astype(np.float64) / 32768
    l, r = x[:, 0], x[:, 1]
    rms = lambda v: 20 * np.log10(np.sqrt(np.mean(v * v)) + 1e-12)
    m = (l + r) / 2
    spec = np.abs(np.fft.rfft(m * np.hanning(len(m)))) ** 2
    f = np.fft.rfftfreq(len(m), 1 / sr)
    tot = spec.sum()
    be = ' '.join('%d-%d:%5.1f%%' % (a, b, 100 * spec[(f >= a) & (f < b)].sum() / tot) for a, b in bands)
    # short-term loudness spread (activity)
    blk = sr // 10
    st = [rms(m[i:i + blk]) for i in range(0, len(m) - blk, blk)]
    print('%s: rms L %.1f R %.1f dB, peak %.2f, 100ms-rms p10/p50/p90 %.0f/%.0f/%.0f, %s' % (
        os.path.basename(path), rms(l), rms(r), np.abs(x).max(), *np.percentile(st, [10, 50, 90]), be))

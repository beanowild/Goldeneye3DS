"""How alike two recordings of the same (deterministic) audio are: the
best-aligned correlation of their spectrograms, per 2 s window.

    py -3 tools/wav_similarity.py REF.wav TEST.wav
"""
import os, sys, wave
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '.tools', 'pylib'))
import numpy as np

def spec(p):
    w = wave.open(p)
    sr = w.getframerate()
    x = np.frombuffer(w.readframes(w.getnframes()), np.int16).reshape(-1, w.getnchannels()).mean(1) / 32768
    x = x[::4]; sr //= 4                       # 24 kHz is plenty
    n, hop = 1024, 256
    frames = np.array([np.abs(np.fft.rfft(x[i:i + n] * np.hanning(n)))[:300] for i in range(0, len(x) - n, hop)])
    return np.log1p(frames * 50), sr / hop

a, fps = spec(sys.argv[1])
b, _ = spec(sys.argv[2])
def corr(u, v):
    u = u - u.mean(); v = v - v.mean()
    return float((u * v).sum() / (np.sqrt((u * u).sum() * (v * v).sum()) + 1e-12))
# global alignment: B shifted by k frames
best = max(range(-200, 201), key=lambda k: corr(a[max(0, k):len(a) + min(0, k)][:len(b)], b[max(0, -k):][:len(a[max(0, k):])]))
k = best
A = a[max(0, k):]; B = b[max(0, -k):]; m = min(len(A), len(B)); A, B = A[:m], B[:m]
print('offset %.0f ms, overall %.3f' % (k * 1000 / fps, corr(A, B)))
win = int(2 * fps)
print('per 2 s:', ' '.join('%.2f' % corr(A[i:i + win], B[i:i + win]) for i in range(0, m - win, win)))

"""Analise espectral rapida de WAVs (noise floor por banda). Uso: python analyze_wav.py a.wav b.wav ..."""
import sys
import numpy as np
from scipy import signal
from scipy.io import wavfile

for fn in sys.argv[1:]:
    sr, x = wavfile.read(fn)
    x = x.astype(np.float64)
    tag = ""
    if x.ndim > 1:
        tag = f" ch={x.shape[1]} L==R:{np.allclose(x[:, 0], x[:, 1])}"
        x = x[:, 0]
    x = x[int(0.3 * sr):]
    x -= x.mean()
    f, p = signal.welch(x, sr, nperseg=8192)
    df = f[1] - f[0]
    b = lambda lo, hi: 10 * np.log10(np.sum(p[(f >= lo) & (f < hi)]) * df + 1e-20)
    print(f"{fn}{tag}")
    print("  rms %.1f dBFS | peak %.1f dBFS" % (20 * np.log10(np.sqrt(np.mean(x ** 2)) + 1e-12), 20 * np.log10(np.abs(x).max() + 1e-12)))
    print("  bandas: 20-200 %.1f | 200-2k %.1f | 2k-8k %.1f | 8k-12k %.1f | 12k-20k %.1f" % (b(20, 200), b(200, 2000), b(2000, 8000), b(8000, 12000), b(12000, 20000)))
    line = []
    for c in [60, 120, 250, 500, 1000, 2000, 4000, 6000, 7500, 8500, 10000, 14000, 18000]:
        m = (f > c / 1.12) & (f < c * 1.12)
        line.append("%d:%.0f" % (c, 10 * np.log10(np.mean(p[m]) + 1e-30)))
    print("  PSD dB/Hz  " + "  ".join(line))

"""Grava o que esta saindo nos fones da UM2 (WASAPI loopback do render) e mede o ruido. Uso: python measure_monitor.py <seg> <tag>"""
import sys, time, numpy as np
import pyaudiowpatch as pa
from scipy import signal
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 5
tag = sys.argv[2] if len(sys.argv) > 2 else "mon"
p = pa.PyAudio()
dev = next(d for d in p.get_loopback_device_info_generator() if "USB Audio CODEC" in d["name"])
sr = int(dev["defaultSampleRate"]); ch = dev["maxInputChannels"]
frames = []
st = p.open(format=pa.paFloat32, channels=ch, rate=sr, input=True, input_device_index=dev["index"], frames_per_buffer=1024,
            stream_callback=lambda d, n, t, s: (frames.append(np.frombuffer(d, np.float32).copy()), pa.paContinue))
time.sleep(secs); st.stop_stream(); st.close(); p.terminate()
x = np.concatenate(frames).reshape(-1, ch)[int(0.3 * sr):, 0].astype(float)
if len(x) < sr:
    print(f"{tag}: pouco audio capturado ({len(x)} amostras) — device em silencio total/sem stream"); sys.exit()
x -= x.mean()
f, pp = signal.welch(x, sr, nperseg=8192); df = f[1] - f[0]
b = lambda lo, hi: 10 * np.log10(np.sum(pp[(f >= lo) & (f < hi)]) * df + 1e-20)
print(f"{tag}: rms {20*np.log10(np.sqrt((x**2).mean())+1e-12):.1f} dBFS | peak {20*np.log10(abs(x).max()+1e-12):.1f} | "
      f"20-200 {b(20,200):.1f} | 200-2k {b(200,2000):.1f} | 2k-8k {b(2000,8000):.1f} | 8k-20k {b(8000,20000):.1f} | silencio digital {np.mean(abs(x)<1e-7)*100:.0f}%")

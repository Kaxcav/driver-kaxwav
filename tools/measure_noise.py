"""Mede o noise floor da UM2 (raw) vs saida processada (CABLE Output) ao mesmo tempo.
Uso: python measure_noise.py [segundos] [tag]
"""
import sys, threading, json, time
from pathlib import Path
import numpy as np
import sounddevice as sd
import soundfile as sf
from scipy import signal

SR = 48000
SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 6.0
TAG = sys.argv[2] if len(sys.argv) > 2 else time.strftime("%H%M%S")
OUT = Path(__file__).parent / "measurements"
OUT.mkdir(exist_ok=True)

def find(name_part, inputs=True):
    for i, d in enumerate(sd.query_devices()):
        if "WASAPI" in sd.query_hostapis(d["hostapi"])["name"] and name_part.lower() in d["name"].lower():
            if (d["max_input_channels"] if inputs else d["max_output_channels"]) > 0:
                return i
    return None

def rec(dev, store, key):
    try:
        x = sd.rec(int(SECS * SR), samplerate=SR, channels=2, dtype="float32", device=dev, blocking=True)
        store[key] = x
    except Exception as e:
        store[key + "_err"] = str(e)

def a_weight(f):
    f2 = f ** 2
    ra = (12194 ** 2 * f2 ** 2) / ((f2 + 20.6 ** 2) * np.sqrt((f2 + 107.7 ** 2) * (f2 + 737.9 ** 2)) * (f2 + 12194 ** 2))
    return ra * 10 ** (2.0 / 20)

def analyze(x, SR=SR):
    x = x - np.mean(x)
    rms = np.sqrt(np.mean(x ** 2)) + 1e-12
    f, p = signal.welch(x, SR, nperseg=8192)
    aw = a_weight(np.maximum(f, 1)) ** 2
    rms_a = np.sqrt(np.sum(p * aw) * (f[1] - f[0])) + 1e-12
    def band(lo, hi):
        m = (f >= lo) & (f < hi)
        return 10 * np.log10(np.sum(p[m]) * (f[1] - f[0]) + 1e-20)
    pk = {}
    for h in (50, 60, 100, 120, 150, 180, 1000, 2000):
        m = (f > h - 3) & (f < h + 3)
        loc = (f > h - 40) & (f < h + 40) & ~m
        pk[h] = float(round(10 * np.log10((np.max(p[m]) + 1e-30) / (np.median(p[loc]) + 1e-30)), 1))
    return {
        "rms_dbfs": float(round(20 * np.log10(rms), 1)),
        "rms_A_dbfs": float(round(20 * np.log10(rms_a), 1)),
        "peak_dbfs": float(round(20 * np.log10(np.max(np.abs(x)) + 1e-12), 1)),
        "band_20_200": float(round(band(20, 200), 1)),
        "band_200_2k": float(round(band(200, 2000), 1)),
        "band_2k_8k": float(round(band(2000, 8000), 1)),
        "band_8k_20k": float(round(band(8000, 20000), 1)),
        "tone_over_floor_db": pk,
    }

raw_dev = find("USB Audio CODEC")
cab_dev = find("CABLE Output")
store = {}
bufs = {}
rates = {}
streams = []
def mk(key, dev):
    bufs[key] = []
    def cb(indata, frames, t, status):
        bufs[key].append(indata.copy())
    rates[key] = int(sd.query_devices(dev)["default_samplerate"])
    return sd.InputStream(device=dev, samplerate=rates[key], channels=2, dtype="float32", callback=cb,
                          extra_settings=sd.WasapiSettings(exclusive=False))
targets = [("raw", raw_dev)]
if cab_dev is not None and "--raw-only" not in sys.argv:
    targets.append(("cable", cab_dev))
for key, dev in targets:
    try:
        st = mk(key, dev); st.start(); streams.append(st)
    except Exception as e:
        store[key + "_err"] = str(e)
time.sleep(SECS + 0.5)
for st in streams:
    st.stop(); st.close()
for key in bufs:
    if bufs[key]:
        x = np.concatenate(bufs[key])
        store[key] = x[int(0.3 * rates[key]):]  # descarta transiente de abertura
res = {"tag": TAG, "secs": SECS}
for key in ("raw", "cable"):
    if key in store:
        x = store[key]
        sr_k = rates.get(key, SR)
        sf.write(OUT / f"{TAG}_{key}.wav", x, sr_k, subtype="FLOAT")
        res[key] = {"sr": sr_k, "ch1_mic": analyze(x[:, 0], sr_k), "ch2_inst": analyze(x[:, 1], sr_k)}
    if key + "_err" in store:
        res[key + "_err"] = store[key + "_err"]
print(json.dumps(res, indent=1))
(OUT / f"{TAG}.json").write_text(json.dumps(res, indent=1))

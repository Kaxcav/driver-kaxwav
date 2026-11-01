"""Signal generation and analysis for the KaxWav harness (M1-M4, M6, M7). numpy/scipy only."""
import sys
from pathlib import Path
import numpy as np
from scipy import signal

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
from measure_noise import a_weight, analyze as legacy_noise_analyze  # noqa: E402  (reuse, tools/measure_noise.py)

FS = 48000


# ---------------- I/O ----------------
def load_i16(path, channels=2):
    x = np.fromfile(path, dtype="<i2")
    return x.reshape(-1, channels) if channels > 1 else x


def load_f32(path, channels):
    x = np.fromfile(path, dtype="<f4")
    return x.reshape(-1, channels) if channels > 1 else x


def read_wav_float(path):
    from scipy.io import wavfile
    sr, x = wavfile.read(path)
    return sr, x.astype(np.float64)


def write_i16(path, x):
    np.asarray(x, dtype="<i2").tofile(path)


def to_i16(x):
    return np.clip(np.round(np.asarray(x) * 32767.0), -32768, 32767).astype(np.int16)


def dbfs(x):
    return float(20 * np.log10(np.sqrt(np.mean(np.square(x, dtype=np.float64))) + 1e-20))


# ---------------- stimuli ----------------
def log_chirp(dur=0.04, f0=200.0, f1=12000.0, amp_db=-12.0, fs=FS):
    t = np.arange(int(dur * fs)) / fs
    x = signal.chirp(t, f0, dur, f1, method="logarithmic") * signal.windows.tukey(len(t), 0.2)
    return x * 10 ** (amp_db / 20)


def pseudo_noise(dur, amp_db=-20.0, seed=2902, fs=FS):
    rng = np.random.default_rng(seed)
    x = rng.standard_normal(int(dur * fs))
    x /= np.sqrt(np.mean(x ** 2))
    return np.clip(x * 10 ** (amp_db / 20), -0.99, 0.99)


def ess(dur=5.0, f0=20.0, f1=20000.0, amp_db=-12.0, fs=FS):
    """Exponential sine sweep (Farina) and its inverse filter."""
    n = int(dur * fs)
    t = np.arange(n) / fs
    L = dur / np.log(f1 / f0)
    x = np.sin(2 * np.pi * f0 * L * (np.exp(t / L) - 1)) * 10 ** (amp_db / 20)
    fade = int(0.01 * fs)
    w = np.ones(n)
    w[:fade] = np.linspace(0, 1, fade)
    w[-fade:] = np.linspace(1, 0, fade)
    x *= w
    inv = x[::-1] * np.exp(-t / L)
    return x, inv


def tone(freq=1000.0, dur=2.0, amp_db=-12.0, fs=FS):
    t = np.arange(int(dur * fs)) / fs
    x = np.sin(2 * np.pi * freq * t) * 10 ** (amp_db / 20)
    r = int(0.01 * fs)
    x[:r] *= np.linspace(0, 1, r)
    x[-r:] *= np.linspace(1, 0, r)
    return x


# ---------------- M1: latency by cross-correlation ----------------
def xcorr_lag(rec, ref, max_lag=None):
    """Lag (samples, sub-sample by parabolic fit) of ref inside rec, plus a peak/median quality ratio."""
    rec = np.asarray(rec, dtype=np.float64)
    ref = np.asarray(ref, dtype=np.float64)
    n = len(rec) + len(ref)
    nfft = 1 << int(np.ceil(np.log2(n)))
    c = np.fft.irfft(np.fft.rfft(rec, nfft) * np.conj(np.fft.rfft(ref, nfft)), nfft)[: len(rec)]
    if max_lag is not None:
        c = c[: max_lag + 1]
    a = np.abs(c)
    k = int(np.argmax(a))
    frac = 0.0
    if 0 < k < len(a) - 1:
        y0, y1, y2 = a[k - 1], a[k], a[k + 1]
        d = y0 - 2 * y1 + y2
        frac = 0.5 * (y0 - y2) / d if d != 0 else 0.0
    quality = float(a[k] / (np.median(a) + 1e-20))
    return k + frac, quality, bool(c[k] < 0)


# ---------------- M2: noise floor ----------------
def psd(x, fs=FS, nperseg=65536):
    x = np.asarray(x, dtype=np.float64)
    x = x - x.mean()
    return signal.welch(x, fs, window="hann", nperseg=min(nperseg, len(x)), noverlap=None, detrend=False)


def band_mean_db(f, p, lo, hi):
    m = (f >= lo) & (f < hi)
    return float(10 * np.log10(np.mean(p[m]) + 1e-30))


def step_7k(f, p, drop_db=6.0):
    """Flags the Voice Clarity signature: PSD 7.5-12 kHz more than drop_db below 4-6.5 kHz."""
    lo = band_mean_db(f, p, 4000, 6500)
    hi = band_mean_db(f, p, 7500, 12000)
    return {"psd_4k_6k5_db_hz": round(lo, 1), "psd_7k5_12k_db_hz": round(hi, 1),
            "drop_db": round(lo - hi, 1), "step_detected": bool(lo - hi > drop_db)}


def noise_floor(x, fs=FS):
    """M2 metrics: broadband dBFS RMS, PSD (Welch, FFT 65536) in dB/Hz, legacy band metrics."""
    x = np.asarray(x, dtype=np.float64)
    f, p = psd(x, fs)
    m = (f >= 20) & (f <= 20000)
    out = {
        "rms_dbfs_broadband": round(dbfs(x - x.mean()), 2),
        "psd_median_20_20k_db_hz": round(float(10 * np.log10(np.median(p[m]) + 1e-30)), 1),
        "psd_bands_db_hz": {f"{lo}-{hi}": round(band_mean_db(f, p, lo, hi), 1)
                            for lo, hi in [(20, 200), (200, 2000), (2000, 8000), (8000, 12000), (12000, 20000)]},
        "step_7k": step_7k(f, p),
        "legacy": legacy_noise_analyze(x.astype(np.float32), fs, nperseg=65536),
    }
    return out


# ---------------- M3/M4 ----------------
def sweep_response(rec, sweep, inv, fs=FS):
    """Magnitude response from an ESS recording, normalised to 1 kHz, 1/6-octave smoothed."""
    ir = signal.fftconvolve(np.asarray(rec, dtype=np.float64), inv)
    k = int(np.argmax(np.abs(ir)))
    seg = ir[max(0, k - 2048): k + 16384] * signal.windows.tukey(min(len(ir), k + 16384) - max(0, k - 2048), 0.05)
    H = np.fft.rfft(seg, 1 << 17)
    f = np.fft.rfftfreq(1 << 17, 1 / fs)
    mag = 20 * np.log10(np.abs(H) + 1e-20)
    centers = 1000 * 2 ** (np.arange(-36, 25) / 6.0)  # ~16 Hz .. 16 kHz in 1/6 octave
    centers = centers[(centers >= 20) & (centers <= 20000)]
    sm = []
    for c in centers:
        m = (f >= c / 2 ** (1 / 12)) & (f < c * 2 ** (1 / 12))
        sm.append(float(np.mean(mag[m])) if m.any() else float("nan"))
    sm = np.array(sm)
    ref = np.interp(1000, centers, sm)
    resp = sm - ref

    def edge(side):
        idx = np.where(resp < -3)[0]
        if side == "low":
            below = idx[centers[idx] < 1000]
            return float(centers[below.max()]) if below.size else None
        above = idx[centers[idx] > 1000]
        return float(centers[above.min()]) if above.size else None

    band = (centers >= 4000) & (centers <= 6500)
    hi = (centers >= 7500) & (centers <= 12000)
    drop = float(np.nanmean(resp[band]) - np.nanmean(resp[hi]))
    return {
        "points_hz_db": [[round(float(c), 1), round(float(r), 2)] for c, r in zip(centers, resp)],
        "minus3db_low_hz": edge("low"), "minus3db_high_hz": edge("high"),
        "ripple_100_10k_db": round(float(np.nanmax(resp[(centers >= 100) & (centers <= 10000)]) -
                                         np.nanmin(resp[(centers >= 100) & (centers <= 10000)])), 2),
        "step_7k": {"drop_db": round(drop, 1), "step_detected": bool(drop > 6.0)},
    }


def thd_n(x, f0=1000.0, fs=FS):
    """THD+N: least-squares fit of the fundamental, residual band-limited 20 Hz-20 kHz."""
    x = np.asarray(x, dtype=np.float64)
    x = x[int(0.2 * len(x)): int(0.8 * len(x))]
    x = x - x.mean()
    t = np.arange(len(x)) / fs
    # refine frequency (clock offset) with a small grid search
    best = None
    for df in np.linspace(-2, 2, 81):
        A = np.column_stack([np.sin(2 * np.pi * (f0 + df) * t), np.cos(2 * np.pi * (f0 + df) * t)])
        coef, *_ = np.linalg.lstsq(A, x, rcond=None)
        r = x - A @ coef
        e = float(np.sum(r ** 2))
        if best is None or e < best[0]:
            best = (e, df, coef, r)
    _, df, coef, r = best
    sos = signal.butter(4, [20, 20000], btype="band", fs=fs, output="sos")
    r = signal.sosfiltfilt(sos, r)
    fund_rms = float(np.hypot(*coef) / np.sqrt(2))
    res_rms = float(np.sqrt(np.mean(r ** 2)))
    ratio = res_rms / (fund_rms + 1e-20)
    return {"fundamental_dbfs": round(20 * np.log10(fund_rms + 1e-20), 2), "freq_offset_hz": round(float(df), 3),
            "thd_n_percent": round(100 * ratio, 5), "thd_n_db": round(20 * np.log10(ratio + 1e-20), 2)}


# ---------------- M6 ----------------
def drift_from_csv(path, window_s=300.0):
    import csv
    rows = []
    with open(path, newline="") as fh:
        for line in fh:
            if line.startswith("#") or line.startswith("t_qpc_s"):
                continue
            rows.append([float(v) for v in next(csv.reader([line]))])
    if len(rows) < 3:
        return None
    a = np.array(rows)
    t, cps, cqs, rps, rqs = a[:, 0], a[:, 1], a[:, 2], a[:, 3], a[:, 4]

    def ppm(pos, q):
        slope = np.polyfit(q - q[0], pos - pos[0], 1)[0]
        return (slope - 1.0) * 1e6

    full = {"ppm_input": round(ppm(cps, cqs), 3), "ppm_output": round(ppm(rps, rqs), 3)}
    full["ppm_differential"] = round(full["ppm_input"] - full["ppm_output"], 3)
    # cross-check with GetBuffer packet positions (device position vs packet QPC)
    pk = a[:, 6] > 0
    if pk.sum() > 2:
        full["ppm_input_packet_qpc"] = round(ppm(a[pk, 5] / FS, a[pk, 6]), 3)
    curve = []
    edges = np.arange(0, t[-1] + 1e-9, window_s)
    for t0 in edges:
        m = (t >= t0) & (t < t0 + window_s)
        if m.sum() >= 10:
            pi, po = ppm(cps[m], cqs[m]), ppm(rps[m], rqs[m])
            curve.append({"t0_s": int(t0), "ppm_input": round(pi, 3), "ppm_output": round(po, 3),
                          "ppm_differential": round(pi - po, 3)})
    # accumulated offset between the two streams, in samples (what a duplex host would have to absorb)
    acc = ((cps - cps[0]) - (rps - rps[0])) * FS
    return {"duration_s": round(float(t[-1]), 1), "samples": len(rows), **full,
            "differential_accumulated_samples_at_end": round(float(acc[-1] - acc[0]), 2),
            "discontinuities": int(a[-1, 9]), "timeouts": int(a[-1, 10]),
            "curve_window_s": window_s, "curve": curve,
            "reference_clock": "QPC (ppm de cada stream relativo ao QPC; o diferencial independe do QPC)"}


# ---------------- M7 ----------------
def null_test(digital_ref_i16, captured_f32_mono, scale=32768.0, max_search=96000):
    """Aligns a float capture to an int16 reference and reports the integer residual."""
    ref = np.asarray(digital_ref_i16, dtype=np.float64)
    cap = np.asarray(captured_f32_mono, dtype=np.float64) * scale
    if len(cap) < len(ref) // 2 or np.max(np.abs(cap)) < 1e-9:
        return {"aligned": False, "reason": "captura vazia ou curta"}
    lag, q, inv = xcorr_lag(cap, ref, max_lag=min(max_search, max(0, len(cap) - 1)))
    k = int(round(lag))
    n = min(len(ref), len(cap) - k)
    if n <= 0:
        return {"aligned": False, "reason": "sem sobreposicao"}
    a, b = ref[:n], cap[k:k + n]
    act = np.abs(a) > 0
    g = float(np.dot(a, b) / (np.dot(a, a) + 1e-20))
    resid = np.round(b) - a
    return {"aligned": True, "lag_samples": round(float(lag), 2), "correlation_quality": round(q, 1),
            "compared_samples": int(n), "nonzero_ref_samples": int(act.sum()),
            "residual_max_lsb": float(np.max(np.abs(resid))), "residual_rms_lsb": round(float(np.sqrt(np.mean(resid ** 2))), 4),
            "residual_dbfs": round(dbfs(resid / scale), 2), "best_fit_gain_db": round(20 * np.log10(abs(g) + 1e-20), 4),
            "identical_samples_percent": round(100.0 * float(np.mean(resid == 0)), 3),
            "bit_perfect": bool(np.all(resid == 0))}

"""M2 side by side: white floor vs mains hum for several RAW captures.

    python measure/scripts/m2_table.py "rotulo=caminho/m2_raw.wav" ...

Same analysis as run_baseline M2 (Welch FFT 65536, hum_analysis); prints Markdown.
"""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import um2_analysis as A  # noqa: E402


def row(label, path):
    sr, x = A.read_wav_float(path)
    x = x[int(0.3 * sr):]
    x = x if x.ndim == 1 else x[:, 0]
    f, p = A.psd(x, sr)
    h = A.hum_analysis(f, p)
    df = f[1] - f[0]
    band = (f >= 60) & (f < 500)

    def line(hz):
        v = h[f"hum_{hz}hz_dbfs"]
        return f"{'-' if v is None else v} ({h[f'hum_{hz}hz_over_floor_db']:+})"

    return (f"| {label} | {A.dbfs(x - x.mean()):.1f} | {h['white_floor_db_hz']} | {h['white_floor_20_20k_dbfs']} | "
            f"{line(60)} | {line(120)} | {line(180)} | {h['hum_total_dbfs']} | {h['hum_share_percent']} | "
            f"{10 * np.log10(np.sum(p[band]) * df + 1e-30):.1f} | {20 * np.log10(np.max(np.abs(x)) + 1e-12):.1f} |")


def main():
    print("| condição | RMS total dBFS | piso branco dB/Hz | piso branco 20 Hz–20 kHz dBFS | 60 Hz dBFS (acima do piso, dB) | "
          "120 Hz | 180 Hz | hum 60–300 Hz total dBFS | hum % da potência | banda 60–500 Hz dBFS | pico dBFS |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for arg in sys.argv[1:]:
        label, _, path = arg.partition("=")
        print(row(label, path))


if __name__ == "__main__":
    main()

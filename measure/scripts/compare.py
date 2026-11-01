"""Before/after table between two baseline JSONs (measure/baselines/*.json).

    python measure/scripts/compare.py <antes.json> <depois.json> [--steps m5,m2,m7]

Prints Markdown, ready for a commit body. Missing metrics show as '-'.
"""
import argparse
import json
from pathlib import Path


def g(d, *path, default="-"):
    for k in path:
        if not isinstance(d, dict) or k not in d or d[k] is None:
            return default
        d = d[k]
    return d


def fmt(v):
    if isinstance(v, float):
        return f"{v:.2f}".rstrip("0").rstrip(".")
    return str(v)


def table(head, rows):
    out = ["| " + " | ".join(head) + " |", "|" + "---|" * len(head)]
    out += ["| " + " | ".join(fmt(c) for c in r) + " |" for r in rows]
    return "\n".join(out)


def m5(a, b):
    def idx(doc):
        return {(r["mode_requested"], r["buffer"]): r for r in g(doc, "M5", "runs", default=[])}
    A, B = idx(a), idx(b)
    rows = []
    for key in sorted(set(A) | set(B), key=lambda k: (k[0], k[1])):
        for tag, d in (("antes", A.get(key, {})), ("depois", B.get(key, {}))):
            t = d.get("telemetry") or {}
            n = d.get("asio_input_ch1_noise") or {}
            ok = d.get("ok", "-")
            active = d.get("mode_active", "-") if ok is not False else "recusado"
            pdc = f"{d.get('pdc_in', '-')}/{d.get('pdc_out', '-')}" if ok is not False else "-"
            rows.append([f"{key[0]} {key[1]}", tag, ok, active, pdc, t.get("block_us_p50", "-"),
                         t.get("block_us_p99", "-"), t.get("block_us_max", "-"), t.get("deadline_misses", "-"),
                         n.get("rms_dbfs", "-"), g(n, "step_7k", "step_detected"), cbmin(d), t.get("max_gap_between_blocks_us", "-")])
    return ("### M5\n" + f"carga: antes = {g(a, 'M5', 'load')} · depois = {g(b, 'M5', 'load')}\n\n" +
            table(["modo/buffer", "", "ok", "ativo", "PDC in/out", "p50 us", "p99 us", "max us", "misses",
                   "ASIO in1 dBFS", "degrau 7k", "callbacks/min (esperado)", "maior intervalo us"], rows))


def cbmin(d):
    """callbacks per minute vs expected; computed from the run if the JSON predates M5b."""
    if not d.get("ok") or not d.get("callbacks"):
        return "-"
    secs, buf = d.get("seconds", 60), d.get("buffer")
    got, exp = d["callbacks"] * 60 / secs, 48000 * 60 / buf
    return f"{got:.0f} ({exp:.0f}, {got / exp:.3f})"


def m2(a, b):
    rows = []
    for kind in ("raw", "apo"):
        for tag, d in (("antes", a), ("depois", b)):
            r = g(d, "M2", kind, "ch1_mic", default={})
            h = r.get("hum", {}) if isinstance(r, dict) else {}
            rows.append([kind, tag, r.get("rms_dbfs_broadband", "-"), r.get("psd_median_20_20k_db_hz", "-"),
                         h.get("white_floor_20_20k_dbfs", "-"), h.get("hum_60hz_dbfs", "-"), h.get("hum_120hz_dbfs", "-"),
                         h.get("hum_180hz_dbfs", "-"), h.get("hum_share_percent", "-"), g(r, "step_7k", "step_detected")])
    return "### M2 (canal 1)\n" + table(["stream", "", "RMS dBFS", "PSD med dB/Hz", "branco dBFS", "60 Hz",
                                         "120 Hz", "180 Hz", "hum % pot.", "degrau 7k"], rows)


def m7(a, b):
    rows = []
    for tag, d in (("antes", a), ("depois", b)):
        rows.append([tag, g(d, "M7", "mode_active"), g(d, "M7", "output_null", "bit_perfect"),
                     g(d, "M7", "output_null", "identical_samples_percent"),
                     g(d, "M7", "input_null", "ch1", "conclusive"), g(d, "M7", "input_null", "ch1", "bit_perfect"),
                     g(d, "M7", "input_null", "ch1", "input_level_dbfs")])
    return "### M7 (SharedFlex 256)\n" + table(["", "ativo", "saída bit-perfect", "% idênticas",
                                                "entrada conclusiva", "entrada bit-perfect", "nível tap dBFS"], rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("antes")
    ap.add_argument("depois")
    ap.add_argument("--steps", default="m5,m2,m7")
    a = ap.parse_args()
    A = json.loads(Path(a.antes).read_text(encoding="utf-8"))
    B = json.loads(Path(a.depois).read_text(encoding="utf-8"))
    print(f"antes: {A.get('run_id')} ({A.get('describe')}) · depois: {B.get('run_id')} ({B.get('describe')})\n")
    for s in a.steps.split(","):
        print({"m5": m5, "m2": m2, "m7": m7}[s.strip()](A, B) + "\n")


if __name__ == "__main__":
    main()

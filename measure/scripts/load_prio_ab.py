"""ExclusivePro under the same synthetic load at different load priority classes (DIAGNOSTIC).
If the render starvation disappears when the load runs below normal, something in the render path runs at
normal-ish priority and loses to the load.

  python measure/scripts/load_prio_ab.py --dll build/ab/X.dll --buffers 256,144 --reps 2 --seconds 30
"""
import argparse
import datetime as dt
import json
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_baseline as RB  # noqa: E402

CLASSES = {"normal": 0x00000020, "belownormal": 0x00004000, "idle": 0x00000040}


def burners(n, cls):
    return [subprocess.Popen([sys.executable, "-c", "while True: pass"], creationflags=CLASSES[cls]) for _ in range(n)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dll", required=True)
    ap.add_argument("--mode", type=int, default=0)
    ap.add_argument("--buffers", default="256,144")
    ap.add_argument("--classes", default="normal,belownormal,idle")
    ap.add_argument("--reps", type=int, default=2)
    ap.add_argument("--seconds", type=int, default=30)
    ap.add_argument("--cpu-load", type=int, default=12)
    ap.add_argument("--run-id", required=True)
    a = ap.parse_args()
    RB.DLL = Path(a.dll).resolve()
    rundir = RB.REPO / "measure" / "out" / a.run_id
    rundir.mkdir(parents=True, exist_ok=True)
    reg_before = RB.reg_read()
    runs = []
    try:
        for k in range(a.reps):
            for cls in a.classes.split(","):
                for buf in [int(b) for b in a.buffers.split(",")]:
                    bs = burners(a.cpu_load, cls)
                    try:
                        time.sleep(3)
                        j = RB.probe(a.mode, buf, a.seconds, rundir, f"lp_{cls}_{buf}_{k + 1}", poll_telemetry=True)
                    finally:
                        for b in bs:
                            b.kill()
                    r = {"load_class": cls, "buffer": buf, "rep": k + 1, "seconds": a.seconds, "ok": j.get("ok"),
                         "mode_active": j.get("mode_active"), "callbacks": j.get("blocks"),
                         "pdc_in": j.get("input_latency"), "pdc_out": j.get("output_latency"),
                         "telemetry": j.get("_telemetry"), "threads": j.get("_threads")}
                    r["punctuality"] = RB.punctuality(r, j)
                    runs.append(r)
                    p = r["punctuality"] or {}
                    print(f"  {cls}@{buf}#{k + 1}: cb/min={p.get('callbacks_per_min')} ratio={p.get('ratio')} "
                          f"gap={p.get('max_gap_us')}", flush=True)
                    time.sleep(2)
    finally:
        RB.reg_restore(reg_before)
    doc = {"run_id": a.run_id, "date": dt.datetime.now().isoformat(timespec="seconds"),
           "commit": RB.git("rev-parse", "--short", "HEAD"), "dll": str(RB.DLL), "dll_md5": RB.md5(RB.DLL),
           "load": f"{a.cpu_load} busy-loops por classe de prioridade", "order": "intercalado", "runs": runs}
    out = RB.REPO / "measure" / "baselines" / f"{a.run_id}.json"
    out.write_text(json.dumps(doc, indent=1, ensure_ascii=False, default=str), encoding="utf-8")
    print(f"load-prio: {out}")


if __name__ == "__main__":
    main()

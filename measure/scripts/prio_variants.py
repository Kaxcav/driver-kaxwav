"""Real priority of the driver's audio thread per variant (DIAGNOSTIC). Interleaved runs of N DLLs under the
same CPU load; the audio thread is identified by the 'tid=' the driver logs right after AvSetMm...;
priorities are sampled from outside (NtQueryInformationThread) every 0.5 s.

  python measure/scripts/prio_variants.py --dll V1=build/ab/V1.dll --dll V2=... --reps 2 --seconds 20 --cpu-load 12
"""
import argparse
import datetime as dt
import json
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_baseline as RB  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dll", action="append", required=True, help="LABEL=path")
    ap.add_argument("--mode", type=int, default=0)
    ap.add_argument("--buffer", type=int, default=256)
    ap.add_argument("--seconds", type=int, default=20)
    ap.add_argument("--reps", type=int, default=2)
    ap.add_argument("--cpu-load", type=int, default=12)
    ap.add_argument("--run-id", required=True)
    a = ap.parse_args()
    variants = [tuple(x.split("=", 1)) for x in a.dll]
    rundir = RB.REPO / "measure" / "out" / a.run_id
    rundir.mkdir(parents=True, exist_ok=True)
    reg_before = RB.reg_read()
    burners = RB.cpu_burners(a.cpu_load)
    runs = []
    try:
        time.sleep(3)
        for k in range(a.reps):
            for label, dll in variants:
                RB.DLL = Path(dll).resolve()
                j = RB.probe(a.mode, a.buffer, a.seconds, rundir, f"prio_{label}_{k + 1}", poll_telemetry=True)
                log = (j.get("_driver_tid_log") or [""])[-1]
                m = re.search(r"tid=(\d+)", log)
                tid = int(m.group(1)) if m else None
                threads = j.get("_threads") or []
                audio = next((t for t in threads if t["tid"] == tid), None)
                r = {"variant": label, "rep": k + 1, "audio_tid": tid, "log": log,
                     "audio_thread": audio, "other_high_threads": [t for t in threads if t["tid"] != tid],
                     "callbacks": j.get("blocks"), "seconds": a.seconds,
                     "callbacks_per_min": round((j.get("blocks") or 0) * 60 / a.seconds, 1)}
                runs.append(r)
                print(f"  {label}#{k + 1}: tid={tid} audio={audio and audio['current_base']} "
                      f"cb/min={r['callbacks_per_min']} outros={[(t['start'], t['current_base']) for t in r['other_high_threads']]}",
                      flush=True)
    finally:
        for b in burners:
            b.kill()
        RB.reg_restore(reg_before)
    doc = {"run_id": a.run_id, "date": dt.datetime.now().isoformat(timespec="seconds"),
           "commit": RB.git("rev-parse", "--short", "HEAD"),
           "variants": {lbl: {"dll": d, "md5": RB.md5(Path(d))} for lbl, d in variants},
           "load": f"{a.cpu_load} busy-loops", "mode": RB.MODES[a.mode], "buffer": a.buffer,
           "note": "prioridade atual/base amostrada por NtQueryInformationThread a cada 0,5 s",
           "runs": runs}
    out = RB.REPO / "measure" / "baselines" / f"{a.run_id}.json"
    out.write_text(json.dumps(doc, indent=1, ensure_ascii=False, default=str), encoding="utf-8")
    print(f"prio: {out}")


if __name__ == "__main__":
    main()

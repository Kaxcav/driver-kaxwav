"""ExclusivePro engine-loop diagnosis (DIAGNOSTIC ONLY): idle vs CPU load, per buffer, DiagCounters=1.

Runs the driver through um2_asio_probe with the UM2Diag section enabled and stores, per case: M5 telemetry,
M5b punctuality and the loop counters (events, pending capture, render padding, bufferSwitch per wake,
zero-fill/underflow, FIFO drops, capture packet age vs the device timestamp).

  python measure/scripts/diag_exclusive.py --cases idle:256,load:256,idle:144,load:144 --seconds 60
"""
import argparse
import datetime as dt
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_baseline as RB  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cases", default="idle:256,load:256,idle:144,load:144")
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--cpu-load", type=int, default=12)
    ap.add_argument("--dll", help="DLL por caminho (default build/Release)")
    ap.add_argument("--run-id", default=None)
    args = ap.parse_args()
    if args.dll:
        RB.DLL = Path(args.dll).resolve()

    run_id = args.run_id or f"{dt.date.today().isoformat()}-diag-ep-{RB.git('rev-parse', '--short', 'HEAD')}"
    rundir = RB.REPO / "measure" / "out" / run_id
    rundir.mkdir(parents=True, exist_ok=True)
    reg_before = RB.reg_read()
    runs = []
    try:
        RB.reg_restore({"DiagCounters": 1})
        for case in [c.strip() for c in args.cases.split(",") if c.strip()]:
            cond, _, buf = case.partition(":")
            buf = int(buf)
            burners = RB.cpu_burners(args.cpu_load) if cond == "load" else []
            try:
                if burners:
                    time.sleep(3)
                j = RB.probe(0, buf, args.seconds, rundir, f"diag_{cond}_{buf}", poll_telemetry=True)
            finally:
                for b in burners:
                    b.kill()
            r = {"case": case, "load": f"{args.cpu_load} busy-loops" if burners else "ociosa", "buffer": buf,
                 "seconds": args.seconds, "ok": j.get("ok"), "mode_active": j.get("mode_active"),
                 "pdc_in": j.get("input_latency"), "pdc_out": j.get("output_latency"), "callbacks": j.get("blocks"),
                 "telemetry": j.get("_telemetry"), "diag": j.get("_diag"), "driver_log": j.get("driver_log")}
            r["punctuality"] = RB.punctuality(r, j)
            runs.append(r)
            d = r["diag"] or {}
            p = r["punctuality"] or {}
            print(f"  {case}: cb/min={p.get('callbacks_per_min')} ratio={p.get('ratio')} wakes={d.get('wakes')} "
                  f"ren={d.get('renderServiced')} cap={d.get('captureServiced')} renWrites={d.get('renWrites')} "
                  f"underflowBlk={d.get('capUnderflowBlocks')} drops={d.get('capDropFrames')} "
                  f"disc={d.get('capDiscontinuity')} renInt_p99={(d.get('renderIntervalHist') or {}).get('p99_us')}",
                  flush=True)
            time.sleep(2)
    finally:
        RB.reg_restore(reg_before)
    doc = {"run_id": run_id, "date": dt.datetime.now().isoformat(timespec="seconds"),
           "commit": RB.git("rev-parse", "--short", "HEAD"), "dll": str(RB.DLL), "dll_md5": RB.md5(RB.DLL),
           "note": "diagnostico (DiagCounters=1); nao e baseline de desempenho", "runs": runs}
    out = RB.REPO / "measure" / "baselines" / f"{run_id}.json"
    out.write_text(json.dumps(doc, indent=1, ensure_ascii=False, default=str), encoding="utf-8")
    print(f"diag: {out}")


if __name__ == "__main__":
    main()

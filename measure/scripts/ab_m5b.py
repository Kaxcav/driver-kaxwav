"""M5b A/B interleaved (A, B, A, B, ...) under the same CPU load - protocol rule for any result that decides.

Each run loads one driver DLL by path (or the installed one via COM with --x-com) through um2_asio_probe and
records callbacks/min vs expected, the longest gap between blocks (M5 telemetry), deadline misses and,
when the driver exposes it, the ExclusivePro diagnostic section (um2_ipc.read_diag).

  python measure/scripts/ab_m5b.py --a build/ab/A.dll --b build/ab/B.dll --mode 0 --buffer 256 --reps 4 --cpu-load 12
"""
import argparse
import datetime as dt
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_baseline as RB  # noqa: E402
import um2_ipc  # noqa: E402


def one(label, dll, use_com, mode, buf, secs, rundir, tag):
    RB.DLL = Path(dll).resolve() if dll else RB.DLL
    RB.USE_COM = use_com

    j = RB.probe(mode, buf, secs, rundir, tag, poll_telemetry=True)
    r = {"variant": label, "dll": str(RB.DLL), "load": j.get("load"), "loaded_path": j.get("loaded_path"),
         "driver_version": j.get("driver_version"), "mode_requested": RB.MODES[mode], "mode_active": j.get("mode_active"),
         "buffer": buf, "seconds": secs, "ok": j.get("ok"), "pdc_in": j.get("input_latency"),
         "pdc_out": j.get("output_latency"), "callbacks": j.get("blocks"), "telemetry": j.get("_telemetry"),
         "diag": j.get("_diag"), "threads": j.get("_threads"), "driver_tid_log": j.get("_driver_tid_log"),
         "mmcss_log": [l for l in (j.get("driver_log") or []) if "MMCSS" in l or "AvSetMm" in l]}
    r["punctuality"] = RB.punctuality(r, j)
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--a", help="DLL da variante A (por caminho)")
    ap.add_argument("--b", help="DLL da variante B (por caminho)")
    ap.add_argument("--label-a", default="A")
    ap.add_argument("--label-b", default="B")
    ap.add_argument("--a-com", action="store_true", help="variante A carregada pelo COM (DLL instalada)")
    ap.add_argument("--b-com", action="store_true", help="variante B carregada pelo COM (DLL instalada)")
    ap.add_argument("--mode", type=int, default=0)
    ap.add_argument("--buffer", type=int, default=256)
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--reps", type=int, default=4)
    ap.add_argument("--cpu-load", type=int, default=12)
    ap.add_argument("--run-id", default=None)
    args = ap.parse_args()

    run_id = args.run_id or f"{dt.date.today().isoformat()}-ab-{RB.git('rev-parse', '--short', 'HEAD')}"
    rundir = RB.REPO / "measure" / "out" / run_id
    rundir.mkdir(parents=True, exist_ok=True)
    reg_before = RB.reg_read()
    burners = RB.cpu_burners(args.cpu_load)
    runs = []
    try:
        time.sleep(3)  # let the load settle
        for k in range(args.reps):
            for label, dll, com in ((args.label_a, args.a, args.a_com), (args.label_b, args.b, args.b_com)):
                tag = f"ab_{label}_{k + 1}_{RB.MODES[args.mode]}_{args.buffer}"
                r = one(label, dll, com, args.mode, args.buffer, args.seconds, rundir, tag)
                runs.append(r)
                p = r["punctuality"] or {}
                print(f"  {label}#{k + 1}: active={r['mode_active']} cb/min={p.get('callbacks_per_min')} "
                      f"ratio={p.get('ratio')} gap={p.get('max_gap_us')}us misses="
                      f"{(r['telemetry'] or {}).get('deadline_misses')} {r['mmcss_log'][:1]}", flush=True)
    finally:
        for b in burners:
            b.kill()
        RB.reg_restore(reg_before)
    doc = {"run_id": run_id, "date": dt.datetime.now().isoformat(timespec="seconds"),
           "commit": RB.git("rev-parse", "--short", "HEAD"),
           "load": f"{args.cpu_load} processos em busy-loop (prioridade normal)", "order": "intercalado A,B,A,B,...",
           "variants": {args.label_a: {"dll": args.a, "md5": RB.md5(Path(args.a)) if args.a else None, "com": args.a_com},
                        args.label_b: {"dll": args.b, "md5": RB.md5(Path(args.b)) if args.b else None, "com": args.b_com}},
           "runs": runs}
    out = RB.REPO / "measure" / "baselines" / f"{run_id}.json"
    out.write_text(json.dumps(doc, indent=1, ensure_ascii=False, default=str), encoding="utf-8")
    print(f"ab: {out}")


if __name__ == "__main__":
    main()

"""Is the ExclusivePro render starvation a trap (stays after the load goes away)? DIAGNOSTIC ONLY.
Needs the diag build (branch diag/ep-cadence) with DiagCounters=1: render-event intervals come from the wake ring.

  python measure/scripts/metastable.py --dll build/ab/C_diag.dll --pattern load-idle --seconds 45 --switch-at 15
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
import um2_ipc  # noqa: E402


def render_timeline(d):
    n = min(int(d.writeIndex), um2_ipc.DIAG_RING)
    start = int(d.writeIndex) - n
    w = sorted((d.ring[(start + i) % um2_ipc.DIAG_RING] for i in range(n)), key=lambda e: e.qpc)
    f = float(d.qpcFrequency)
    ren = [(e.qpc, e.renPadding) for e in w if e.flags & 1]
    return [((b[0] - a[0]) / f * 1e3, b[1]) for a, b in zip(ren, ren[1:])], (w[0].qpc, w[-1].qpc, f)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dll", required=True)
    ap.add_argument("--pattern", choices=["load-idle", "idle-load"], required=True)
    ap.add_argument("--buffer", type=int, default=256)
    ap.add_argument("--seconds", type=int, default=45)
    ap.add_argument("--switch-at", type=float, default=15)
    ap.add_argument("--cpu-load", type=int, default=12)
    ap.add_argument("--run-id", required=True)
    a = ap.parse_args()
    reg_before = RB.reg_read()
    RB.reg_restore({"DiagCounters": 1, "DriverMode": 0, "BufferSize": a.buffer})
    burners = []
    snaps = []
    try:
        if a.pattern == "load-idle":
            burners = RB.cpu_burners(a.cpu_load)
            time.sleep(3)
        p = subprocess.Popen([str(RB.PROBE), "--dll", str(Path(a.dll).resolve()), "--mode", "0", "--buffer", str(a.buffer),
                              "--seconds", str(a.seconds)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        t0 = time.time()
        switched = False
        while p.poll() is None:
            el = time.time() - t0
            if not switched and el >= a.switch_at:
                if a.pattern == "load-idle":
                    for b in burners:
                        b.kill()
                    burners = []
                else:
                    burners = RB.cpu_burners(a.cpu_load)
                switched = True
            d = um2_ipc.read_diag()
            if d is not None and d.ownerPid == p.pid and el > 2 and int(el) % 5 == 0:
                tl, _ = render_timeline(d)
                if tl:
                    recent = tl[-180:]
                    snaps.append({"t_s": round(el, 1), "render_interval_ms_mean": round(sum(x for x, _ in recent) / len(recent), 2),
                                  "padding0_frac": round(sum(1 for _, pd in recent if pd == 0) / len(recent), 3),
                                  "renderServiced": d.renderServiced, "bufferSwitches": d.bufferSwitches})
                    print(snaps[-1], flush=True)
            time.sleep(1.0 if int(el) % 5 != 4 else 0.2)
        out, _ = p.communicate()
    finally:
        for b in burners:
            b.kill()
        RB.reg_restore(reg_before)
    doc = {"run_id": a.run_id, "date": dt.datetime.now().isoformat(timespec="seconds"), "pattern": a.pattern,
           "switch_at_s": a.switch_at, "buffer": a.buffer, "dll": a.dll, "snapshots": snaps}
    outp = RB.REPO / "measure" / "baselines" / f"{a.run_id}.json"
    outp.write_text(json.dumps(doc, indent=1), encoding="utf-8")
    print("meta:", outp)


if __name__ == "__main__":
    main()

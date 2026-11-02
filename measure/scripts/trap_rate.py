"""ExclusivePro render trap: how often a start under load gets stuck, and whether a mid-run hiccup traps it.

Each trial launches um2_asio_probe (EP) for --seconds; the callback rate of the LAST --tail seconds is taken from
the M5 telemetry ring (block start times), so a trap that begins mid-run is seen. Optional hiccup: the driver's
audio thread (TID from the driver log, "tid=") is suspended for --inject-ms at --inject-at seconds.

  python measure/scripts/trap_rate.py --dll X.dll --trials 12 --buffer 256 --cpu-load 12 --run-id ID
  python measure/scripts/trap_rate.py --dll X.dll --trials 6 --start idle --inject-ms 30 --run-id ID
"""
import argparse
import ctypes
import datetime as dt
import json
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_baseline as RB  # noqa: E402
import um2_ipc  # noqa: E402

k32 = ctypes.windll.kernel32
k32.OpenThread.restype = ctypes.c_void_p


def suspend(tid, ms):
    h = k32.OpenThread(0x0002, False, tid)  # THREAD_SUSPEND_RESUME
    if not h:
        return False
    k32.SuspendThread(ctypes.c_void_p(h))
    time.sleep(ms / 1000.0)
    k32.ResumeThread(ctypes.c_void_p(h))
    k32.CloseHandle(ctypes.c_void_p(h))
    return True


def tail_rate(t, tail_s, buf):
    """callbacks per minute over the last tail_s seconds, from the telemetry ring."""
    if t is None:
        return None
    n = min(int(t.writeIndex), um2_ipc.RING)
    starts = sorted(t.ring[i].qpcStart for i in range(n))
    if len(starts) < 2:
        return None
    f = float(t.qpcFrequency)
    end = starts[-1]
    lo = end - int(tail_s * f)
    sel = [s for s in starts if s >= lo]
    span = (sel[-1] - sel[0]) / f if len(sel) > 1 else 0
    return round((len(sel) - 1) / span * 60, 1) if span > 0 else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dll", required=True)
    ap.add_argument("--buffer", type=int, default=256)
    ap.add_argument("--trials", type=int, default=12)
    ap.add_argument("--seconds", type=float, default=12)
    ap.add_argument("--tail", type=float, default=3.0)
    ap.add_argument("--cpu-load", type=int, default=12)
    ap.add_argument("--start", choices=["load", "idle"], default="load", help="carga ja ligada na partida?")
    ap.add_argument("--inject-ms", type=float, default=0)
    ap.add_argument("--inject-at", type=float, default=5)
    ap.add_argument("--run-id", required=True)
    a = ap.parse_args()
    dll = Path(a.dll).resolve()
    expected = 48000 / a.buffer * 60
    reg_before = RB.reg_read()
    trials = []
    burners = RB.cpu_burners(a.cpu_load) if a.start == "load" else []
    try:
        if burners:
            time.sleep(3)
        for i in range(a.trials):
            p = subprocess.Popen([str(RB.PROBE), "--dll", str(dll), "--mode", "0", "--buffer", str(a.buffer),
                                  "--seconds", str(a.seconds)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            t0 = time.time()
            last = None
            injected = None
            early_rate = None
            added = []
            while p.poll() is None:
                el = time.time() - t0
                t = um2_ipc.read_telemetry()
                if t is not None and t.ownerPid == p.pid:
                    last = t
                if a.start == "idle" and not added and el >= a.inject_at - 1.5:
                    added = RB.cpu_burners(a.cpu_load)  # load arrives mid-run
                if a.inject_ms and injected is None and el >= a.inject_at:
                    early_rate = tail_rate(last, 2.0, a.buffer)
                    m = [l for l in RB.tail_driver_log(60) if "tid=" in l]
                    tid = int(re.search(r"tid=(\d+)", m[-1]).group(1)) if m else None
                    injected = {"tid": tid, "ms": a.inject_ms, "ok": bool(tid and suspend(tid, a.inject_ms)),
                                "rate_before_per_min": early_rate}
                time.sleep(0.25)
            p.communicate()
            for b in added:
                b.kill()
            j = {"trial": i + 1, "tail_rate_per_min": tail_rate(last, a.tail, a.buffer), "expected_per_min": expected,
                 "blocks": last.blocks if last else None, "injected": injected}
            j["trapped"] = bool(j["tail_rate_per_min"] is not None and j["tail_rate_per_min"] < 0.9 * expected)
            trials.append(j)
            print(f"  #{i + 1}: tail={j['tail_rate_per_min']}/min trapped={j['trapped']} inj={injected}", flush=True)
            time.sleep(1.5)
    finally:
        for b in burners:
            b.kill()
        RB.reg_restore(reg_before)
    doc = {"run_id": a.run_id, "date": dt.datetime.now().isoformat(timespec="seconds"),
           "commit": RB.git("rev-parse", "--short", "HEAD"), "dll": str(dll), "dll_md5": RB.md5(dll),
           "buffer": a.buffer, "start": a.start, "cpu_load": a.cpu_load, "inject_ms": a.inject_ms,
           "trapped": sum(1 for t in trials if t["trapped"]), "trials": trials}
    out = RB.REPO / "measure" / "baselines" / f"{a.run_id}.json"
    out.write_text(json.dumps(doc, indent=1), encoding="utf-8")
    print(f"trap: {doc['trapped']}/{len(trials)} -> {out}")


if __name__ == "__main__":
    main()

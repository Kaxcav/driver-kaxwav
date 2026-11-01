"""Every thread in a WPR ETL: switch-ins, priority, ready->run latency (DIAGNOSTIC ONLY).
Finds which threads wait long in Ready under load (the ones losing to the load).

  python measure/scripts/etl_threads.py trace.etl [--min-switch 1000] [--json out.json]
"""
import argparse
import json
import struct
import sys
from collections import Counter, defaultdict

from etl.etl import build_from_stream, ChunkParser

G_PROCESS, G_THREAD = 3, 5


def gi(g):
    return getattr(g, "intvalue", g)


def pct(xs, q):
    if not xs:
        return None
    s = sorted(xs)
    return round(s[min(len(s) - 1, int(q * (len(s) - 1)))], 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("etl")
    ap.add_argument("--min-switch", type=int, default=1000)
    ap.add_argument("--qpc-freq", type=float, default=1e7)
    ap.add_argument("--json")
    a = ap.parse_args()
    ready = defaultdict(list)
    sw = defaultdict(list)
    prio = defaultdict(Counter)
    tid_pid, pid_name = {}, {}
    with open(a.etl, "rb") as f:
        etl = build_from_stream(f.read())
    for chunk in etl.chunks:
        for ev in ChunkParser.parse(chunk.payload):
            v = ev.value
            if ev.type == "SystemTraceRecord":
                h, ts, data = v.system_header.header, v.system_header.system_time, v.mof_data
            elif ev.type == "PerfInfoTraceRecord":
                h, ts, data = v.header, v.timestamp, v.mof_data
            else:
                continue
            g, t = gi(h.group), h.type
            if g == G_THREAD:
                if t == 36 and len(data) >= 11:
                    nt, _ot, np_ = struct.unpack_from("<IIb", data, 0)
                    sw[nt].append(ts)
                    prio[nt][np_] += 1
                elif t == 50 and len(data) >= 4:
                    ready[struct.unpack_from("<I", data, 0)[0]].append(ts)
                elif t in (1, 3) and len(data) >= 8:
                    p, tid = struct.unpack_from("<II", data, 0)
                    tid_pid[tid] = p
            elif g == G_PROCESS and t in (1, 3) and len(data) > 60:
                p = struct.unpack_from("<I", data, 8)[0]
                try:
                    sid_off = 52
                    name_off = sid_off + 8 + 4 * data[sid_off + 1]
                    pid_name[p] = data[name_off:data.index(b"\x00", name_off)].decode("ascii", "replace")
                except (IndexError, ValueError):
                    pass
    us = 1e6 / a.qpc_freq
    rows = []
    for tid, s in sw.items():
        if len(s) < a.min_switch:
            continue
        s.sort()
        r = sorted(ready.get(tid, []))
        lat = []
        j = 0
        import bisect
        for x in r:
            k = bisect.bisect_left(s, x)
            if k < len(s):
                lat.append((s[k] - x) * us)
        span = (s[-1] - s[0]) * us / 1e6 if len(s) > 1 else 1
        p = tid_pid.get(tid)
        rows.append({"tid": tid, "pid": p, "proc": pid_name.get(p), "switch_ins_per_s": round(len(s) / span, 1),
                     "prio": dict(prio[tid].most_common(3)),
                     "ready_to_run_us_p50": pct(lat, .5), "p99": pct(lat, .99), "max": round(max(lat), 1) if lat else None,
                     "over_1ms": sum(1 for x in lat if x > 1000), "total_ready_ms": round(sum(lat) / 1000, 1)})
    rows.sort(key=lambda r: -(r["total_ready_ms"] or 0))
    for r in rows[:40]:
        print(json.dumps(r))
    if a.json:
        with open(a.json, "w", encoding="utf-8") as fo:
            json.dump(rows, fo, indent=1)


if __name__ == "__main__":
    sys.exit(main())

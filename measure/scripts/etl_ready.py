"""Who readies a thread, from a WPR ETL (measure/wpr/um2_sched.wprp). DIAGNOSTIC ONLY.

Same attribution WPA uses for "ReadyingThread": a ReadyThread event logged on CPU c at time t was issued
by (a) the DPC/ISR running on c at t, if any, else (b) the thread running on c at t (last CSwitch on c).
Per target thread it also reports switch-in priority, ready->run latency and the readiers.

Kernel payloads unpacked by hand (Windows kernel trace MOF classes, x64):
  CSwitch (Thread 36): NewTid u32, OldTid u32, NewPrio i8, OldPrio i8, PrevCState u8, Spare i8, WaitReason i8, ...
  ReadyThread (Thread 50): Tid u32, AdjustReason i8, AdjustIncrement i8, Flag i8
  Thread Start/DCStart (Thread 1/3): ProcessId u32, Tid u32, ...
  Process Start/DCStart (Process 1/3, v4): ..., ProcessId u32 @8, ..., TOKEN_USER @36, ImageFileName (ascii)
  DPC/TimerDPC/ThreadedDPC (PerfInfo 68/69/66): InitialTime u64, Routine ptr; event time = end
  ISR/ISR-MSI (PerfInfo 67/50): InitialTime u64, Routine ptr, ...
  Image Load/DCStart (Image 10/3, v3): ImageBase ptr, ImageSize ptr, ProcessId u32, ..., FileName wstr @56

  python measure/scripts/etl_ready.py trace.etl --pid 1234 --tid audio=5678 --tid kse=9012 [--auto-prio 20] --json out.json
"""
import argparse
import bisect
import json
import struct
import sys
from array import array
from collections import Counter, defaultdict

from etl.etl import build_from_stream, ChunkParser

G_PROCESS, G_THREAD, G_IMAGE, G_PERFINFO = 3, 5, 20, 15


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
    ap.add_argument("--pid", type=int, required=True, help="processo do host (probe)")
    ap.add_argument("--tid", action="append", default=[], help="label=tid")
    ap.add_argument("--auto-prio", type=int, default=20, help="tambem analisa threads do pid com prioridade >= N")
    ap.add_argument("--qpc-freq", type=float, default=1e7)
    ap.add_argument("--json")
    a = ap.parse_args()
    labels = {int(v): k for k, v in (x.split("=", 1) for x in a.tid)}

    cs_ts = defaultdict(lambda: array("q"))
    cs_tid = defaultdict(lambda: array("I"))
    dpc = defaultdict(list)            # cpu -> [(start, end, routine, kind)]
    readies = []                       # (ts, cpu, tid)
    switch_in = defaultdict(list)      # tid -> [(ts, prio)]
    tid_pid, pid_name = {}, {}
    images = []                        # (base, size, name) kernel modules
    n = 0

    with open(a.etl, "rb") as f:
        etl = build_from_stream(f.read())
    for chunk in etl.chunks:
        cpu = chunk.header.wnode.client_context.processor_index
        for ev in ChunkParser.parse(chunk.payload):
            n += 1
            v = ev.value
            if ev.type == "SystemTraceRecord":
                h = v.system_header.header
                ts, data = v.system_header.system_time, v.mof_data
            elif ev.type == "PerfInfoTraceRecord":
                h = v.header
                ts, data = v.timestamp, v.mof_data
            else:
                continue
            g, t = gi(h.group), h.type
            if g == G_THREAD:
                if t == 36 and len(data) >= 11:
                    new_t, _old_t, new_p = struct.unpack_from("<IIb", data, 0)
                    cs_ts[cpu].append(ts)
                    cs_tid[cpu].append(new_t)
                    switch_in[new_t].append((ts, new_p))
                elif t == 50 and len(data) >= 4:
                    readies.append((ts, cpu, struct.unpack_from("<I", data, 0)[0]))
                elif t in (1, 3) and len(data) >= 8:
                    p, tid = struct.unpack_from("<II", data, 0)
                    tid_pid[tid] = p
            elif g == G_PROCESS and t in (1, 3) and len(data) > 60:
                p = struct.unpack_from("<I", data, 8)[0]
                try:
                    sid_off = 36 + 16
                    sub = data[sid_off + 1]
                    name_off = sid_off + 8 + 4 * sub
                    end = data.index(b"\x00", name_off)
                    pid_name[p] = data[name_off:end].decode("ascii", "replace")
                except (IndexError, ValueError):
                    pass
            elif g == G_PERFINFO and t in (66, 68, 69, 67, 50) and len(data) >= 16:
                start, routine = struct.unpack_from("<QQ", data, 0)
                dpc[cpu].append((start, ts, routine, "ISR" if t in (67, 50) else "DPC"))
            elif g == G_IMAGE and t in (3, 10) and len(data) > 58:
                base, size, ipid = struct.unpack_from("<QQI", data, 0)
                if ipid == 0:
                    try:
                        raw = data[56:]
                        end = raw.index(b"\x00\x00")
                        end += end % 2
                        name = raw[:end].decode("utf-16-le", "replace").split("\\")[-1]
                    except ValueError:
                        name = "?"
                    images.append((base, size, name))
    images.sort()
    img_bases = [b for b, _, _ in images]

    def module_of(addr):
        i = bisect.bisect_right(img_bases, addr) - 1
        if i >= 0 and addr < images[i][0] + images[i][1]:
            return images[i][2]
        return "?"

    # per-CPU sorted timelines
    cpu_order = {}
    for cpu in cs_ts:
        idx = sorted(range(len(cs_ts[cpu])), key=cs_ts[cpu].__getitem__)
        cpu_order[cpu] = (array("q", (cs_ts[cpu][i] for i in idx)), array("I", (cs_tid[cpu][i] for i in idx)))
    for cpu in dpc:
        dpc[cpu].sort()
    dpc_starts = {cpu: [d[0] for d in dpc[cpu]] for cpu in dpc}
    for tid in switch_in:
        switch_in[tid].sort()
    sw_ts = {tid: [x[0] for x in v] for tid, v in switch_in.items()}

    # targets: labelled tids + high-priority threads of the host process
    targets = dict(labels)
    for tid, v in switch_in.items():
        if tid_pid.get(tid) == a.pid and tid not in targets and max(p for _, p in v) >= a.auto_prio:
            targets[tid] = f"auto_tid{tid}"

    us = 1e6 / a.qpc_freq
    res = {"kernel_records": n, "host_pid": a.pid, "host_name": pid_name.get(a.pid), "targets": {}}
    readies.sort()
    by_tid = defaultdict(list)
    for r in readies:
        if r[2] in targets:
            by_tid[r[2]].append(r)
    for tid, label in targets.items():
        ctx = Counter()
        lat = []
        for ts, cpu, _ in by_tid.get(tid, []):
            who = None
            if cpu in dpc_starts:
                i = bisect.bisect_right(dpc_starts[cpu], ts) - 1
                # DPCs can nest under ISRs; scan back a few entries for one still running at ts
                for j in range(i, max(-1, i - 4), -1):
                    s, e, routine, kind = dpc[cpu][j]
                    if s <= ts <= e:
                        who = f"{kind}:{module_of(routine)}"
                        break
            if who is None and cpu in cpu_order:
                tss, tids = cpu_order[cpu]
                k = bisect.bisect_right(tss, ts) - 1
                if k >= 0:
                    rt = tids[k]
                    if rt == tid:
                        who = "self"
                    else:
                        rp = tid_pid.get(rt)
                        who = f"thread:{targets.get(rt, 'tid' + str(rt) if rp == a.pid else pid_name.get(rp, 'pid' + str(rp)))}"
                        if rp == a.pid and rt not in targets:
                            who = f"thread:host_tid{rt}"
            ctx[who or "?"] += 1
            s = sw_ts.get(tid, [])
            k = bisect.bisect_left(s, ts)
            if k < len(s):
                lat.append((s[k] - ts) * us)
        pr = Counter(p for _, p in switch_in.get(tid, []))
        res["targets"][label] = {
            "tid": tid, "pid": tid_pid.get(tid), "readies": len(by_tid.get(tid, [])),
            "switch_ins": len(switch_in.get(tid, [])),
            "priority_at_switch_in": dict(pr.most_common(8)),
            "ready_to_run_us": {"p50": pct(lat, .5), "p99": pct(lat, .99), "p999": pct(lat, .999),
                                "max": round(max(lat), 1) if lat else None,
                                "over_1ms": sum(1 for x in lat if x > 1000),
                                "total_ready_ms": round(sum(lat) / 1000, 1)},
            "readied_by": dict(ctx.most_common(10)),
        }
    print(json.dumps(res, indent=1))
    if a.json:
        with open(a.json, "w", encoding="utf-8") as fo:
            json.dump(res, fo, indent=1)


if __name__ == "__main__":
    sys.exit(main())

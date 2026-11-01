"""Scheduler view of one thread from a WPR ETL (measure/wpr/um2_sched.wprp). DIAGNOSTIC ONLY.

tracerpt does not decode CSwitch v5, so the kernel records are read with etl-parser
(pip install etl-parser) and the two payloads used here are unpacked by hand (layouts documented in
the Windows kernel trace MOF classes):
  CSwitch  (Thread group, type 36): NewThreadId u32, OldThreadId u32, NewThreadPriority i8, OldThreadPriority i8,
           PreviousCState u8, SpareByte i8, OldThreadWaitReason i8, OldThreadWaitMode i8, OldThreadState i8,
           OldThreadWaitIdealProcessor i8, NewThreadWaitTime u32, ...
  ReadyThread (Thread group, type 50): TThreadId u32, AdjustReason i8, AdjustIncrement i8, Flag i8, ...
Timestamps are QPC ticks (WPR default clock); --qpc-freq converts them.

  python measure/scripts/etl_sched.py um2_sched.etl --tid 3656 [--json out.json]
"""
import argparse
import json
import struct
import sys
from collections import Counter

from etl.etl import IEtlFileObserver, build_from_stream

GROUP_THREAD = 0x05
WAIT_REASONS = {0: "Executive", 6: "UserRequest", 13: "WrUserRequest", 15: "WrQueue", 7: "WrExecutive"}


def pct(xs, q):
    if not xs:
        return None
    s = sorted(xs)
    return s[min(len(s) - 1, int(q * (len(s) - 1)))]


class Obs(IEtlFileObserver):
    def __init__(self, tid):
        self.tid = tid
        self.ready = []
        self.run = []
        self.prio = Counter()
        self.reason = Counter()
        self.pending = None
        self.lat = []
        self.n = 0

    def _handle(self, group, etype, ts, data):
        self.n += 1
        if getattr(group, "intvalue", group) != GROUP_THREAD:  # etl-parser gives a construct EnumIntegerString
            return
        # Records arrive per CPU buffer, not in global time order: collect, sort later (finish()).
        if etype == 50 and len(data) >= 4:
            (t,) = struct.unpack_from("<I", data, 0)
            if t == self.tid:
                self.ready.append(ts)
        elif etype == 36 and len(data) >= 24:
            new_t, old_t, new_p, _old_p, _cs, _sp, reason = struct.unpack_from("<IIbbBbb", data, 0)
            if new_t == self.tid:
                self.run.append(ts)
                self.prio[new_p] += 1
            if old_t == self.tid:
                self.reason[reason] += 1

    def finish(self):
        self.ready.sort()
        self.run.sort()
        # ready -> next switch-in of the same thread
        j = 0
        for r in self.ready:
            while j < len(self.run) and self.run[j] < r:
                j += 1
            if j < len(self.run):
                self.lat.append(self.run[j] - r)

    def on_perfinfo_trace(self, obj):
        h = obj.source.header
        self._handle(h.group, h.type, obj.source.timestamp, obj.source.mof_data)

    def on_system_trace(self, obj):
        sh = obj.source.system_header
        self._handle(sh.header.group, sh.header.type, sh.system_time, obj.source.mof_data)

    def on_event_record(self, event):
        pass

    def on_trace_record(self, event):
        pass

    def on_win_trace(self, obj):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("etl")
    ap.add_argument("--tid", type=int, required=True)
    ap.add_argument("--qpc-freq", type=float, default=1e7)
    ap.add_argument("--json")
    a = ap.parse_args()
    with open(a.etl, "rb") as f:
        etl = build_from_stream(f.read())
    o = Obs(a.tid)
    etl.parse(o)
    o.finish()
    us = 1e6 / a.qpc_freq
    lat = [x * us for x in o.lat]
    gaps = [(b - c) * us for c, b in zip(o.run, o.run[1:])]
    rgaps = [(b - c) * us for c, b in zip(o.ready, o.ready[1:])]
    res = {"tid": a.tid, "kernel_records_seen": o.n, "readies": len(o.ready), "switch_ins": len(o.run),
           "span_s": (o.run[-1] - o.run[0]) * us / 1e6 if len(o.run) > 1 else None,
           "ready_to_run_us": {"n": len(lat), "p50": pct(lat, .5), "p99": pct(lat, .99), "p999": pct(lat, .999),
                               "max": max(lat) if lat else None},
           "gap_between_switch_ins_us": {"p50": pct(gaps, .5), "p99": pct(gaps, .99), "max": max(gaps) if gaps else None},
           "gap_between_readies_us": {"p50": pct(rgaps, .5), "p99": pct(rgaps, .99), "max": max(rgaps) if rgaps else None},
           "ready_to_run_over_1ms": sum(1 for x in lat if x > 1000),
           "priority_at_switch_in": dict(o.prio.most_common(6)),
           "switch_out_wait_reason": {WAIT_REASONS.get(k, str(k)): v for k, v in o.reason.most_common(6)}}
    print(json.dumps(res, indent=1))
    if a.json:
        with open(a.json, "w", encoding="utf-8") as fo:
            json.dump(res, fo, indent=1)


if __name__ == "__main__":
    sys.exit(main())

"""Start routine (module+offset) of given threads from a WPR ETL (Thread DCStart/Start v3, x64:
ProcessId u32, TThreadId u32, StackBase, StackLimit, UserStackBase, UserStackLimit, Affinity, Win32StartAddr @48).
Kernel modules come from Image DCStart/Load with ProcessId 0. DIAGNOSTIC ONLY.

  python measure/scripts/etl_tidinfo.py trace.etl 1364 736
"""
import bisect
import json
import struct
import sys

from etl.etl import build_from_stream, ChunkParser


def gi(g):
    return getattr(g, "intvalue", g)


def main():
    path, tids = sys.argv[1], {int(x) for x in sys.argv[2:]}
    starts, images = {}, []
    with open(path, "rb") as f:
        etl = build_from_stream(f.read())
    for chunk in etl.chunks:
        for ev in ChunkParser.parse(chunk.payload):
            if ev.type != "SystemTraceRecord":
                continue
            v = ev.value
            g, t, data = gi(v.system_header.header.group), v.system_header.header.type, v.mof_data
            if g == 5 and t in (1, 3, 2, 4) and len(data) >= 56:
                pid, tid = struct.unpack_from("<II", data, 0)
                if tid in tids:
                    starts[tid] = (pid, struct.unpack_from("<Q", data, 48)[0])
            elif g == 20 and t in (3, 10) and len(data) > 58:
                base, size, ipid = struct.unpack_from("<QQI", data, 0)
                if ipid == 0:
                    raw = data[56:]
                    try:
                        end = raw.index(b"\x00\x00")
                        end += end % 2
                        name = raw[:end].decode("utf-16-le", "replace").split("\\")[-1]
                    except ValueError:
                        name = "?"
                    images.append((base, size, name))
    images.sort()
    bases = [b for b, _, _ in images]
    out = {}
    for tid, (pid, addr) in starts.items():
        i = bisect.bisect_right(bases, addr) - 1
        mod = f"{images[i][2]}+0x{addr - images[i][0]:X}" if i >= 0 and addr < images[i][0] + images[i][1] else f"0x{addr:X}"
        out[tid] = {"pid": pid, "start": mod}
    print(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()

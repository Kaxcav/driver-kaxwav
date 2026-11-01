"""Read-only access to the driver's shared memory sections (Windows).

- UM2SharedState (panel IPC, 'Local\\BehringerUM2SharedMemory'): the ctypes layout is taken from
  panel/panel_backend.py, the same mirror of src/UM2Ipc.h the panel uses, so there is one copy.
- UM2Telemetry (M5, 'Local\\BehringerUM2Telemetry'): mirror of src/UM2Telemetry.h.
"""
import ctypes
from ctypes import wintypes
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FILE_MAP_READ = 0x0004

_k = ctypes.windll.kernel32
_k.OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
_k.OpenFileMappingW.restype = wintypes.HANDLE
_k.MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_size_t]
_k.MapViewOfFile.restype = ctypes.c_void_p
_k.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
_k.CloseHandle.argtypes = [wintypes.HANDLE]


def _shared_state_type():
    src = (REPO / "panel" / "panel_backend.py").read_text(encoding="utf-8")
    start = src.index("class UM2SharedState(ctypes.Structure):")
    end = src.index("# ====", start)
    ns = {"ctypes": ctypes}
    exec(src[start:end], ns)  # ctypes.Structure definition only
    return ns["UM2SharedState"]


class _Mapped:
    def __init__(self, name, size):
        self.h = _k.OpenFileMappingW(FILE_MAP_READ, False, name)
        self.p = _k.MapViewOfFile(self.h, FILE_MAP_READ, 0, 0, size) if self.h else None

    def __enter__(self):
        return self.p

    def __exit__(self, *a):
        if self.p:
            _k.UnmapViewOfFile(self.p)
        if self.h:
            _k.CloseHandle(self.h)


DSP_FIELDS = [
    "isLicensed", "softwareMute", "driverMode", "bufferSize", "sampleRate", "pdcOffsetSamples",
    "inputLatencySamples", "outputLatencySamples", "hardwareModel", "xruns",
    "ch1Phase", "ch1Hpf", "ch1Mute", "ch1Trim", "ch1Gate", "ch1PreampMode", "ch1CompAmount", "ch1DeesserAmount",
    "ch2Phase", "ch2Pad", "ch2Mute", "ch2Trim", "ch2Gate", "ch2PreampMode", "ch2CompAmount", "ch2CabSim",
    "gateThresholdDb", "dspMonitorOnly", "loopbackEnabled", "loopbackGain", "loopbackMono", "loopbackBroadcastMix",
    "loopbackMode", "masterDim", "masterMono", "masterMute", "masterTrim", "monSubCheck", "monPhoneSim", "monCrossfeed",
]


def read_shared_state(fields=DSP_FIELDS):
    S = _shared_state_type()
    with _Mapped("Local\\BehringerUM2SharedMemory", ctypes.sizeof(S)) as p:
        if not p:
            return None
        st = S.from_address(p)
        out = {}
        for f in fields:
            v = getattr(st, f)
            out[f] = round(v, 6) if isinstance(v, float) else v
        return out


# ---------------- M5 telemetry ----------------
RING = 4096
BINS = 4096


class TimingEntry(ctypes.Structure):
    _fields_ = [("qpcStart", ctypes.c_int64), ("qpcDawStart", ctypes.c_int64),
                ("qpcDawEnd", ctypes.c_int64), ("qpcEnd", ctypes.c_int64)]


class Telemetry(ctypes.Structure):  # natural alignment == #pragma pack(8) in src/UM2Telemetry.h
    _fields_ = [
        ("magic", ctypes.c_uint32), ("version", ctypes.c_uint32), ("ownerPid", ctypes.c_uint32),
        ("sessionId", ctypes.c_uint32), ("qpcFrequency", ctypes.c_int64), ("sampleRate", ctypes.c_uint32),
        ("bufferSize", ctypes.c_uint32), ("budgetTicks", ctypes.c_int64), ("blocks", ctypes.c_uint64),
        ("misses", ctypes.c_uint64), ("maxTicks", ctypes.c_int64), ("maxIntervalTicks", ctypes.c_int64),
        ("lastStart", ctypes.c_int64), ("writeIndex", ctypes.c_uint64), ("histBinNs", ctypes.c_uint32),
        ("histBins", ctypes.c_uint32), ("hist", ctypes.c_uint32 * BINS), ("ring", TimingEntry * RING),
    ]


assert ctypes.sizeof(Telemetry) == 147552, "src/UM2Telemetry.h layout changed"
TELEMETRY_MAGIC = 0x54324D55


def read_telemetry():
    """Snapshot copy of the telemetry section, or None if no driver instance has it open."""
    with _Mapped("Local\\BehringerUM2Telemetry", ctypes.sizeof(Telemetry)) as p:
        if not p:
            return None
        snap = Telemetry()
        ctypes.memmove(ctypes.byref(snap), p, ctypes.sizeof(Telemetry))
        return snap if snap.magic == TELEMETRY_MAGIC else None


def summarize_telemetry(t):
    """p50/p99/max of block processing time, computed here (outside the callback)."""
    if t is None or t.blocks == 0:
        return None
    hist = list(t.hist)
    total = sum(hist)
    bin_us = t.histBinNs / 1000.0

    def pct(q):
        acc, target = 0, q * total
        for i, c in enumerate(hist):
            acc += c
            if acc >= target:
                return (i + 1) * bin_us  # upper edge of the bin
        return None

    f = float(t.qpcFrequency)
    budget_us = t.budgetTicks / f * 1e6
    # gap statistics from the ring (most recent RING blocks)
    n = min(int(t.writeIndex), RING)
    starts = sorted(t.ring[i].qpcStart for i in range(n))
    gaps = [(b - a) / f * 1e6 for a, b in zip(starts, starts[1:])]
    daw = [(t.ring[i].qpcDawEnd - t.ring[i].qpcDawStart) / f * 1e6 for i in range(n)]
    return {
        "session": t.sessionId, "owner_pid": t.ownerPid, "buffer": t.bufferSize, "sample_rate": t.sampleRate,
        "blocks": t.blocks, "budget_us": round(budget_us, 1),
        "block_us_p50": pct(0.50), "block_us_p99": pct(0.99), "block_us_max": round(t.maxTicks / f * 1e6, 1),
        "deadline_misses": t.misses, "miss_rate": t.misses / t.blocks,
        "max_gap_between_blocks_us": round(t.maxIntervalTicks / f * 1e6, 1),
        "hist_bin_us": bin_us, "overflow_bin_count": hist[-1],
        "recent_window_blocks": n,
        "recent_daw_callback_us_max": round(max(daw), 1) if daw else None,
        "recent_gap_us_p99": round(sorted(gaps)[int(0.99 * (len(gaps) - 1))], 1) if gaps else None,
    }

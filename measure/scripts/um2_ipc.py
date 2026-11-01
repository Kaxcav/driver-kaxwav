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


# ---------------- thread priorities of another process (no elevation needed for own processes) ----------------
class _THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ThreadID", wintypes.DWORD),
                ("th32OwnerProcessID", wintypes.DWORD), ("tpBasePri", wintypes.LONG), ("tpDeltaPri", wintypes.LONG),
                ("dwFlags", wintypes.DWORD)]


class _THREAD_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [("ExitStatus", wintypes.LONG), ("TebBaseAddress", ctypes.c_void_p),
                ("UniqueProcess", ctypes.c_void_p), ("UniqueThread", ctypes.c_void_p),
                ("AffinityMask", ctypes.c_size_t), ("Priority", wintypes.LONG), ("BasePriority", wintypes.LONG)]


_k.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
_k.OpenThread.restype = wintypes.HANDLE
_ntdll = ctypes.windll.ntdll


class _MODULEINFO(ctypes.Structure):
    _fields_ = [("lpBaseOfDll", ctypes.c_void_p), ("SizeOfImage", wintypes.DWORD), ("EntryPoint", ctypes.c_void_p)]


def thread_start_modules(pid):
    """{tid: 'module+0xoffset'} of each thread's Win32 start address (who created the thread)."""
    psapi = ctypes.windll.psapi
    _k.OpenProcess.restype = wintypes.HANDLE
    hp = _k.OpenProcess(0x0410, False, pid)  # PROCESS_QUERY_INFORMATION | PROCESS_VM_READ
    mods = []
    if hp:
        arr = (ctypes.c_void_p * 1024)()
        need = wintypes.DWORD()
        if psapi.EnumProcessModulesEx(hp, arr, ctypes.sizeof(arr), ctypes.byref(need), 3):
            for i in range(min(1024, need.value // ctypes.sizeof(ctypes.c_void_p))):
                mi = _MODULEINFO()
                name = ctypes.create_unicode_buffer(260)
                if psapi.GetModuleInformation(hp, ctypes.c_void_p(arr[i]), ctypes.byref(mi), ctypes.sizeof(mi)):
                    psapi.GetModuleBaseNameW(hp, ctypes.c_void_p(arr[i]), name, 260)
                    mods.append((mi.lpBaseOfDll or 0, mi.SizeOfImage, name.value))
        _k.CloseHandle(hp)
    out = {}
    for tid, _c, _b in thread_priorities(pid):
        h = _k.OpenThread(0x0040, False, tid)  # THREAD_QUERY_INFORMATION
        if not h:
            continue
        addr = ctypes.c_void_p()
        if _ntdll.NtQueryInformationThread(h, 9, ctypes.byref(addr), ctypes.sizeof(addr), None) == 0 and addr.value:
            a = addr.value
            hit = next((f"{n}+0x{a - b:X}" for b, s, n in mods if b <= a < b + s), f"0x{a:X}")
            out[tid] = hit
        _k.CloseHandle(h)
    return out


def thread_priorities(pid):
    """[(tid, current_priority, base_priority)] for every thread of pid, via NtQueryInformationThread
    (ThreadBasicInformation): the current priority includes boosts such as the MMCSS one."""
    snap = _k.CreateToolhelp32Snapshot(0x4, 0)  # TH32CS_SNAPTHREAD
    out = []
    if not snap or snap == wintypes.HANDLE(-1).value:
        return out
    try:
        te = _THREADENTRY32()
        te.dwSize = ctypes.sizeof(te)
        ok = _k.Thread32First(snap, ctypes.byref(te))
        while ok:
            if te.th32OwnerProcessID == pid:
                h = _k.OpenThread(0x0800, False, te.th32ThreadID)  # THREAD_QUERY_LIMITED_INFORMATION
                if h:
                    tbi = _THREAD_BASIC_INFORMATION()
                    if _ntdll.NtQueryInformationThread(h, 0, ctypes.byref(tbi), ctypes.sizeof(tbi), None) == 0:
                        out.append((te.th32ThreadID, tbi.Priority, tbi.BasePriority))
                    _k.CloseHandle(h)
            ok = _k.Thread32Next(snap, ctypes.byref(te))
    finally:
        _k.CloseHandle(snap)
    return out


# ---------------- engine-loop diagnostics (src/UM2Diag.h, off unless DiagCounters=1) ----------------
DIAG_RING = 4096
DIAG_TBINS = 128
DIAG_TBIN_US = 250
DIAG_PBINS = 17
DIAG_MAGIC = 0x47444D55
WAKE_FLAGS = {1: "render", 2: "capture", 4: "timeout", 8: "ren_write", 16: "cap_disc"}


class DiagWake(ctypes.Structure):
    _fields_ = [("qpc", ctypes.c_int64), ("flags", ctypes.c_uint16), ("capPending", ctypes.c_uint16),
                ("renPadding", ctypes.c_uint16), ("blocks", ctypes.c_uint16)]


_U64 = ctypes.c_uint64
_I64 = ctypes.c_int64
_U32 = ctypes.c_uint32


class Diag(ctypes.Structure):  # mirror of src/UM2Diag.h (#pragma pack(8) == natural alignment here)
    _fields_ = (
        [(n, _U32) for n in ("magic", "version", "ownerPid", "mode", "bufferSize", "renderPeriodFrames",
                             "capturePeriodFrames", "audioThreadId")]
        + [("qpcFrequency", _I64)]
        + [(n, _U64) for n in ("wakes", "waitTimeouts", "wakeRenderOnly", "wakeCaptureOnly", "wakeBoth", "reserved2",
                               "renderServiced", "captureServiced", "capPacketsMain", "capPacketsOpportunistic",
                               "capEmptyOnEvent", "capGetBufferFail", "capFramesIn", "capDropFrames",
                               "capUnderflowFrames", "capUnderflowBlocks", "capDiscontinuity", "capSilentFlag")]
        + [("lastCapHr", _U32), ("lastRenHr", _U32)]
        + [(n, _U64) for n in ("renWrites", "renGetBufferFail", "renSkipNoData", "renPaddingFail", "bufferSwitches")]
        + [(n, _I64) for n in ("maxWakeIntervalTicks", "maxRenderIntervalTicks", "maxCapAgeTicks",
                               "lastWakeQpc", "lastRenderQpc")]
        + [("blocksPerWakeHist", _U32 * 8), ("capPendingHist", _U32 * DIAG_PBINS),
           ("renPaddingHist", _U32 * DIAG_PBINS), ("wakeIntervalHist", _U32 * DIAG_TBINS),
           ("renderIntervalHist", _U32 * DIAG_TBINS), ("capAgeHist", _U32 * DIAG_TBINS),
           ("writeIndex", _U64), ("ring", DiagWake * DIAG_RING)]
    )


assert ctypes.sizeof(Diag) == 67520, "src/UM2Diag.h layout changed"


def read_diag():
    with _Mapped("Local\\BehringerUM2Diag", ctypes.sizeof(Diag)) as p:
        if not p:
            return None
        snap = Diag()
        ctypes.memmove(ctypes.byref(snap), p, ctypes.sizeof(Diag))
        return snap if snap.magic == DIAG_MAGIC else None


def _tpct(hist, q):
    total = sum(hist)
    if not total:
        return None
    acc = 0
    for i, c in enumerate(hist):
        acc += c
        if acc >= q * total:
            return (i + 1) * DIAG_TBIN_US  # upper edge, us
    return None


def _nonzero(hist, unit):
    return {f"{i * unit}": c for i, c in enumerate(hist) if c}


def summarize_diag(d):
    """Counters + histograms of the engine loop, plus the cadence pattern of the most recent wakes."""
    if d is None or d.wakes == 0:
        return None
    f = float(d.qpcFrequency)
    us = lambda t: round(t / f * 1e6, 1)  # noqa: E731
    out = {k: getattr(d, k) for k in (
        "ownerPid", "audioThreadId", "mode", "bufferSize", "renderPeriodFrames", "capturePeriodFrames", "wakes", "waitTimeouts",
        "wakeRenderOnly", "wakeCaptureOnly", "wakeBoth", "renderServiced", "captureServiced", "capPacketsMain",
        "capPacketsOpportunistic", "capEmptyOnEvent", "capGetBufferFail", "capFramesIn", "capDropFrames",
        "capUnderflowFrames", "capUnderflowBlocks", "capDiscontinuity", "capSilentFlag", "renWrites",
        "renGetBufferFail", "renSkipNoData", "renPaddingFail", "bufferSwitches")}
    out["lastCapHr"] = f"0x{d.lastCapHr:08X}"
    out["lastRenHr"] = f"0x{d.lastRenHr:08X}"
    out["max_wake_interval_us"] = us(d.maxWakeIntervalTicks)
    out["max_render_interval_us"] = us(d.maxRenderIntervalTicks)
    out["max_capture_packet_age_us"] = us(d.maxCapAgeTicks)
    out["blocks_per_wake_hist"] = list(d.blocksPerWakeHist)
    out["cap_pending_hist_quarter_periods"] = _nonzero(d.capPendingHist, 1)
    out["ren_padding_hist_quarter_periods"] = _nonzero(d.renPaddingHist, 1)
    for name in ("wakeIntervalHist", "renderIntervalHist", "capAgeHist"):
        h = list(getattr(d, name))
        out[name] = {"bin_us": DIAG_TBIN_US, "p50_us": _tpct(h, 0.5), "p99_us": _tpct(h, 0.99),
                     "nonzero": _nonzero(h, DIAG_TBIN_US)}
    n = min(int(d.writeIndex), DIAG_RING)
    start = int(d.writeIndex) - n
    w = [d.ring[(start + i) % DIAG_RING] for i in range(n)]
    w.sort(key=lambda e: e.qpc)
    out["recent_wakes"] = n
    out["recent_tail"] = [{"t_us": us(e.qpc - w[0].qpc), "flags": [v for k, v in WAKE_FLAGS.items() if e.flags & k],
                           "cap_pending": e.capPending, "ren_padding": e.renPadding, "blocks": e.blocks}
                          for e in w[-48:]] if w else []
    return out

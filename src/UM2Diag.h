#pragma once
#include <cstddef>
#include <cstdint>
#include <atomic>

// Engine-loop diagnostics (ExclusivePro/SharedFlex wake cadence). DIAGNOSTIC ONLY, off by default:
// enabled by HKCU\Software\BehringerUM2\ASIO DiagCounters=1, read in start() (never in the audio thread).
// When off, the audio thread takes none of the extra calls (GetCurrentPadding, QPC reads) used here.
// Own shared section so UM2SharedState (panel IPC) and UM2Telemetry (M5) keep their layouts.
// Writer: the audio thread only, plain stores of fixed-size integers. Reader: measure/scripts/um2_ipc.py.
constexpr uint32_t UM2_DIAG_MAGIC = 0x47444D55;  // "UMDG"
constexpr uint32_t UM2_DIAG_VERSION = 1;
constexpr wchar_t UM2_DIAG_NAME[] = L"Local\\BehringerUM2Diag";
constexpr uint32_t UM2_DIAG_RING = 4096;      // power of two
constexpr uint32_t UM2_DIAG_TBINS = 128;      // time histograms: 250 us per bin, last = overflow (>= 31.75 ms)
constexpr uint32_t UM2_DIAG_TBIN_US = 250;
constexpr uint32_t UM2_DIAG_PBINS = 17;       // padding histograms: quarter-periods 0..15, last = >= 4 periods

enum : uint16_t {
    UM2_WAKE_RENDER = 1,     // render event observed this wake
    UM2_WAKE_CAPTURE = 2,    // capture event observed this wake
    UM2_WAKE_TIMEOUT = 4,    // wait timed out (20 ms)
    UM2_WAKE_REN_WRITE = 8,  // render buffer written
    UM2_WAKE_CAP_DISC = 16,  // capture packet with DATA_DISCONTINUITY
};

#pragma pack(push, 8)
struct UM2DiagWake {
    int64_t  qpc;          // wake time
    uint16_t flags;        // UM2_WAKE_*
    uint16_t capPending;   // capture frames available at wake (GetCurrentPadding)
    uint16_t renPadding;   // render frames still queued at wake (GetCurrentPadding)
    uint16_t blocks;       // ASIO blocks (bufferSwitch) dispatched in this wake
};

struct UM2Diag {
    uint32_t magic, version, ownerPid, mode;      // mode: 0 ExclusivePro, 1 SharedFlex
    uint32_t bufferSize, renderPeriodFrames, capturePeriodFrames, audioThreadId;  // TID for ETW correlation
    int64_t  qpcFrequency;
    // wait loop
    uint64_t wakes, waitTimeouts, wakeRenderOnly, wakeCaptureOnly, wakeBoth, reserved2;
    uint64_t renderServiced, captureServiced;
    // capture
    uint64_t capPacketsMain, capPacketsOpportunistic, capEmptyOnEvent, capGetBufferFail;
    uint64_t capFramesIn, capDropFrames, capUnderflowFrames, capUnderflowBlocks, capDiscontinuity, capSilentFlag;
    uint32_t lastCapHr, lastRenHr;
    // render
    uint64_t renWrites, renGetBufferFail, renSkipNoData, renPaddingFail;
    // ASIO
    uint64_t bufferSwitches;
    int64_t  maxWakeIntervalTicks, maxRenderIntervalTicks, maxCapAgeTicks;
    int64_t  lastWakeQpc, lastRenderQpc;
    uint32_t blocksPerWakeHist[8];                  // 0..6, last = >= 7
    uint32_t capPendingHist[UM2_DIAG_PBINS];        // capture pending at capture wake, in quarter capture periods
    uint32_t renPaddingHist[UM2_DIAG_PBINS];        // render padding at render wake, in quarter render periods
    uint32_t wakeIntervalHist[UM2_DIAG_TBINS];      // between consecutive wakes
    uint32_t renderIntervalHist[UM2_DIAG_TBINS];    // between consecutive render-serviced wakes
    uint32_t capAgeHist[UM2_DIAG_TBINS];            // now - device QPC position of the capture packet
    uint64_t writeIndex;                            // wake ring entries written (release)
    UM2DiagWake ring[UM2_DIAG_RING];
};
#pragma pack(pop)

static_assert(offsetof(UM2Diag, writeIndex) % 8 == 0, "writeIndex must be 8-byte aligned");
static_assert(sizeof(UM2DiagWake) == 16, "ring entry layout mirrored in measure/scripts/um2_ipc.py");
static_assert(sizeof(UM2Diag) == 67520, "layout mirrored in measure/scripts/um2_ipc.py");

inline uint32_t UM2DiagTBin(int64_t ticks, int64_t freq) {
    if (ticks <= 0 || freq <= 0) return 0;
    const int64_t us = (ticks * 1000000LL) / freq;
    const int64_t b = us / UM2_DIAG_TBIN_US;
    return static_cast<uint32_t>(b >= UM2_DIAG_TBINS ? UM2_DIAG_TBINS - 1 : b);
}

inline uint32_t UM2DiagPBin(uint32_t frames, uint32_t period) {
    if (period == 0) return 0;
    const uint32_t q = (frames * 4u) / period;
    return q >= UM2_DIAG_PBINS ? UM2_DIAG_PBINS - 1 : q;
}

inline uint16_t UM2DiagClamp16(uint64_t v) { return static_cast<uint16_t>(v > 0xFFFF ? 0xFFFF : v); }

inline void UM2DiagPushWake(UM2Diag* d, int64_t qpc, uint16_t flags, uint32_t capPending, uint32_t renPadding, uint32_t blocks) {
    const uint64_t idx = d->writeIndex;
    UM2DiagWake& w = d->ring[idx & (UM2_DIAG_RING - 1)];
    w.qpc = qpc;
    w.flags = flags;
    w.capPending = UM2DiagClamp16(capPending);
    w.renPadding = UM2DiagClamp16(renPadding);
    w.blocks = UM2DiagClamp16(blocks);
    std::atomic_ref<uint64_t>(d->writeIndex).store(idx + 1, std::memory_order_release);
}

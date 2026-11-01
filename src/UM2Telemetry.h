#pragma once
#include <cstddef>
#include <cstdint>
#include <atomic>

// M5 deadline telemetry (docs/MEASUREMENT-PROTOCOL.md).
// Lives in its own shared section so the panel IPC (UM2SharedState, 848 bytes) is untouched.
// Writer: the driver's audio thread, once per ASIO block. Reader: measure/scripts (another process),
// which computes p50/p99/max from the histogram - nothing is computed in the callback.
constexpr uint32_t UM2_TELEMETRY_MAGIC = 0x54324D55;  // "UM2T"
constexpr uint32_t UM2_TELEMETRY_VERSION = 1;
constexpr wchar_t UM2_TELEMETRY_NAME[] = L"Local\\BehringerUM2Telemetry";
constexpr uint32_t UM2_TIMING_RING_SIZE = 4096;  // power of two
constexpr uint32_t UM2_TIMING_HIST_BINS = 4096;  // last bin = overflow
constexpr uint32_t UM2_TIMING_BIN_NS = 5000;     // 5 us per bin -> 20.475 ms before overflow

#pragma pack(push, 8)
struct UM2TimingEntry {
    int64_t qpcStart;     // block entry (start of processing, before capture FIFO read)
    int64_t qpcDawStart;  // just before bufferSwitch / bufferSwitchTimeInfo
    int64_t qpcDawEnd;    // just after the DAW callback returns
    int64_t qpcEnd;       // after the output stage, before meters/telemetry
};

struct UM2Telemetry {
    uint32_t magic;
    uint32_t version;
    uint32_t ownerPid;
    uint32_t sessionId;
    int64_t  qpcFrequency;
    uint32_t sampleRate;
    uint32_t bufferSize;
    int64_t  budgetTicks;       // bufferSize / sampleRate, in QPC ticks
    uint64_t blocks;
    uint64_t misses;            // blocks with (qpcEnd - qpcStart) > budgetTicks
    int64_t  maxTicks;          // longest block (qpcEnd - qpcStart)
    int64_t  maxIntervalTicks;  // longest qpcStart-to-qpcStart gap
    int64_t  lastStart;
    uint64_t writeIndex;        // ring entries written, published with release semantics
    uint32_t histBinNs;
    uint32_t histBins;
    uint32_t hist[UM2_TIMING_HIST_BINS];  // block duration histogram
    UM2TimingEntry ring[UM2_TIMING_RING_SIZE];
};
#pragma pack(pop)

static_assert(offsetof(UM2Telemetry, writeIndex) % 8 == 0, "writeIndex must be 8-byte aligned");
static_assert(offsetof(UM2Telemetry, ring) % 8 == 0, "ring must be 8-byte aligned");
static_assert(sizeof(UM2Telemetry) == 147552, "layout mirrored in measure/scripts/um2_ipc.py");

// Audio-thread side: fixed-size integer arithmetic and plain stores only.
// No allocation, no lock, no log, no system call.
inline void UM2RecordBlockTiming(UM2Telemetry* t, int64_t qpcStart, int64_t qpcDawStart, int64_t qpcDawEnd, int64_t qpcEnd) {
    const int64_t dur = qpcEnd - qpcStart;
    if (t->blocks > 0) {
        const int64_t gap = qpcStart - t->lastStart;
        if (gap > t->maxIntervalTicks) t->maxIntervalTicks = gap;
    }
    t->lastStart = qpcStart;
    if (dur > t->maxTicks) t->maxTicks = dur;
    if (dur > t->budgetTicks) t->misses++;
    const int64_t ns = (t->qpcFrequency > 0 && dur > 0) ? (dur * 1000000000LL) / t->qpcFrequency : 0;
    uint64_t bin = static_cast<uint64_t>(ns) / UM2_TIMING_BIN_NS;
    if (bin >= UM2_TIMING_HIST_BINS) bin = UM2_TIMING_HIST_BINS - 1;
    t->hist[bin]++;
    const uint64_t idx = t->writeIndex;
    UM2TimingEntry& e = t->ring[idx & (UM2_TIMING_RING_SIZE - 1)];
    e.qpcStart = qpcStart;
    e.qpcDawStart = qpcDawStart;
    e.qpcDawEnd = qpcDawEnd;
    e.qpcEnd = qpcEnd;
    std::atomic_ref<uint64_t>(t->writeIndex).store(idx + 1, std::memory_order_release);
    t->blocks++;
}

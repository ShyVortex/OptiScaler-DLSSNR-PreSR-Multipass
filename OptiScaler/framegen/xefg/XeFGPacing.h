#pragma once

// Spreads Intel's generated frames out in time, so that multi frame generation
// above 2X stops arriving as one clump followed by a gap.

#include <windows.h>
#ifdef _MSC_VER
#include <intrin.h>
#define XE_RETURN_ADDRESS() _ReturnAddress()
#else
#define XE_RETURN_ADDRESS() __builtin_return_address(0)
#endif

#include <algorithm>
#include <format>
#include "Logger.h"
#include "SysUtils.h"

namespace XeFGPacing
{
// --- provider internals, libxess_fg.dll 1.3.1.78 ------------------------

constexpr uint32_t PresentThunkRva = 0x25C0;
constexpr uint32_t NativePresentRva = 0x21F730;
constexpr uint32_t PacedCallerRva = 0x2202ED;
constexpr uint32_t LastFrameCallerRva = 0x220467;

constexpr uint32_t LimiterEnabledOffset = 0x340;
constexpr uint32_t BurstLimiterField = 0x28;

// --- the provider's own frame scheduler --------------------------------

constexpr uint32_t SchedThunkRva = 0x3100;
constexpr uint32_t SchedFnRva = 0x21EE30;
constexpr uint32_t BurstGateOffset = 0xC0;

constexpr uint32_t SchedLimiterOffset = 0x340;
constexpr uint32_t SchedEnableOffset = 0x341;

constexpr uint32_t RingSnapshotFnRva = 0x224CF0;
constexpr uint32_t RingOffset = 0x168;

constexpr uint32_t TimestampThunkRva = 0x3430;
constexpr uint32_t TimestampFnRva = 0x224B30;

constexpr uint32_t RingMeasuredOffset = 0x1B8;

inline const uint8_t TimestampThunkExpected[16] = {
    0xE9, 0xFB, 0x16, 0x22, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
};

inline const uint8_t SchedThunkExpected[16] = {
    0xE9, 0x2B, 0xBD, 0x21, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
};

constexpr uint32_t PresentThunkSize = 16;

inline const uint8_t PresentThunkExpected[PresentThunkSize] = {
    0xE9, 0x6B, 0xD1, 0x21, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
};

using PresentFn = int64_t (*)(void*, uint32_t, uint32_t, uint64_t, void*, void*, uint64_t);

constexpr int32_t SampleCount = 15;

inline uint8_t* g_base = nullptr;
inline PresentFn g_native = nullptr;
inline bool g_enabled = false;

inline uint8_t g_presentThunkOrig[PresentThunkSize] {};
inline uint8_t g_schedThunkOrig[PresentThunkSize] {};
inline uint8_t g_tsThunkOrig[PresentThunkSize] {};
inline bool g_presentHooked = false;
inline bool g_schedHooked = false;
inline bool g_tsHooked = false;

inline LARGE_INTEGER g_freq {};
inline int64_t g_lastBurstQpc = 0;
inline int64_t g_periodNs = 0;
inline int64_t g_intervalQpc = 0;
inline int64_t g_targetQpc = 0;
inline int64_t g_samples[SampleCount] {};
inline int32_t g_sampleCount = 0;
inline int32_t g_samplePos = 0;
inline int64_t g_pacedFrames = 0;
inline int64_t g_pacedBursts = 0;
inline bool g_loggedFirstBurst = false;

using SchedFn = bool (*)(void*, void*, uint8_t, void*, uint32_t);
using RingSnapshotFn = void* (*)(void*, void*);

inline SchedFn g_schedNative = nullptr;
inline RingSnapshotFn g_ringSnapshot = nullptr;

inline int32_t g_schedLogged = 0;
inline uint64_t g_schedLastIndex = ~0ull;
inline int64_t g_schedCalls = 0;
inline int64_t g_schedRefused = 0;
inline int64_t g_schedWaitQpc = 0;
inline int64_t g_lastMultiplier = 0;

inline int64_t g_burstBlockQpc = 0;
inline int64_t g_renderTimeNs = 0;
inline double g_fedFrameTimeMs = 0.0;

constexpr int32_t SchedLogLimit = 12;

using TimestampFn = void* (*)(void*, int64_t*, void*, void*, uint32_t, uint32_t);

inline TimestampFn g_tsNative = nullptr;
inline uint8_t* g_ring = nullptr;

inline int64_t g_nextDeadlineNs = 0;
inline int64_t g_burstStepNs = 0;
inline uint32_t g_lastTsIndex = 0;
inline uint32_t g_lastTsCountPlus1 = 0;

inline int64_t g_tsCalls = 0;
inline int64_t g_tsRebased = 0;
inline int64_t g_tsClamped = 0;

inline bool SchedulerUsable(void* ctx)
{
    const auto* ctxBytes = static_cast<const uint8_t*>(ctx);
    return ctxBytes[SchedLimiterOffset] == 0 && ctxBytes[SchedEnableOffset] != 0;
}

inline int64_t g_lastPacedQpc = 0;
inline int64_t g_gapSumQpc = 0;
inline int64_t g_gapMinQpc = 0;
inline int64_t g_gapMaxQpc = 0;
inline int32_t g_gapCount = 0;
inline int64_t g_statsDeadline = 0;

constexpr int64_t StatsWindowNs = 5000000000LL; // 5 s

inline int64_t NsFromQpc(int64_t delta)
{
    if (g_freq.QuadPart <= 0 || delta <= 0)
        return 0;

    const int64_t sec = delta / g_freq.QuadPart;
    const int64_t rem = delta % g_freq.QuadPart;
    return sec * 1000000000LL + (rem * 1000000000LL) / g_freq.QuadPart;
}

inline int64_t QpcFromNs(int64_t ns) { return g_freq.QuadPart > 0 ? (ns * g_freq.QuadPart) / 1000000000LL : 0; }

inline double MsFromQpc(int64_t qpc) { return g_freq.QuadPart > 0 ? (qpc * 1000.0) / g_freq.QuadPart : 0.0; }

struct DiagFrameRecord
{
    uint32_t index = 0;
    uint32_t countPlus1 = 0;
    int64_t nativeOutNs = 0;
    int64_t detouredOutNs = 0;
    int64_t medianNs = 0;
    float ringMeasuredMs = 0.0f;
    int64_t schedDurationUs = 0;
    bool scheduled = false;
};

constexpr size_t DiagRingCapacity = 32;
inline DiagFrameRecord g_diagRing[DiagRingCapacity] {};
inline uint64_t g_diagSeq = 0;

inline void ResetStats(int64_t nowQpc)
{
    g_gapSumQpc = 0;
    g_gapMinQpc = 0;
    g_gapMaxQpc = 0;
    g_gapCount = 0;
    g_statsDeadline = nowQpc + QpcFromNs(StatsWindowNs);
}

inline void ReportStats(int64_t multiplier)
{
    if (g_gapCount <= 0)
        return;

    LOG_INFO("XeFG pacing: {}X, real frame {:.2f} ms ({:.1f} fps), target {:.2f} ms/frame; "
             "gap {:.2f} avg / {:.2f} min / {:.2f} max ms over {} frames; "
             "scheduler {} calls, {} refused, {:.2f} ms avg inside; "
             "deadlines {} calls, {} rebased, {} clamped; render-est {:.2f} ms, fed {:.2f} ms",
             multiplier, g_periodNs / 1000000.0, g_periodNs > 0 ? 1e9 / g_periodNs : 0.0, MsFromQpc(g_intervalQpc),
             MsFromQpc(g_gapSumQpc / g_gapCount), MsFromQpc(g_gapMinQpc), MsFromQpc(g_gapMaxQpc), g_gapCount,
             g_schedCalls, g_schedRefused, g_schedCalls > 0 ? MsFromQpc(g_schedWaitQpc / g_schedCalls) : 0.0, g_tsCalls,
             g_tsRebased, g_tsClamped, g_renderTimeNs / 1000000.0, g_fedFrameTimeMs);

    if (g_diagSeq > 0)
    {
        std::string trace;
        const uint64_t count = (g_diagSeq < DiagRingCapacity) ? g_diagSeq : DiagRingCapacity;
        const uint64_t start = (g_diagSeq >= count) ? (g_diagSeq - count) : 0;
        int lastBurstStart = -1;
        for (int i = static_cast<int>(count) - 1; i >= 0; i--)
        {
            const auto& r = g_diagRing[(start + i) % DiagRingCapacity];
            if (r.index == 1)
            {
                lastBurstStart = i;
                break;
            }
        }

        if (lastBurstStart >= 0)
        {
            const float ringMs = g_diagRing[(start + lastBurstStart) % DiagRingCapacity].ringMeasuredMs;
            const double medMs = g_diagRing[(start + lastBurstStart) % DiagRingCapacity].medianNs / 1000000.0;
            for (uint64_t i = static_cast<uint64_t>(lastBurstStart); i < count; i++)
            {
                const auto& r = g_diagRing[(start + i) % DiagRingCapacity];
                if (i > static_cast<uint64_t>(lastBurstStart) && r.index == 1)
                    break;

                trace +=
                    std::format(" [f{}/{}: nat {:.2f}ms det {:.2f}ms wait {:.2f}ms]", r.index, r.countPlus1,
                                r.nativeOutNs / 1000000.0, r.detouredOutNs / 1000000.0, r.schedDurationUs / 1000.0);
            }
            LOG_INFO("XeFG recent burst trace ({}X, ring {:.2f}ms, med {:.2f}ms):{}", multiplier, ringMs, medMs, trace);
        }
    }
}

inline void RecordGap(int64_t endQpc)
{
    if (g_lastPacedQpc != 0)
    {
        const int64_t gap = endQpc - g_lastPacedQpc;
        g_gapSumQpc += gap;

        if (g_gapCount == 0 || gap < g_gapMinQpc)
            g_gapMinQpc = gap;

        if (gap > g_gapMaxQpc)
            g_gapMaxQpc = gap;

        g_gapCount++;
    }

    g_lastPacedQpc = endQpc;

    if (g_statsDeadline == 0)
        ResetStats(endQpc);
    else if (endQpc >= g_statsDeadline)
    {
        ReportStats(g_lastMultiplier);
        ResetStats(endQpc);
    }
}

inline void PushPeriod(int64_t ns)
{
    if (ns <= 0)
        return;

    g_samples[g_samplePos] = ns;
    g_samplePos = (g_samplePos + 1) % SampleCount;

    if (g_sampleCount < SampleCount)
        g_sampleCount++;

    int64_t sorted[SampleCount];
    memcpy(sorted, g_samples, sizeof(int64_t) * g_sampleCount);

    for (int32_t i = 1; i < g_sampleCount; i++)
    {
        int64_t key = sorted[i];
        int32_t j = i - 1;

        while (j >= 0 && sorted[j] > key)
        {
            sorted[j + 1] = sorted[j];
            j--;
        }

        sorted[j + 1] = key;
    }

    g_periodNs = sorted[g_sampleCount / 2];
}

inline void NoteFrame(uint64_t index, uint64_t count, int64_t nowQpc)
{
    if (index > 1)
        return;

    if (g_lastBurstQpc != 0)
    {
        const int64_t totalDeltaNs = NsFromQpc(nowQpc - g_lastBurstQpc);

        if (totalDeltaNs >= 1000000LL && totalDeltaNs <= 500000000LL)
        {
            const int64_t blockNs = NsFromQpc(g_burstBlockQpc);

            // Subtract presentation wait duration from total burst delta BEFORE median filtering
            // so g_renderTimeNs reflects pure unblocked engine render + GPU interpolation time,
            // completely eliminating the self-referential wait inflation feedback loop.
            const int64_t pureNs = (totalDeltaNs > blockNs + 2000000LL) ? (totalDeltaNs - blockNs) : 2000000LL;
            PushPeriod(pureNs);
            g_renderTimeNs = g_periodNs;
        }
    }

    g_burstBlockQpc = 0;
    g_lastBurstQpc = nowQpc;
    g_pacedBursts++;

    if (g_periodNs > 0 && g_freq.QuadPart > 0)
        g_intervalQpc = QpcFromNs(g_periodNs / (static_cast<int64_t>(count) + 1));
}

inline double RenderTimeMs() { return g_renderTimeNs > 0 ? g_renderTimeNs / 1000000.0 : 0.0; }

inline void NoteFedFrameTime(double ms) { g_fedFrameTimeMs = ms; }

inline void WaitUntil(int64_t targetQpc)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    const int64_t yieldBelow = QpcFromNs(200000); // 200 us

    while (now.QuadPart < targetQpc)
    {
        if ((targetQpc - now.QuadPart) > yieldBelow)
            Sleep(0);
        else
            YieldProcessor();

        QueryPerformanceCounter(&now);
    }
}

inline void PaceFrame(uint64_t index, uint64_t count)
{
    const int64_t multiplier = static_cast<int64_t>(count) + 1;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    NoteFrame(index, count, now.QuadPart);

    if (multiplier <= 2)
        return;

    if (index <= 1)
    {
        if (g_periodNs <= 0 || g_intervalQpc <= 0)
            return;

        g_targetQpc = now.QuadPart + g_intervalQpc;

        if (!g_loggedFirstBurst)
        {
            g_loggedFirstBurst = true;
            LOG_INFO("XeFG pacing: {}X, real frame {:.3f} ms -> {:.3f} ms per generated frame", multiplier,
                     g_periodNs / 1000000.0, (g_periodNs / multiplier) / 1000000.0);
        }
    }
    else
    {
        if (g_intervalQpc <= 0)
            return;

        g_targetQpc += g_intervalQpc;
    }

    const int64_t latest = now.QuadPart + QpcFromNs(g_periodNs);
    if (g_targetQpc > latest)
        g_targetQpc = latest;

    WaitUntil(g_targetQpc);

    g_pacedFrames++;

    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);

    RecordGap(end.QuadPart);
}

inline bool SchedForwarder(void* ctx, void* burst, uint8_t gate, void* timing, uint32_t index)
{
    if (g_schedNative == nullptr)
        return false;

    LARGE_INTEGER before;
    QueryPerformanceCounter(&before);

    const bool scheduled = g_schedNative(ctx, burst, gate, timing, index);

    LARGE_INTEGER after;
    QueryPerformanceCounter(&after);

    const int64_t waitQpc = after.QuadPart - before.QuadPart;
    g_schedCalls++;
    g_schedWaitQpc += waitQpc;
    g_burstBlockQpc += waitQpc;

    if (!scheduled)
        g_schedRefused++;

    return scheduled;
}

inline void ScheduleFrame(void* ctx, uint8_t* burst, uint64_t index)
{
    if (g_schedNative == nullptr || g_ringSnapshot == nullptr)
        return;

    g_ring = reinterpret_cast<uint8_t*>(ctx) + RingOffset;

    alignas(16) uint8_t timing[0x20] {};
    g_ringSnapshot(reinterpret_cast<uint8_t*>(ctx) + RingOffset, timing);

    const uint8_t gate = burst[BurstGateOffset] & 1;
    const uint64_t count = *reinterpret_cast<uint64_t*>(burst + 8);

    LARGE_INTEGER before;
    QueryPerformanceCounter(&before);

    NoteFrame(index, count, before.QuadPart);

    const bool scheduled = SchedForwarder(ctx, burst, gate, timing, static_cast<uint32_t>(index));

    LARGE_INTEGER after;
    QueryPerformanceCounter(&after);

    const int64_t waitQpc = after.QuadPart - before.QuadPart;

    if (g_diagSeq > 0)
    {
        auto& d = g_diagRing[(g_diagSeq - 1) % DiagRingCapacity];
        if (d.index == index)
        {
            d.schedDurationUs = g_freq.QuadPart > 0 ? (waitQpc * 1000000LL) / g_freq.QuadPart : 0;
            d.scheduled = scheduled;
        }
    }

    if (g_schedLogged < SchedLogLimit && index != g_schedLastIndex)
    {
        g_schedLastIndex = index;
        g_schedLogged++;

        LOG_INFO("XeFG pacing: frame {}/{} -> {} in {:.3f} ms "
                 "(ctx+0x340 {} ctx+0x341 {}, ring {} samples, median {})",
                 index, count + 1, scheduled ? "scheduled" : "REFUSED", MsFromQpc(after.QuadPart - before.QuadPart),
                 reinterpret_cast<uint8_t*>(ctx)[SchedLimiterOffset],
                 reinterpret_cast<uint8_t*>(ctx)[SchedEnableOffset], *reinterpret_cast<uint64_t*>(timing),
                 *reinterpret_cast<uint64_t*>(timing + 8));
    }
}

inline bool ProviderPacesLastFrame(void* ctx, const uint8_t* burst, uint64_t count)
{
    if (count <= 2)
        return false;

    const auto* ctxBytes = reinterpret_cast<const uint8_t*>(ctx);
    return ctxBytes[LimiterEnabledOffset] != 0 && *reinterpret_cast<const uint32_t*>(burst + BurstLimiterField) == 0;
}

inline void TryPace(void* ctx, void* arg5, void* arg6, uint64_t arg7, bool isLast)
{
    if (arg5 == nullptr)
        return;

    const uint8_t flag = static_cast<uint8_t>(arg7);
    if ((flag == 1) == isLast)
        return;

    auto* burst = reinterpret_cast<uint8_t*>(arg5) - 0x38;
    const uint64_t count = *reinterpret_cast<uint64_t*>(burst + 8);

    if (count < 1 || count > 5)
        return;

    g_lastMultiplier = static_cast<int64_t>(count) + 1;
    uint64_t index = count;

    if (!isLast)
    {
        auto* frames = *reinterpret_cast<uint8_t**>(burst);
        if (frames == nullptr || reinterpret_cast<uint8_t*>(arg6) < frames)
            return;

        const uint64_t offset = reinterpret_cast<uint8_t*>(arg6) - frames;
        if ((offset % 8) != 0)
            return;

        index = offset / 8;
        if (index < 1 || index >= count)
            return;
    }

    // When Intel's scheduler is usable, intermediate frames (1 .. count-1) are scheduled here via
    // ScheduleFrame, and the final generated frame (index = count, isLast = true) is ALREADY
    // scheduled by Intel's native call at 0x220411 (which invokes 0x3100 -> TsDetour for index = count)
    // right before 0x220462 (LastFrameCallerRva). Returning here for isLast avoids double-waiting.
    if (g_schedNative != nullptr && SchedulerUsable(ctx))
    {
        if (!isLast)
            ScheduleFrame(ctx, burst, index);

        return;
    }

    if (isLast && ProviderPacesLastFrame(ctx, burst, count))
        return;

    PaceFrame(index, count);
}

inline int64_t Detour(void* ctx, uint32_t a2, uint32_t a3, uint64_t a4, void* arg5, void* arg6, uint64_t arg7)
{
    if (g_enabled)
    {
        auto* caller = reinterpret_cast<uint8_t*>(XE_RETURN_ADDRESS());

        if (caller == g_base + PacedCallerRva)
        {
            TryPace(ctx, arg5, arg6, arg7, false);
        }
        else if (caller == g_base + LastFrameCallerRva)
        {
            TryPace(ctx, arg5, arg6, arg7, true);
        }
        else if (g_lastMultiplier >= 2 && g_burstStepNs > 0 && g_lastPacedQpc != 0)
        {
            // Caller is the Base Frame present at 0x220A52 / 0x220DAD.
            // Wait one g_burstStepNs from the presentation of Frame count so that Frame count
            // stays on screen for a full step rather than being overwritten in 0.3 ms!
            const int64_t targetQpc = g_lastPacedQpc + QpcFromNs(g_burstStepNs);
            LARGE_INTEGER before;
            QueryPerformanceCounter(&before);

            if (before.QuadPart < targetQpc)
            {
                WaitUntil(targetQpc);
                LARGE_INTEGER after;
                QueryPerformanceCounter(&after);
                g_burstBlockQpc += (after.QuadPart - before.QuadPart);
            }
        }
    }

    const int64_t res = g_native(ctx, a2, a3, a4, arg5, arg6, arg7);

    if (g_enabled)
    {
        LARGE_INTEGER end;
        QueryPerformanceCounter(&end);
        RecordGap(end.QuadPart);
    }

    return res;
}

inline void* TsDetour(void* a1, int64_t* out, void* lookup, void* timing, uint32_t index, uint32_t countPlus1)
{
    void* const result = g_tsNative(a1, out, lookup, timing, index, countPlus1);

    if (out == nullptr || timing == nullptr || index == 0 || index > 5 || countPlus1 < 3)
        return result;

    const int64_t nativeOut = *out;
    const int64_t median = *reinterpret_cast<const int64_t*>(reinterpret_cast<const uint8_t*>(timing) + 8);

    // Calculate target step duration based on pure unblocked render time if available, or ring median
    const int64_t refNs = (g_renderTimeNs > 0) ? g_renderTimeNs : median;
    const int64_t stepNs = std::clamp(refNs / static_cast<int64_t>(countPlus1), 1000000LL, 20000000LL);

    if (stepNs <= 0)
        return result;

    g_tsCalls++;

    LARGE_INTEGER nowQpc {};
    QueryPerformanceCounter(&nowQpc);
    const int64_t nowNs = NsFromQpc(nowQpc.QuadPart);

    const bool fresh = index == 1 || index <= g_lastTsIndex || countPlus1 != g_lastTsCountPlus1;

    if (fresh)
    {
        if (g_ring != nullptr)
        {
            float f = *reinterpret_cast<const float*>(g_ring + RingMeasuredOffset);

            if (!(f >= 0.125f) || f > 500.0f)
                f = 500.0f;

            const int64_t fNs = static_cast<int64_t>(f * 1000000.0f);
            const int64_t clampedUnit = fNs / static_cast<int64_t>(countPlus1);

            if (clampedUnit < stepNs)
                g_tsClamped++;
        }

        // Frame 1 presents immediately when GPU interpolation finishes (0 wait!),
        // because the display was already occupied by the Base Frame during the preceding
        // CPU submission + GPU interpolation window (~16 ms).
        g_nextDeadlineNs = nowNs;
        g_burstStepNs = stepNs;
        g_tsRebased++;
    }
    else
    {
        g_nextDeadlineNs += g_burstStepNs;

        // If a previous frame took longer than expected and less than half a step remains,
        // step forward from current time so the frame never dumps in 0 ms.
        if (nowNs > 0 && g_nextDeadlineNs < nowNs + (g_burstStepNs / 2))
            g_nextDeadlineNs = nowNs + g_burstStepNs;
    }

    g_lastTsIndex = index;
    g_lastTsCountPlus1 = countPlus1;

    *out = g_nextDeadlineNs;

    const uint64_t slot = g_diagSeq++;
    auto& d = g_diagRing[slot % DiagRingCapacity];
    d.index = index;
    d.countPlus1 = countPlus1;
    d.nativeOutNs = nativeOut;
    d.detouredOutNs = g_nextDeadlineNs;
    d.medianNs = median;
    d.ringMeasuredMs = (g_ring != nullptr) ? *reinterpret_cast<const float*>(g_ring + RingMeasuredOffset) : 0.0f;
    d.schedDurationUs = 0;
    d.scheduled = false;

    return result;
}

inline bool WriteVerified(uint8_t* dst, const uint8_t* bytes, uint32_t size)
{
    DWORD oldProtect = 0;

    if (!VirtualProtect(dst, size, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;

    memcpy(dst, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), dst, size);

    DWORD ignored = 0;
    VirtualProtect(dst, size, oldProtect, &ignored);

    return memcmp(dst, bytes, size) == 0;
}

inline bool HookThunk(uint8_t* base, uint32_t rva, uint32_t targetRva, const uint8_t* expected, void* detour,
                      void** nativeOut, uint8_t* origOut, bool* hookedFlag)
{
    uint8_t* thunk = base + rva;

    if (memcmp(thunk, expected, PresentThunkSize) != 0)
    {
        LOG_WARN("XeFG pacing: thunk at {:#x} has unexpected bytes, not hooking", rva);
        return false;
    }

    if (origOut)
        memcpy(origOut, thunk, PresentThunkSize);

    uint8_t replacement[PresentThunkSize] = {
        0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    };

    auto target = reinterpret_cast<uint64_t>(detour);
    memcpy(replacement + 6, &target, sizeof(target));

    if (!WriteVerified(thunk, replacement, PresentThunkSize))
    {
        LOG_WARN("XeFG pacing: failed to write the thunk at {:#x}", rva);
        return false;
    }

    *nativeOut = base + targetRva;
    if (hookedFlag)
        *hookedFlag = true;

    return true;
}

inline unsigned int InstalledDetourCount()
{
    unsigned int count = 0;
    if (g_presentHooked)
        count++;
    if (g_schedHooked)
        count++;
    if (g_tsHooked)
        count++;
    return count;
}

inline bool Install(uint8_t* base)
{
    if (g_enabled)
        return true;

    if (base == nullptr)
        return false;

    QueryPerformanceFrequency(&g_freq);

    if (g_freq.QuadPart <= 0)
    {
        LOG_WARN("XeFG pacing: no performance counter, not hooking");
        return false;
    }

    g_base = base;
    g_lastBurstQpc = 0;
    g_periodNs = 0;
    g_intervalQpc = 0;
    g_targetQpc = 0;
    g_sampleCount = 0;
    g_samplePos = 0;
    g_pacedFrames = 0;
    g_pacedBursts = 0;
    g_loggedFirstBurst = false;
    g_lastPacedQpc = 0;
    g_schedLogged = 0;
    g_schedLastIndex = ~0ull;
    g_schedCalls = 0;
    g_schedRefused = 0;
    g_schedWaitQpc = 0;
    g_lastMultiplier = 0;
    g_burstBlockQpc = 0;
    g_renderTimeNs = 0;
    g_fedFrameTimeMs = 0.0;
    g_ring = nullptr;
    g_nextDeadlineNs = 0;
    g_burstStepNs = 0;
    g_lastTsIndex = 0;
    g_lastTsCountPlus1 = 0;
    g_tsCalls = 0;
    g_tsRebased = 0;
    g_tsClamped = 0;
    g_diagSeq = 0;
    ResetStats(0);

    void* native = nullptr;

    if (!HookThunk(base, PresentThunkRva, NativePresentRva, PresentThunkExpected, reinterpret_cast<void*>(&Detour),
                   &native, g_presentThunkOrig, &g_presentHooked))
        return false;

    g_native = reinterpret_cast<PresentFn>(native);
    g_ringSnapshot = reinterpret_cast<RingSnapshotFn>(base + RingSnapshotFnRva);
    g_enabled = true;

    LOG_INFO("XeFG pacing: generated frames are paced above 2X (thunk {:#x} -> {:#x})", PresentThunkRva,
             reinterpret_cast<uint64_t>(&Detour));

    g_schedNative = reinterpret_cast<SchedFn>(base + SchedFnRva);

    if (HookThunk(base, SchedThunkRva, SchedFnRva, SchedThunkExpected, reinterpret_cast<void*>(&SchedForwarder),
                  &native, g_schedThunkOrig, &g_schedHooked))
    {
        LOG_INFO("XeFG pacing: scheduling every generated frame through the provider's own scheduler (thunk {:#x})",
                 SchedThunkRva);
    }
    else
    {
        g_schedNative = nullptr;
        LOG_WARN("XeFG pacing: no scheduler, falling back to the wall clock");
    }

    g_tsNative = reinterpret_cast<TimestampFn>(base + TimestampFnRva);

    if (g_schedNative != nullptr && HookThunk(base, TimestampThunkRva, TimestampFnRva, TimestampThunkExpected,
                                              reinterpret_cast<void*>(&TsDetour), &native, g_tsThunkOrig, &g_tsHooked))
    {
        LOG_INFO("XeFG pacing: deadlines come from the burst's own frame interval (thunk {:#x})", TimestampThunkRva);
    }
    else
    {
        g_tsNative = nullptr;
    }

    return true;
}

inline void Uninstall()
{
    if (!g_base)
        return;

    if (g_presentHooked)
    {
        WriteVerified(g_base + PresentThunkRva, g_presentThunkOrig, PresentThunkSize);
        g_presentHooked = false;
    }

    if (g_schedHooked)
    {
        WriteVerified(g_base + SchedThunkRva, g_schedThunkOrig, PresentThunkSize);
        g_schedHooked = false;
    }

    if (g_tsHooked)
    {
        WriteVerified(g_base + TimestampThunkRva, g_tsThunkOrig, PresentThunkSize);
        g_tsHooked = false;
    }

    g_ring = nullptr;
    g_nextDeadlineNs = 0;
    g_burstStepNs = 0;
    g_lastTsIndex = 0;
    g_lastTsCountPlus1 = 0;
    g_diagSeq = 0;
    g_enabled = false;
    g_native = nullptr;
    g_schedNative = nullptr;
    g_tsNative = nullptr;
    g_ringSnapshot = nullptr;
    g_base = nullptr;
}
} // namespace XeFGPacing

#pragma once

// Spreads Intel's generated frames out in time, so that multi frame generation
// above 2X stops arriving as one clump followed by a gap.
//
// The provider already knows what the spacing should be. Once per burst it
// works out how long a real frame took and divides it by the number of frames
// the burst will deliver:
//
//     0x220254  mov  r9, [r8+8]        ; count = generated frames in this burst
//     0x220258  lea  rcx, [r9+1]       ; count + 1 = the multiplier
//     0x22025E  mov  rax, [rbp-0x41]   ; duration of the real frame
//     0x220262  div  rcx
//     0x220265  mov  r12, rax          ; -> duration / multiplier, per frame
//
// but then the present loop that follows hands every generated frame straight
// to the swapchain back to back:
//
//     0x220280  ...                    ; loop over rbx = 1 .. count-1
//     0x2202E8  call 0x1800025c0       ; -> jmp 0x18021f730, present one frame
//     0x2202ED  ...
//     0x22030A  jb   0x180220280
//
// and `r12` is not consumed until the loop has finished, by the FPS limiter
// block at 0x220317 - which is itself only reached when count > 2 and two other
// conditions hold. So the burst goes out as fast as the swapchain accepts it and
// the limiter then holds the *last* frame of the burst back, which is what reads
// as frames being bunched up and out of order above 2X.
//
// The last frame is not presented from the loop at all. It goes out from the
// call at 0x220462, after the loop and after the provider has submitted the
// burst, so it needs pacing of its own:
//
//     0x220411  call 0x180003100       ; -> jmp 0x18021ee30, submit the burst
//     0x220445  mov  byte [rsp+0x30], 0
//     0x220462  call 0x1800025c0       ; present the last generated frame
//
// Fixing it means pacing each generated frame, which means getting in between
// the loop and the present. Every one of those presents goes through the 5 byte
// thunk at 0x25C0, and that thunk is followed by 11 bytes of int3 padding, so
// it is 16 bytes that contain nothing but a jump. That is room for a 14 byte
// `jmp qword ptr [rip+0]`, so the thunk can be redirected anywhere in the
// address space without relocating a single instruction and without a code cave.
//
// The thunk has two other callers (0x220A52, 0x220DAD), so the detour checks
// the return address and paces only the two sites above; every other present is
// forwarded untouched.
//
// The spacing is not invented here. The provider has a frame scheduler of its
// own - it just never calls it for the frames the loop presents, only for the
// burst's last one. So each of those presents is handed to 0x21EE30 with the
// arguments the present path already has, and the provider works out the
// presentation time itself. A wall clock does the same job when the scheduler
// cannot be reached; waiting on the provider's D3D12 fence as well was tried and
// then removed due to timer tick quantization overshooting.

#include <windows.h>
#ifdef _MSC_VER
#include <intrin.h>
#define XE_RETURN_ADDRESS() _ReturnAddress()
#else
#define XE_RETURN_ADDRESS() __builtin_return_address(0)
#endif

#include "Config.h"
#include "Logger.h"
#include "SysUtils.h"

namespace XeFGPacing
{
// --- provider internals, libxess_fg.dll 1.3.1.78 ------------------------

constexpr uint32_t PresentThunkRva = 0x25C0;
constexpr uint32_t NativePresentRva = 0x21F730;

// Return address of the `call` in the burst loop - i.e. the exact identity
// of the call site we want to pace.
constexpr uint32_t PacedCallerRva = 0x2202ED;

// The burst's last generated frame is not presented from the loop. It goes
// out after the loop and after the provider's own submit, from the call at
// 0x220462, with the same burst in arg5 but a different arg6 and arg7 = 0
// instead of 1:
//
//     0x220445  mov  byte ptr [rsp+0x30], 0   ; arg7 = 0
//     0x220462  call 0x25c0
//
// Left alone, every burst ends with two frames carrying the same timestamp:
// at 4X the delivered pattern is 0, i, 2i, 2i instead of 0, i, 2i, 3i.
constexpr uint32_t LastFrameCallerRva = 0x220467;

// The provider does space that last frame itself - but only from the limiter
// block at 0x220317, and only when all three of its conditions hold:
//
//     0x220317  cmp  byte  ptr [rsi + 0x340], 0   ; enabled
//     0x220324  cmp  qword ptr [r8 + 8], 2        ; count > 2, i.e. 4X up
//     0x22032F  cmp  dword ptr [r8 + 0x28], 0     ; per-burst field is 0
//
// Mirroring the condition keeps exactly one of us waiting on that frame
// rather than two waits stacking into one long frame.
constexpr uint32_t LimiterEnabledOffset = 0x340;
constexpr uint32_t BurstLimiterField = 0x28;

// --- the provider's own frame scheduler --------------------------------

// 0x3100 schedules the frame: look the frame's record up in the ring at
// ctx+0x168 by burst->tag, stamp it with the current counter, then have
// 0x224B30 (via 0x3430) compute a presentation time and hand that time to 0x7A30.
// Its arg5 is the frame's index in the burst.
constexpr uint32_t SchedThunkRva = 0x3100;
constexpr uint32_t SchedFnRva = 0x21EE30;

// arg2 of the scheduler is present arg5 - 0x38, so the gate byte the
// present path reads into r8 at 0x22040A sits at 0xC0 of it.
constexpr uint32_t BurstGateOffset = 0xC0;

// The scheduler is not always on. Its entry gate is 0xdbb0 -> 0x21ed40, and
// the whole of it is two bytes of the context:
//     [ctx+0x340] == 0 && [ctx+0x341] != 0
constexpr uint32_t SchedLimiterOffset = 0x340;
constexpr uint32_t SchedEnableOffset = 0x341;

// arg4 comes out of 0x4DA0, which is a thunk for 0x224CF0.
constexpr uint32_t RingSnapshotFnRva = 0x224CF0;
constexpr uint32_t RingOffset = 0x168;

// Every frame that gets a deadline ends up waiting in 0x7A30 -> 0x225070,
// and the value it waits on is produced by 0x3430 -> 0x224B30.
constexpr uint32_t TimestampThunkRva = 0x3430;
constexpr uint32_t TimestampFnRva = 0x224B30;

// The float 0x224B30 clamps, at ring + 0x1B8.
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

inline int64_t NsFromQpc(int64_t delta) { return g_freq.QuadPart > 0 ? (delta * 1000000000LL) / g_freq.QuadPart : 0; }

inline int64_t QpcFromNs(int64_t ns) { return g_freq.QuadPart > 0 ? (ns * g_freq.QuadPart) / 1000000000LL : 0; }

inline double MsFromQpc(int64_t qpc) { return g_freq.QuadPart > 0 ? (qpc * 1000.0) / g_freq.QuadPart : 0.0; }

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
        PushPeriod(NsFromQpc(nowQpc - g_lastBurstQpc));

    if (g_periodNs > 0)
    {
        const int64_t blockNs = NsFromQpc(g_burstBlockQpc);
        g_renderTimeNs = g_periodNs > blockNs ? g_periodNs - blockNs : 0;
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

    if (multiplier <= 2)
        return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    NoteFrame(index, count, now.QuadPart);

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

    const bool scheduled = g_schedNative(ctx, burst, gate, timing, static_cast<uint32_t>(index));

    LARGE_INTEGER after;
    QueryPerformanceCounter(&after);

    g_schedCalls++;

    if (!scheduled)
        g_schedRefused++;

    g_schedWaitQpc += after.QuadPart - before.QuadPart;
    g_burstBlockQpc += after.QuadPart - before.QuadPart;

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

    RecordGap(after.QuadPart);
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
            TryPace(ctx, arg5, arg6, arg7, false);
        else if (caller == g_base + LastFrameCallerRva)
            TryPace(ctx, arg5, arg6, arg7, true);
    }

    return g_native(ctx, a2, a3, a4, arg5, arg6, arg7);
}

inline bool SchedForwarder(void* ctx, void* burst, uint8_t gate, void* timing, uint32_t index)
{
    return g_schedNative != nullptr && g_schedNative(ctx, burst, gate, timing, index);
}

inline void* TsDetour(void* a1, int64_t* out, void* lookup, void* timing, uint32_t index, uint32_t countPlus1)
{
    void* const result = g_tsNative(a1, out, lookup, timing, index, countPlus1);

    if (out == nullptr || timing == nullptr || index == 0 || index > 5 || countPlus1 < 2)
        return result;

    const int64_t median = *reinterpret_cast<const int64_t*>(reinterpret_cast<const uint8_t*>(timing) + 8);
    const int64_t unit = median / static_cast<int64_t>(countPlus1);

    if (unit <= 0)
        return result;

    g_tsCalls++;

    const bool fresh = index == 1 || index <= g_lastTsIndex || countPlus1 != g_lastTsCountPlus1;

    if (fresh)
    {
        int64_t nativeUnit = unit;

        if (g_ring != nullptr)
        {
            float f = *reinterpret_cast<const float*>(g_ring + RingMeasuredOffset);

            if (!(f >= 0.125f) || f > 500.0f)
                f = 500.0f;

            const int64_t fNs = static_cast<int64_t>(f * 1000000.0f);
            const int64_t clampedUnit = fNs / static_cast<int64_t>(countPlus1);

            if (clampedUnit < nativeUnit)
            {
                nativeUnit = clampedUnit;
                g_tsClamped++;
            }
        }

        g_nextDeadlineNs = *out + static_cast<int64_t>(index) * (unit - nativeUnit);
        g_burstStepNs = unit;
        g_tsRebased++;
    }
    else
    {
        g_nextDeadlineNs += g_burstStepNs;
    }

    g_lastTsIndex = index;
    g_lastTsCountPlus1 = countPlus1;

    *out = g_nextDeadlineNs;

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

    auto config = Config::Instance();
    if (config && !config->XeMfgExtraPacing.value_or(true))
    {
        LOG_INFO("XeFG pacing: disabled by config (XeMFG\\ExtraPacing)");
        return false;
    }

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

    g_enabled = false;
    g_native = nullptr;
    g_schedNative = nullptr;
    g_tsNative = nullptr;
    g_ringSnapshot = nullptr;
    g_base = nullptr;
}
} // namespace XeFGPacing

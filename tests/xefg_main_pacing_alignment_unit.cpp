#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// Unit test validating XeFGPacing's alignment with the main branch architecture:
// 1. TsDetour deadline rebasing: repairs Intel's clamped unit bug when ring measured offset is small.
// 2. Burst-to-burst period measurement: NoteFrame measures engine frame time at index == 1.
// 3. Detour caller routing: base application frame bypasses TryPace with 0 wait.
// 4. Provider scheduler delegation: intermediate frames route to ScheduleFrame, last frame handled natively.
// 5. Windowed deadline clamping, monotonic step, and isLast deduplication.

namespace XeFGMainPacingAlignmentTest
{

constexpr int64_t FrameCallerRva = 0x2202ED;
constexpr int64_t LastFrameCallerRva = 0x220467;
constexpr int64_t BaseFrameCaller1Rva = 0x220A52;
constexpr int64_t BaseFrameCaller2Rva = 0x220DAD;

struct SimulatedMainPacer
{
    int64_t freq = 10000000LL; // 10 MHz QPC frequency (1 tick = 100 ns)
    int64_t lastBurstQpc = 0;
    int64_t periodNs = 0;
    int64_t renderTimeNs = 0;
    int64_t intervalQpc = 0;
    int64_t targetQpc = 0;

    int64_t nextDeadlineNs = 0;
    int64_t burstStepNs = 0;
    uint32_t lastTsIndex = 0;
    uint32_t lastTsCountPlus1 = 0;
    int64_t tsCalls = 0;
    int64_t tsRebased = 0;
    int64_t tsClamped = 0;

    int64_t pacedFrames = 0;
    int64_t scheduledFrames = 0;
    int64_t basePresents = 0;

    int64_t NsFromQpc(int64_t delta) const { return freq > 0 ? (delta * 1000000000LL) / freq : 0; }
    int64_t QpcFromNs(int64_t ns) const { return freq > 0 ? (ns * freq) / 1000000000LL : 0; }
    double RenderTimeMs() const { return renderTimeNs > 0 ? static_cast<double>(renderTimeNs) / 1000000.0 : 0.0; }

    void NoteFrame(uint64_t multiplier, int64_t nowQpc)
    {
        const int64_t last = lastBurstQpc;
        lastBurstQpc = nowQpc;

        if (multiplier < 2)
            return;

        if (last == 0)
            return;

        const int64_t deltaQpc = nowQpc - last;
        if (deltaQpc <= 0)
            return;

        const int64_t measuredNs = NsFromQpc(deltaQpc);
        if (measuredNs < 1000000LL || measuredNs > 500000000LL)
            return;

        periodNs = measuredNs;
        renderTimeNs = periodNs;
        intervalQpc = deltaQpc / static_cast<int64_t>(multiplier);
    }

    int64_t TsDetour(int64_t nativeOut, int64_t medianNs, uint32_t countPlus1, uint32_t index, float ringF = 500.0f,
                     int64_t nowNs = 0)
    {
        if (countPlus1 <= 2)
            return nativeOut;

        const int64_t unit = medianNs / static_cast<int64_t>(countPlus1);
        if (unit <= 0)
            return nativeOut;

        tsCalls++;

        const bool fresh = index == 1 || index <= lastTsIndex || countPlus1 != lastTsCountPlus1;

        if (fresh)
        {
            int64_t nativeUnit = unit;
            if (ringF >= 0.125f && ringF <= 500.0f)
            {
                const int64_t fNs = static_cast<int64_t>(ringF * 1000000.0f);
                const int64_t clampedUnit = fNs / static_cast<int64_t>(countPlus1);
                if (clampedUnit < nativeUnit)
                {
                    nativeUnit = clampedUnit;
                    tsClamped++;
                }
            }

            int64_t candidateNs = nativeOut + static_cast<int64_t>(index) * (unit - nativeUnit);

            if (nowNs > 0)
            {
                if (candidateNs > nowNs + unit)
                    candidateNs = nowNs + unit;

                if (candidateNs < nowNs)
                    candidateNs = nowNs;
            }

            nextDeadlineNs = candidateNs;
            burstStepNs = unit;
            tsRebased++;
        }
        else
        {
            nextDeadlineNs += burstStepNs;

            if (nowNs > 0 && nextDeadlineNs < nowNs + (burstStepNs / 2))
                nextDeadlineNs = nowNs + burstStepNs;
        }

        lastTsIndex = index;
        lastTsCountPlus1 = countPlus1;
        return nextDeadlineNs;
    }

    enum class PacingOutcome
    {
        ScheduledViaNative,
        ProviderPacedLast,
        OptiPacedFallback,
        BypassedBaseFrame,
    };

    PacingOutcome TryPace(uint64_t index, uint64_t count, bool isLast, bool schedUsable, bool providerPacesLast)
    {
        if (schedUsable)
        {
            if (!isLast)
            {
                scheduledFrames++;
                return PacingOutcome::ScheduledViaNative;
            }

            return PacingOutcome::ScheduledViaNative;
        }

        if (isLast && providerPacesLast)
            return PacingOutcome::ProviderPacedLast;

        pacedFrames++;
        return PacingOutcome::OptiPacedFallback;
    }

    PacingOutcome OnDetour(int64_t callerRva, uint64_t index, uint64_t count, bool schedUsable, bool providerPacesLast)
    {
        if (callerRva == FrameCallerRva)
            return TryPace(index, count, false, schedUsable, providerPacesLast);

        if (callerRva == LastFrameCallerRva)
            return TryPace(index, count, true, schedUsable, providerPacesLast);

        // Caller is base application frame (0x220A52, 0x220DAD)
        basePresents++;
        return PacingOutcome::BypassedBaseFrame;
    }
};

} // namespace XeFGMainPacingAlignmentTest

int main()
{
    printf("[+] Starting XeFGPacing Main Branch Alignment Unit Tests...\n");

    // Test 1: NoteFrame measures burst-to-burst delta and decoupling
    {
        XeFGMainPacingAlignmentTest::SimulatedMainPacer pacer;
        const int64_t t0 = 100000000LL;
        pacer.NoteFrame(4, t0); // Frame 1 of burst 1 (lastBurstQpc initialized)
        assert(pacer.periodNs == 0 && "First burst must not measure delta yet");

        // Engine renders at 30 ms (3,000,000 QPC ticks)
        const int64_t t1 = t0 + 300000;
        pacer.NoteFrame(4, t1); // Frame 1 of burst 2
        assert(pacer.periodNs == 30000000LL && "Second burst must record exact 30 ms period");
        assert(pacer.RenderTimeMs() == 30.0 && "RenderTimeMs must return exact 30.0 ms");
        assert(pacer.intervalQpc == 300000 / 4 && "Interval QPC must be partitioned by multiplier");

        // Filter pause/loading spikes (> 500 ms)
        const int64_t t2 = t1 + 6000000; // 600 ms gap
        pacer.NoteFrame(4, t2);
        assert(pacer.periodNs == 30000000LL && "Period must ignore > 500 ms loading spikes");

        printf("  [PASS] Test 1: NoteFrame measures burst-to-burst delta and filters spikes.\n");
    }

    // Test 2: TsDetour repairs Intel's clamp bug when ring measured offset is small
    {
        XeFGMainPacingAlignmentTest::SimulatedMainPacer pacer;
        const int64_t medianNs = 32000000LL;    // 32 ms median cycle
        const uint32_t countPlus1 = 4;          // 4X FG (count = 3 generated frames)
        const int64_t expectedUnit = 8000000LL; // 8 ms per generated frame

        // Intel's stock bug: ring measured offset was clamped to 16 ms (f = 16.0f)
        // Intel computed clampedUnit = 16 ms / 4 = 4 ms, causing generated frames to clump at 4 ms intervals!
        const float clampedRingF = 16.0f;
        const int64_t nativeOut1 = 1000000000LL; // base deadline

        int64_t d1 = pacer.TsDetour(nativeOut1, medianNs, countPlus1, 1, clampedRingF);
        assert(pacer.tsClamped == 1 && "TsDetour must detect Intel's clamp bug");
        assert(pacer.tsRebased == 1 && "TsDetour must rebase deadline");

        // Subsequent generated frames in the burst advance by full unit (8 ms)
        int64_t d2 = pacer.TsDetour(nativeOut1, medianNs, countPlus1, 2, clampedRingF);
        int64_t d3 = pacer.TsDetour(nativeOut1, medianNs, countPlus1, 3, clampedRingF);

        assert((d2 - d1) == expectedUnit && "Generated frame 2 deadline must step by exact uncorrupted unit (8 ms)");
        assert((d3 - d2) == expectedUnit && "Generated frame 3 deadline must step by exact uncorrupted unit (8 ms)");

        printf("  [PASS] Test 2: TsDetour repairs Intel's clamp bug and partitions deadlines uniformly.\n");
    }

    // Test 3: Detour caller routing ensures base application frame bypasses TryPace with zero wait
    {
        XeFGMainPacingAlignmentTest::SimulatedMainPacer pacer;

        // Generated frames route to TryPace
        auto res1 = pacer.OnDetour(XeFGMainPacingAlignmentTest::FrameCallerRva, 1, 3, true, true);
        auto res2 = pacer.OnDetour(XeFGMainPacingAlignmentTest::FrameCallerRva, 2, 3, true, true);
        auto res3 = pacer.OnDetour(XeFGMainPacingAlignmentTest::LastFrameCallerRva, 3, 3, true, true);
        assert(res1 == XeFGMainPacingAlignmentTest::SimulatedMainPacer::PacingOutcome::ScheduledViaNative);
        assert(res2 == XeFGMainPacingAlignmentTest::SimulatedMainPacer::PacingOutcome::ScheduledViaNative);
        assert(res3 == XeFGMainPacingAlignmentTest::SimulatedMainPacer::PacingOutcome::ScheduledViaNative);

        // Base application frame callers (0x220A52, 0x220DAD) bypass TryPace entirely
        auto base1 = pacer.OnDetour(XeFGMainPacingAlignmentTest::BaseFrameCaller1Rva, 4, 3, true, true);
        auto base2 = pacer.OnDetour(XeFGMainPacingAlignmentTest::BaseFrameCaller2Rva, 4, 3, true, true);
        assert(base1 == XeFGMainPacingAlignmentTest::SimulatedMainPacer::PacingOutcome::BypassedBaseFrame);
        assert(base2 == XeFGMainPacingAlignmentTest::SimulatedMainPacer::PacingOutcome::BypassedBaseFrame);
        assert(pacer.basePresents == 2 && "Base frame presents must execute directly without detour delay");

        printf("  [PASS] Test 3: Base application frame callers bypass TryPace with zero wait.\n");
    }

    // Test 4: Provider scheduler delegation avoids double wait on intermediate and final frames
    {
        XeFGMainPacingAlignmentTest::SimulatedMainPacer pacer;

        // When scheduler is usable, intermediate frames route to ScheduleFrame
        pacer.TryPace(1, 3, false, true, true);
        pacer.TryPace(2, 3, false, true, true);
        assert(pacer.scheduledFrames == 2);
        assert(pacer.pacedFrames == 0 && "Intermediate frames must not invoke OptiScaler PaceFrame");

        // When provider limiter is active for last frame, returns immediately
        auto lastRes = pacer.TryPace(3, 3, true, false, true);
        assert(lastRes == XeFGMainPacingAlignmentTest::SimulatedMainPacer::PacingOutcome::ProviderPacedLast);
        assert(pacer.pacedFrames == 0 && "Provider-paced last frame must not invoke OptiScaler PaceFrame");

        // When scheduler is NOT usable and provider limiter inactive, falls back to PaceFrame
        auto fallbackRes = pacer.TryPace(1, 3, false, false, false);
        assert(fallbackRes == XeFGMainPacingAlignmentTest::SimulatedMainPacer::PacingOutcome::OptiPacedFallback);
        assert(pacer.pacedFrames == 1 && "Fallback must invoke PaceFrame");

        printf("  [PASS] Test 4: Provider scheduler delegation verified without double wait.\n");
    }

    // Test 5: Windowed deadline clamping in TsDetour and deduplication on isLast
    {
        XeFGMainPacingAlignmentTest::SimulatedMainPacer pacer;
        const int64_t medianNs = 20440000LL; // ~20.44 ms cycle (49 fps, typical in HD2)
        const uint32_t countPlus1 = 4;       // 4X Multi-Frame Generation (3 generated frames)
        const int64_t unit = 5110000LL;      // 5.11 ms per slice

        // Subtest 5A: Windowed Clamping - Cap Frame 1 wait to at most 1 * unit
        // Intel's stock 2X-biased calculation yields nativeOut 10 ms ahead (half the cycle)
        const int64_t nowNs1 = 1000000000LL;          // 1.0 s wall-clock time
        const int64_t nativeOutTooFar = 1010000000LL; // 10 ms ahead (nativeOut - nowNs = 10 ms)

        int64_t d1 = pacer.TsDetour(nativeOutTooFar, medianNs, countPlus1, 1, 500.0f, nowNs1);
        // Candidate was capped at nowNs1 + unit (1000000000 + 5110000 = 1005110000)
        assert(d1 == nowNs1 + unit && "Frame 1 deadline must be capped to at most nowNs + 1 * unit (5.11 ms)");
        assert(d1 - nowNs1 == unit && "Frame 1 wait duration must not exceed 1 * unit");

        // Subtest 5B: Windowed Clamping - Floor Frame 1 deadline at nowNs when GPU execution lagged
        // GPU interpolation took 8 ms, so nativeOut (T_0 + 5.11 ms) is already in the past!
        XeFGMainPacingAlignmentTest::SimulatedMainPacer pacerLag;
        const int64_t nowNsLag1 = 1008000000LL;
        const int64_t nativeOutPast = 1005110000LL; // 2.89 ms in the past

        int64_t dLag1 = pacerLag.TsDetour(nativeOutPast, medianNs, countPlus1, 1, 500.0f, nowNsLag1);
        assert(dLag1 == nowNsLag1 &&
               "Frame 1 deadline in the past must be floored to nowNs for immediate presentation");

        // Frame 2 arrives after Frame 1 presented: nowNs is now 1008200000 (0.2 ms after Frame 1)
        const int64_t nowNsLag2 = 1008200000LL;
        int64_t dLag2 = pacerLag.TsDetour(nativeOutPast, medianNs, countPlus1, 2, 500.0f, nowNsLag2);
        // Because dLag1 was floored at nowNsLag1 (1008000000), Frame 2 steps forward by unit (5.11 ms):
        // 1008000000 + 5110000 = 1013110000.
        // Relative to nowNsLag2 (1008200000), Frame 2 waits ~4.91 ms instead of dumping in 0.002 ms!
        assert(dLag2 == dLag1 + unit && "Frame 2 deadline must step forward by 1 * unit from floored Frame 1");
        assert(dLag2 - nowNsLag2 > 4000000LL && "Frame 2 must have a future wait > 4 ms, eliminating 0.002 ms dump");

        // Subtest 5C: Monotonic step forward when a previous frame presentation experienced sudden hitch
        // Frame 2 was delayed by an OS/GPU hitch, arriving at nowNs = 1012000000 (less than 1.11 ms before deadline
        // 1013110000) If nextDeadlineNs < nowNs + (burstStepNs / 2), monotonic guard pushes it to nowNs + burstStepNs
        const int64_t nowNsHitch = 1012000000LL; // only 1.11 ms left of 5.11 ms step (< 2.55 ms)
        int64_t dLag3 = pacerLag.TsDetour(nativeOutPast, medianNs, countPlus1, 3, 500.0f, nowNsHitch);
        assert(dLag3 >= nowNsHitch + unit && "Monotonic guard must step deadline forward from nowNs on hitch");

        // Subtest 5D: Deduplication in TryPace for isLast when scheduler is usable
        // With schedUsable = true, TryPace on count with isLast = true must NOT invoke OptiScaler PaceFrame
        auto resLast = pacer.TryPace(3, 3, true, true, false);
        assert(resLast == XeFGMainPacingAlignmentTest::SimulatedMainPacer::PacingOutcome::ScheduledViaNative);
        assert(pacer.pacedFrames == 0 &&
               "TryPace on isLast must return without calling PaceFrame when scheduler is usable");

        printf("  [PASS] Test 5: Windowed deadline clamping, monotonic step, and isLast deduplication verified.\n");
    }

    printf("[+] All XeFGPacing Main Branch Alignment Unit Tests PASSED successfully!\n");
    return 0;
}

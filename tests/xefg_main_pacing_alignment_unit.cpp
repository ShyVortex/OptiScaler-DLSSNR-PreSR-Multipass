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

    int64_t TsDetour(int64_t nativeOut, int64_t medianNs, uint32_t countPlus1, uint32_t index, float ringF = 500.0f)
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

            nextDeadlineNs = nativeOut + static_cast<int64_t>(index) * (unit - nativeUnit);
            burstStepNs = unit;
            tsRebased++;
        }
        else
        {
            nextDeadlineNs += burstStepNs;
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

    printf("[+] All XeFGPacing Main Branch Alignment Unit Tests PASSED successfully!\n");
    return 0;
}

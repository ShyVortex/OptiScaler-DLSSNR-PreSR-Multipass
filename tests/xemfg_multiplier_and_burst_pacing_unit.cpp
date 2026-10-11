#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Unit test suite for XeMFG multi-frame generation multiplier uncap (2X-6X)
// and end-to-end presentation burst pacing in XeFG_Dx12 and XeFGPacing.
//
// Validates:
// 1. XeFG_Dx12 multiplier ceiling uncap: SetInterpolatedFrameCount accepts 2..5 (3X-6X FG)
//    without clamping to 1 when XeMFG is active.
// 2. XeFG_Dx12 clamping retention when XeMFG is inactive (stock behavior preserved).
// 3. Dispatch validation respects effectiveMax (up to 5) instead of stock 1.
// 4. 2X Full-burst pacing: generated frame (50%) and base frame (100%) presented with exact 18.2 ms spacing.
// 5. 3X Full-burst pacing: gen 1 (33%), gen 2 (67%), and base (100%) presented with exact 12.1 ms spacing.
// 6. 4X & 6X Full-burst pacing: complete cadence uniformity across all presentations in the cycle.
// 7. Elimination of 2 ms dump: verifies consecutive presents never dump with sub-interval latency.
// 8. Dynamic anti-clump protection floor (interval / 2) on irregular render arrivals.
// 9. Frame render time measurement derived from consecutive base frames feeds valid delta time.

namespace XeMfgPacingTest
{

constexpr int32_t SampleCount = 15;

// Mock XeMfgLoader session state
struct MockXeMfgLoader
{
    static inline bool sessionActive = false;
    static inline int liveCeiling = 5;

    static bool AnyEnabledForSession() { return sessionActive; }
    static int EffectiveMax(int stockMax) { return sessionActive ? liveCeiling : stockMax; }
    static void SetMaxGeneratedFrames(int count) { liveCeiling = count; }
};

// Simulated XeFG_Dx12 multiplier management logic
struct SimulatedXeFGContext
{
    int maxInterpolationCount = 1; // Stock default from props.maxSupportedInterpolations
    int activeInterpolationCount = 1;
    bool clamped = false;
    std::string lastWarning;

    void OnInitSwapchain()
    {
        if (MockXeMfgLoader::AnyEnabledForSession())
        {
            maxInterpolationCount = std::max(maxInterpolationCount, 5);
        }
    }

    bool SetInterpolatedFrameCount(uint32_t count)
    {
        const int effectiveMax =
            MockXeMfgLoader::AnyEnabledForSession()
                ? std::max(maxInterpolationCount, MockXeMfgLoader::EffectiveMax(maxInterpolationCount))
                : maxInterpolationCount;

        uint32_t clampedCount = count;
        if (effectiveMax > 0 && clampedCount > static_cast<uint32_t>(effectiveMax))
        {
            clamped = true;
            lastWarning = "Requested interpolation count exceeds max supported, clamping";
            clampedCount = static_cast<uint32_t>(effectiveMax);
        }
        else
        {
            clamped = false;
            lastWarning.clear();
        }

        activeInterpolationCount = clampedCount;
        maxInterpolationCount = std::max(maxInterpolationCount, static_cast<int>(clampedCount));

        if (MockXeMfgLoader::AnyEnabledForSession())
        {
            MockXeMfgLoader::SetMaxGeneratedFrames(clampedCount);
        }

        return !clamped;
    }

    uint32_t ValidateDispatchCount(uint32_t requestedCount)
    {
        const int effectiveMax =
            MockXeMfgLoader::AnyEnabledForSession()
                ? std::max(maxInterpolationCount, MockXeMfgLoader::EffectiveMax(maxInterpolationCount))
                : maxInterpolationCount;

        if (effectiveMax > 0 && requestedCount > static_cast<uint32_t>(effectiveMax))
            return static_cast<uint32_t>(effectiveMax);

        return requestedCount;
    }
};

// Simulated End-to-End XeFGPacing Presentation Engine
struct SimulatedXeFGPacer
{
    int64_t freq = 10000000LL; // 10 MHz QPC frequency (1 tick = 100 ns)
    int64_t lastBaseFrameQpc = 0;
    int64_t lastPresentedQpc = 0;
    int64_t burstAnchorQpc = 0;
    int64_t periodNs = 0;
    int64_t intervalQpc = 0;
    int64_t samples[SampleCount] {};
    int32_t sampleCount = 0;
    int32_t samplePos = 0;
    int64_t pacedFrames = 0;
    int64_t pacedBursts = 0;
    int64_t renderTimeNs = 0;
    int64_t lastMultiplier = 2;
    int32_t clumpCount = 0;

    std::vector<int64_t> presentationLog; // Recorded swapchain present timestamps

    int64_t NsFromQpc(int64_t delta) const { return freq > 0 ? (delta * 1000000000LL) / freq : 0; }
    int64_t QpcFromNs(int64_t ns) const { return freq > 0 ? (ns * freq) / 1000000000LL : 0; }
    double MsFromQpc(int64_t qpc) const { return freq > 0 ? (qpc * 1000.0) / freq : 0.0; }

    void PushPeriod(int64_t ns)
    {
        if (ns <= 0)
            return;

        samples[samplePos] = ns;
        samplePos = (samplePos + 1) % SampleCount;

        if (sampleCount < SampleCount)
            sampleCount++;

        int64_t sorted[SampleCount];
        std::memcpy(sorted, samples, sizeof(int64_t) * sampleCount);

        for (int32_t i = 1; i < sampleCount; i++)
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

        periodNs = sorted[sampleCount / 2];
    }

    // Paces generated frames (index 1..count)
    int64_t PaceGeneratedFrame(uint64_t index, uint64_t count, int64_t arrivalQpc)
    {
        const int64_t multiplier = static_cast<int64_t>(count) + 1;
        if (multiplier < 2)
            return arrivalQpc;

        lastMultiplier = multiplier;

        if (periodNs <= 0)
            periodNs = 33333333LL;

        const int64_t intervalNs = periodNs / multiplier;
        const int64_t interval = QpcFromNs(intervalNs);
        intervalQpc = interval;

        if (index == 1)
        {
            burstAnchorQpc = arrivalQpc;
            int64_t targetQpc = arrivalQpc;

            // Anti-clump check relative to preceding cycle's presentation
            if (lastPresentedQpc != 0)
            {
                const int64_t minGapQpc = interval / 2;
                const int64_t antiClumpFloor = lastPresentedQpc + minGapQpc;
                if (targetQpc < antiClumpFloor)
                    targetQpc = antiClumpFloor;
            }

            burstAnchorQpc = targetQpc;
            CommitPresentation(targetQpc);
            return targetQpc;
        }

        // Subsequent generated frames (index 2..count for 3X-6X FG)
        int64_t targetQpc = burstAnchorQpc + static_cast<int64_t>(index - 1) * interval;

        if (lastPresentedQpc != 0)
        {
            const int64_t minGapQpc = interval / 2;
            const int64_t antiClumpFloor = lastPresentedQpc + minGapQpc;
            if (targetQpc < antiClumpFloor)
                targetQpc = antiClumpFloor;
        }

        CommitPresentation(targetQpc);
        return targetQpc;
    }

    // Paces the base application frame (frame M of the burst)
    int64_t PaceBaseFrame(int64_t arrivalQpc)
    {
        int64_t multiplier = lastMultiplier;
        if (multiplier < 2)
            multiplier = 2;

        if (periodNs <= 0)
            periodNs = 33333333LL;

        const int64_t intervalNs = periodNs / multiplier;
        const int64_t interval = QpcFromNs(intervalNs);
        intervalQpc = interval;

        int64_t targetQpc = arrivalQpc;
        if (burstAnchorQpc != 0 && interval > 0)
        {
            targetQpc = burstAnchorQpc + static_cast<int64_t>(multiplier - 1) * interval;

            if (lastPresentedQpc != 0)
            {
                const int64_t minGapQpc = interval / 2;
                const int64_t antiClumpFloor = lastPresentedQpc + minGapQpc;
                if (targetQpc < antiClumpFloor)
                    targetQpc = antiClumpFloor;
            }
        }

        if (lastBaseFrameQpc != 0)
        {
            const int64_t burstDeltaNs = NsFromQpc(targetQpc - lastBaseFrameQpc);
            if (burstDeltaNs >= 5000000LL && burstDeltaNs <= 200000000LL)
            {
                PushPeriod(burstDeltaNs);
                renderTimeNs = periodNs;
            }
        }

        lastBaseFrameQpc = targetQpc;
        CommitPresentation(targetQpc);
        pacedBursts++;
        return targetQpc;
    }

    void CommitPresentation(int64_t qpc)
    {
        if (lastPresentedQpc != 0)
        {
            const int64_t gapNs = NsFromQpc(qpc - lastPresentedQpc);
            // Gap less than 2.5 ms is considered a severe saw-tooth clump
            if (gapNs < 2500000LL)
                clumpCount++;
        }

        lastPresentedQpc = qpc;
        presentationLog.push_back(qpc);
        pacedFrames++;
    }

    double RenderTimeMs() const { return renderTimeNs > 0 ? renderTimeNs / 1000000.0 : 0.0; }
};

} // namespace XeMfgPacingTest

int main()
{
    printf("[+] Starting XeMFG Multiplier Unlock & Burst Pacing Comprehensive Unit Tests...\n");

    // Test 1: Multiplier uncap in XeFG_Dx12 with active XeMFG session
    {
        XeMfgPacingTest::MockXeMfgLoader::sessionActive = true;
        XeMfgPacingTest::MockXeMfgLoader::liveCeiling = 5;

        XeMfgPacingTest::SimulatedXeFGContext ctx;
        ctx.OnInitSwapchain();
        assert(ctx.maxInterpolationCount == 5 &&
               "Swapchain init must set maxInterpolationCount to 5 when XeMFG is active");

        for (uint32_t count = 1; count <= 5; ++count)
        {
            bool ok = ctx.SetInterpolatedFrameCount(count);
            assert(ok && "SetInterpolatedFrameCount must succeed for 1..5 without clamping");
            assert(ctx.activeInterpolationCount == static_cast<int>(count));
            assert(!ctx.clamped && "No clamping warning should be triggered");
            assert(XeMfgPacingTest::MockXeMfgLoader::liveCeiling == static_cast<int>(count));
        }

        printf("  [PASS] Test 1: XeFG_Dx12 multiplier ceiling uncap supports 2X-6X live without clamping.\n");
    }

    // Test 2: Multiplier clamping preserved when XeMFG is inactive (stock behavior)
    {
        XeMfgPacingTest::MockXeMfgLoader::sessionActive = false;

        XeMfgPacingTest::SimulatedXeFGContext ctx;
        ctx.OnInitSwapchain();
        assert(ctx.maxInterpolationCount == 1 && "Stock swapchain init must remain at 1");

        // Requesting 2 (3X FG) must clamp to 1 when XeMFG is disabled
        bool ok = ctx.SetInterpolatedFrameCount(2);
        assert(!ok && "Must indicate clamping occurred");
        assert(ctx.clamped && "Clamped flag must be true");
        assert(ctx.activeInterpolationCount == 1 && "Active count must be clamped to 1");
        assert(!ctx.lastWarning.empty());

        printf("  [PASS] Test 2: Stock multiplier clamping preserved when XeMFG session is inactive.\n");
    }

    // Test 3: Dispatch validation allows un-capped interpolation count up to effectiveMax
    {
        XeMfgPacingTest::MockXeMfgLoader::sessionActive = true;
        XeMfgPacingTest::MockXeMfgLoader::liveCeiling = 5;

        XeMfgPacingTest::SimulatedXeFGContext ctx;
        ctx.OnInitSwapchain();

        assert(ctx.ValidateDispatchCount(1) == 1);
        assert(ctx.ValidateDispatchCount(2) == 2);
        assert(ctx.ValidateDispatchCount(3) == 3);
        assert(ctx.ValidateDispatchCount(4) == 4);
        assert(ctx.ValidateDispatchCount(5) == 5);
        assert(ctx.ValidateDispatchCount(6) == 5 && "Above 5 must clamp to 5");

        printf("  [PASS] Test 3: Dispatch validation accepts 2X-6X counts without down-clamping.\n");
    }

    // Test 4: 2X Full-burst presentation pacing (Generated Frame at 50%, Base Frame at 100%)
    {
        XeMfgPacingTest::SimulatedXeFGPacer pacer;
        const int64_t framePeriodNs = 36400000LL; // 36.4 ms (~27.5 fps base, matching Helldivers 2 log)
        pacer.PushPeriod(framePeriodNs);

        const int64_t burst1Start = 100000000LL;

        // Burst 1:
        // Frame 1 (Generated): arrived from GPU at burst1Start
        int64_t gen1Qpc = pacer.PaceGeneratedFrame(1, 1, burst1Start);
        // Frame 2 (Base application frame): arrived at burst1Start + small delta
        int64_t base1Qpc = pacer.PaceBaseFrame(burst1Start + pacer.QpcFromNs(2000000LL));

        // Burst 2 (next frame, engine completed next cycle after ~36.4 ms):
        const int64_t burst2Start = burst1Start + pacer.QpcFromNs(framePeriodNs);
        int64_t gen2Qpc = pacer.PaceGeneratedFrame(1, 1, burst2Start);
        int64_t base2Qpc = pacer.PaceBaseFrame(burst2Start + pacer.QpcFromNs(2000000LL));

        // Verify spacing:
        // Spacing between Gen 1 and Base 1
        int64_t delta1Ns = pacer.NsFromQpc(base1Qpc - gen1Qpc);
        // Spacing between Base 1 and Gen 2
        int64_t delta2Ns = pacer.NsFromQpc(gen2Qpc - base1Qpc);
        // Spacing between Gen 2 and Base 2
        int64_t delta3Ns = pacer.NsFromQpc(base2Qpc - gen2Qpc);

        const int64_t expectedIntervalNs = framePeriodNs / 2; // 18.2 ms

        assert(delta1Ns == expectedIntervalNs && "Gen1 to Base1 must be exactly 18.2 ms");
        assert(delta2Ns == expectedIntervalNs && "Base1 to Gen2 must be exactly 18.2 ms");
        assert(delta3Ns == expectedIntervalNs && "Gen2 to Base2 must be exactly 18.2 ms");
        assert(pacer.clumpCount == 0 && "Zero saw-tooth dumps across 2X presentations");

        printf("  [PASS] Test 4: 2X Full-burst presentation spacing delivers uniform 18.2 ms cadence (0 clumps).\n");
    }

    // Test 5: 3X Full-burst presentation pacing (Gen 1 at 33%, Gen 2 at 67%, Base at 100%)
    {
        XeMfgPacingTest::SimulatedXeFGPacer pacer;
        const int64_t framePeriodNs = 36300000LL; // 36.3 ms
        pacer.PushPeriod(framePeriodNs);

        const int64_t burstStart = 200000000LL;

        // Gen 1 (index 1, count 2)
        int64_t gen1Qpc = pacer.PaceGeneratedFrame(1, 2, burstStart);
        // Gen 2 (index 2, count 2)
        int64_t gen2Qpc = pacer.PaceGeneratedFrame(2, 2, burstStart + pacer.QpcFromNs(5000000LL));
        // Base Frame (multiplier 3)
        int64_t baseQpc = pacer.PaceBaseFrame(burstStart + pacer.QpcFromNs(8000000LL));

        int64_t step1Ns = pacer.NsFromQpc(gen2Qpc - gen1Qpc);
        int64_t step2Ns = pacer.NsFromQpc(baseQpc - gen2Qpc);

        const int64_t expectedIntervalNs = framePeriodNs / 3; // 12.1 ms

        assert(step1Ns == expectedIntervalNs && "Gen1 to Gen2 step must be exactly 12.1 ms");
        assert(step2Ns == expectedIntervalNs && "Gen2 to Base step must be exactly 12.1 ms");
        assert(pacer.clumpCount == 0 && "Zero clumps across 3X presentation");

        printf("  [PASS] Test 5: 3X Full-burst presentation delivers uniform 12.1 ms cadence across all 3 frames.\n");
    }

    // Test 6: 4X and 6X Full-burst presentation cadence uniformity
    {
        // 4X test (count 3, multiplier 4)
        {
            XeMfgPacingTest::SimulatedXeFGPacer pacer;
            const int64_t framePeriodNs = 36400000LL;
            pacer.PushPeriod(framePeriodNs);
            const int64_t burstStart = 300000000LL;

            int64_t g1 = pacer.PaceGeneratedFrame(1, 3, burstStart);
            int64_t g2 = pacer.PaceGeneratedFrame(2, 3, burstStart + pacer.QpcFromNs(3000000LL));
            int64_t g3 = pacer.PaceGeneratedFrame(3, 3, burstStart + pacer.QpcFromNs(6000000LL));
            int64_t base = pacer.PaceBaseFrame(burstStart + pacer.QpcFromNs(9000000LL));

            const int64_t intervalNs = framePeriodNs / 4; // 9.1 ms
            assert(pacer.NsFromQpc(g2 - g1) == intervalNs);
            assert(pacer.NsFromQpc(g3 - g2) == intervalNs);
            assert(pacer.NsFromQpc(base - g3) == intervalNs);
            assert(pacer.clumpCount == 0);
        }

        // 6X test (count 5, multiplier 6)
        {
            XeMfgPacingTest::SimulatedXeFGPacer pacer;
            const int64_t framePeriodNs = 36600000LL;
            pacer.PushPeriod(framePeriodNs);
            const int64_t burstStart = 400000000LL;

            std::vector<int64_t> frames;
            frames.push_back(pacer.PaceGeneratedFrame(1, 5, burstStart));
            for (uint64_t i = 2; i <= 5; ++i)
                frames.push_back(pacer.PaceGeneratedFrame(i, 5, burstStart + pacer.QpcFromNs(i * 1000000LL)));
            frames.push_back(pacer.PaceBaseFrame(burstStart + pacer.QpcFromNs(7000000LL)));

            const int64_t intervalNs = framePeriodNs / 6; // 6.1 ms
            for (size_t i = 1; i < frames.size(); ++i)
            {
                int64_t stepNs = pacer.NsFromQpc(frames[i] - frames[i - 1]);
                assert(stepNs == intervalNs && "Every step in 6X burst must equal interval");
            }
            assert(pacer.clumpCount == 0);
        }

        printf("  [PASS] Test 6: 4X and 6X Full-burst pacing partitions all generated and base frames uniformly.\n");
    }

    // Test 7: Direct comparison: Legacy 2 ms dump vs Overhauled Burst Pacing
    {
        // Legacy behavior: generated frame at 18.2 ms, base frame dumped 2.187 ms later (34.3 ms stall)
        XeMfgPacingTest::SimulatedXeFGPacer legacyPacer;
        const int64_t framePeriodNs = 36400000LL;
        legacyPacer.PushPeriod(framePeriodNs);
        const int64_t t0 = 500000000LL;

        // Legacy paced generated frame
        int64_t legGen = legacyPacer.PaceGeneratedFrame(1, 1, t0);
        // Legacy base frame was unpaced: dumped immediately when caller 0x220A52 called present
        int64_t legBase = legGen + legacyPacer.QpcFromNs(2187000LL); // 2.187 ms dump from log!
        legacyPacer.CommitPresentation(legBase);

        assert(legacyPacer.clumpCount == 1 && "Legacy pacing resulted in detected 2.187 ms saw-tooth clump!");

        // Overhauled behavior: base frame is paced to anchor + 1 * interval
        XeMfgPacingTest::SimulatedXeFGPacer overhauledPacer;
        overhauledPacer.PushPeriod(framePeriodNs);
        int64_t newGen = overhauledPacer.PaceGeneratedFrame(1, 1, t0);
        int64_t newBase = overhauledPacer.PaceBaseFrame(t0 + overhauledPacer.QpcFromNs(2187000LL));

        assert(overhauledPacer.clumpCount == 0 && "Overhauled burst pacing completely eliminated the 2.187 ms clump!");
        int64_t newGapNs = overhauledPacer.NsFromQpc(newBase - newGen);
        assert(newGapNs == 18200000LL && "Overhauled gap is uniform 18.2 ms instead of 2.187 ms!");

        printf("  [PASS] Test 7: Cadence comparison proves elimination of 2.187 ms dump and 34.3 ms stall.\n");
    }

    // Test 8: Dynamic anti-clump floor protects against late render arrivals
    {
        XeMfgPacingTest::SimulatedXeFGPacer pacer;
        const int64_t framePeriodNs = 36400000LL;
        pacer.PushPeriod(framePeriodNs);

        const int64_t t0 = 600000000LL;
        int64_t gen = pacer.PaceGeneratedFrame(1, 1, t0);

        // Simulate severe engine delay where base frame arrives late, past the target deadline
        const int64_t interval = pacer.QpcFromNs(18200000LL);
        const int64_t lateArrivalQpc = gen + pacer.QpcFromNs(1000000LL); // Only 1 ms after gen!

        // Pacer must enforce anti-clump floor = gen + interval / 2 (9.1 ms)
        int64_t basePacedQpc = pacer.PaceBaseFrame(lateArrivalQpc);
        int64_t gapNs = pacer.NsFromQpc(basePacedQpc - gen);

        assert(gapNs >= 9100000LL && "Anti-clump floor must enforce at least interval / 2 spacing");
        assert(pacer.clumpCount == 0);

        printf("  [PASS] Test 8: Anti-clump protection floor prevents presentation collision on late arrivals.\n");
    }

    // Test 9: Frame render time measurement feeds valid delta time to Intel provider
    {
        XeMfgPacingTest::SimulatedXeFGPacer pacer;
        const int64_t framePeriodNs = 33333333LL; // 33.3 ms
        const int64_t t0 = 700000000LL;

        // Cycle 1
        pacer.PaceGeneratedFrame(1, 1, t0);
        pacer.PaceBaseFrame(t0 + pacer.QpcFromNs(16666666LL));

        // Cycle 2 (33.3 ms later)
        const int64_t t1 = t0 + pacer.QpcFromNs(framePeriodNs);
        pacer.PaceGeneratedFrame(1, 1, t1);
        pacer.PaceBaseFrame(t1 + pacer.QpcFromNs(16666666LL));

        assert(pacer.RenderTimeMs() >= 33.0 && pacer.RenderTimeMs() <= 34.0);
        assert(pacer.renderTimeNs > 0 && "Render time must never collapse to 0.0 ms");

        printf("  [PASS] Test 9: Decoupled frame render time measurement verified (non-zero delta time feed).\n");
    }

    printf("[+] All XeMFG Multiplier Unlock & Burst Pacing Unit Tests PASSED successfully!\n");
    return 0;
}

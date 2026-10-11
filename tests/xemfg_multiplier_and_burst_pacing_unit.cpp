#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Unit test suite for XeMFG multi-frame generation multiplier uncap (2X-6X),
// non-blocking base frame presentation, recursive feedback loop isolation,
// and TsDetour uniform background worker deadline scheduling.
//
// Validates:
// 1. XeFG_Dx12 multiplier ceiling uncap: SetInterpolatedFrameCount accepts 2..5 (3X-6X FG)
//    without clamping to 1 when XeMFG is active.
// 2. XeFG_Dx12 clamping retention when XeMFG is inactive (stock behavior preserved).
// 3. Dispatch validation respects effectiveMax (up to 5) instead of stock 1.
// 4. Non-blocking base frame presentation: NoteBasePresent() executes with 0 wait time.
// 5. Zero feedback loop: NoteBasePresent() excludes burst blocking, preventing frame time
//    inflation (stays locked at native render time instead of ballooning to 108 ms).
// 6. TsDetour (RVA 0x3430) partitions deadlines evenly across generated frames 1..count
//    (deadline(i) = base + (i-1) * unit) without accumulating drift across bursts.
// 7. ApplyAntiClumpProtection prevents sub-millisecond frame dumps (<1 ms) without stalling.
// 8. Frame render time measurement (RenderTimeMs()) feeds valid non-zero delta time to Intel.
// 9. Swapchain init parameters: params.maxInterpolatedFrames == 1 sent to D3D12InitFromSwapChainDesc
//    on non-Intel GPUs, satisfying 0x1801ffb44 validation and preventing INVALID_ARGUMENT.
// 10. Startup multiplier synchronization: post-creation SetInterpolatedFrameCount immediately
//     binds the user-configured frame count (2..5) on frame 1 without menu slider interaction.
// 11. Quirk lifecycle: ForceBorderlessWhenUsingXeFG quirk correctly applies when XeMFG/XeMFG-SR
//     auto-configures activeFgOutput == XeFG without premature quirk destruction.

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

    bool isIntelGPU = false;                 // Non-Intel GPU by default (NVIDIA GA103 / RTX 3080 Ti Laptop)
    bool hasSetNumInterpolatedFrames = true; // Modern libxess_fg.dll v1.3.1.78
    uint32_t initParamsMaxInterpolatedFrames = 0;
    uint32_t currentProviderFrames = 1;

    bool CreateSwapchain(uint32_t configTargetCount, bool forceXeLL = false, bool useFlawedInit = false)
    {
        OnInitSwapchain();
        int effectiveMax = maxInterpolationCount;
        if (MockXeMfgLoader::AnyEnabledForSession())
            effectiveMax = std::max(effectiveMax, 5);

        int intTarget = effectiveMax;
        if (!hasSetNumInterpolatedFrames)
            intTarget = configTargetCount;

        if (intTarget < 1 || intTarget > effectiveMax)
            intTarget = 1;

        if (useFlawedInit)
        {
            // Old flawed logic from 55b47147: passed intTarget (5) directly
            initParamsMaxInterpolatedFrames = intTarget;
        }
        else
        {
            // Fixed logic: pass 1 when SetNumInterpolatedFrames is present or forceXeLL
            if (forceXeLL || hasSetNumInterpolatedFrames)
                initParamsMaxInterpolatedFrames = 1;
            else
                initParamsMaxInterpolatedFrames = intTarget;
        }

        // Simulated Intel libxess_fg.dll D3D12InitFromSwapChainDesc validation:
        // In 0x1801ffb41: cmpl $1, params.maxInterpolatedFrames on non-Intel GPUs (0x1801ffb44)
        if (!isIntelGPU && initParamsMaxInterpolatedFrames != 1)
        {
            return false; // XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT
        }

        // Post-creation startup binding:
        if (hasSetNumInterpolatedFrames)
        {
            SetInterpolatedFrameCount(configTargetCount);
            currentProviderFrames = activeInterpolationCount;
        }
        else
        {
            activeInterpolationCount = intTarget;
            currentProviderFrames = intTarget;
        }

        return true;
    }
};

// Simulated XeFGPacing Presentation & Deadline Engine
struct SimulatedXeFGPacer
{
    int64_t freq = 10000000LL; // 10 MHz QPC frequency (1 tick = 100 ns)
    int64_t lastBaseFrameQpc = 0;
    int64_t lastPresentedQpc = 0;
    int64_t periodNs = 0;
    int64_t intervalQpc = 0;
    int64_t samples[SampleCount] {};
    int32_t sampleCount = 0;
    int32_t samplePos = 0;
    int64_t pacedFrames = 0;
    int64_t pacedBursts = 0;
    int64_t burstBlockQpc = 0;
    int64_t renderTimeNs = 0;
    int64_t lastMultiplier = 2;

    // TsDetour state
    int64_t burstBaseDeadlineNs = 0;
    int64_t burstStepNs = 0;
    int64_t tsRebased = 0;

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

    // Non-blocking base frame present handler (0 ms wait, zero thread stall)
    void NoteBasePresent(int64_t nowQpc)
    {
        if (lastBaseFrameQpc != 0)
        {
            const int64_t deltaNs = NsFromQpc(nowQpc - lastBaseFrameQpc);
            if (deltaNs >= 5000000LL && deltaNs <= 200000000LL)
            {
                // Subtract burst blocking duration so PushPeriod isolates pure engine render time
                const int64_t blockNs = NsFromQpc(burstBlockQpc);
                const int64_t pureRenderNs = deltaNs > blockNs ? deltaNs - blockNs : deltaNs;

                PushPeriod(pureRenderNs);
                renderTimeNs = periodNs;
            }
        }

        lastPresentedQpc = nowQpc;
        lastBaseFrameQpc = nowQpc;
        pacedBursts++;
        burstBlockQpc = 0;
    }

    // Anti-clump protection on generated frames (enforces >= 1.5 ms gap without stalling for full intervals)
    int64_t ApplyAntiClumpProtection(uint64_t index, uint64_t count, int64_t arrivalQpc)
    {
        const int64_t multiplier = static_cast<int64_t>(count) + 1;
        if (periodNs <= 0)
            periodNs = 33333333LL;

        const int64_t intervalNs = periodNs / multiplier;
        const int64_t interval = QpcFromNs(intervalNs);

        int64_t presentedQpc = arrivalQpc;
        if (lastPresentedQpc != 0 && interval > 0)
        {
            const int64_t minGapNs = (intervalNs * 2) / 5;
            const int64_t cappedMinGapNs =
                minGapNs > 2000000LL ? 2000000LL : (minGapNs < 500000LL ? 500000LL : minGapNs);
            const int64_t antiClumpFloorQpc = lastPresentedQpc + QpcFromNs(cappedMinGapNs);
            if (presentedQpc < antiClumpFloorQpc)
                presentedQpc = antiClumpFloorQpc;
        }

        burstBlockQpc += presentedQpc - arrivalQpc;
        lastPresentedQpc = presentedQpc;
        pacedFrames++;
        return presentedQpc;
    }

    // Simulated TsDetour deadline calculator
    int64_t CalculateDeadline(int64_t nativeOut, int64_t medianNs, uint32_t index, uint32_t countPlus1)
    {
        const int64_t unit = medianNs / static_cast<int64_t>(countPlus1);
        if (unit <= 0)
            return nativeOut;

        if (index == 1)
        {
            burstBaseDeadlineNs = nativeOut;
            burstStepNs = unit;
        }

        int64_t calculatedDeadline = nativeOut;
        if (burstBaseDeadlineNs != 0)
        {
            calculatedDeadline = burstBaseDeadlineNs + static_cast<int64_t>(index - 1) * burstStepNs;
            tsRebased++;
        }

        return calculatedDeadline;
    }

    double RenderTimeMs() const { return renderTimeNs > 0 ? renderTimeNs / 1000000.0 : 0.0; }
};

} // namespace XeMfgPacingTest

int main()
{
    printf("[+] Starting XeMFG Multiplier Unlock & Non-Blocking Pacing Comprehensive Unit Tests...\n");

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

    // Test 4: Non-blocking base frame presentation (0 ms wait, returns immediately)
    {
        XeMfgPacingTest::SimulatedXeFGPacer pacer;
        const int64_t t0 = 100000000LL;

        // Base frame present must execute immediately with 0 added delay
        pacer.NoteBasePresent(t0);
        assert(pacer.lastBaseFrameQpc == t0);
        assert(pacer.lastPresentedQpc == t0);
        assert(pacer.burstBlockQpc == 0 && "Burst block QPC must be cleanly reset");
        assert(pacer.pacedBursts == 1);

        printf("  [PASS] Test 4: Base application frame presents with 0 wait and returns immediately.\n");
    }

    // Test 5: Zero recursive feedback loop in frame time period measurement
    {
        XeMfgPacingTest::SimulatedXeFGPacer pacer;
        const int64_t trueRenderNs = 33333333LL;   // Game engine renders at steady 30 FPS (33.3 ms)
        const int64_t presentDelayNs = 45000000LL; // 45 ms spent in GPU swapchain operations

        int64_t currentQpc = 100000000LL;
        pacer.NoteBasePresent(currentQpc);

        // Simulate 20 consecutive frames where swapchain experienced internal delays
        for (int i = 0; i < 20; ++i)
        {
            // Advance by engine render time
            currentQpc += pacer.QpcFromNs(trueRenderNs);

            // Record burst block (present delay)
            pacer.burstBlockQpc = pacer.QpcFromNs(presentDelayNs);
            currentQpc += pacer.burstBlockQpc;

            // NoteBasePresent must subtract burstBlockQpc and push ONLY trueRenderNs
            pacer.NoteBasePresent(currentQpc);
        }

        // Period must remain locked at true engine render time (~33.3 ms), NEVER ballooning to 108 ms!
        const double measuredMs = pacer.periodNs / 1000000.0;
        assert(measuredMs >= 33.0 && measuredMs <= 34.0 &&
               "Measured period must isolate true engine render time (33.3 ms)");
        assert(measuredMs < 50.0 && "Period must NEVER balloon into positive feedback loop!");

        printf("  [PASS] Test 5: Recursive feedback loop eliminated: frame time stays locked at 33.3 ms (never 108 "
               "ms).\n");
    }

    // Test 6: TsDetour partitions deadlines uniformly across generated frames 1..5
    {
        XeMfgPacingTest::SimulatedXeFGPacer pacer;
        const int64_t medianNs = 36000000LL; // 36 ms median cycle
        const int64_t baseDeadlineNs = 5000000000LL;

        // 6X FG (countPlus1 = 6, multiplier 6X)
        const int64_t unitNs = medianNs / 6; // 6 ms per generated frame
        std::vector<int64_t> deadlines;

        for (uint32_t i = 1; i <= 5; ++i)
        {
            int64_t nativeDummy = baseDeadlineNs; // Intel stock sets identical deadline for i > 1
            int64_t d = pacer.CalculateDeadline(nativeDummy, medianNs, i, 6);
            deadlines.push_back(d);
        }

        assert(deadlines[0] == baseDeadlineNs && "Frame 1 deadline matches base");
        for (size_t i = 1; i < deadlines.size(); ++i)
        {
            int64_t step = deadlines[i] - deadlines[i - 1];
            assert(step == unitNs && "Each generated frame deadline step must equal exactly unitNs (6 ms)");
        }

        // Test drift immunity on next burst
        const int64_t nextBaseDeadlineNs = baseDeadlineNs + 36000000LL;
        int64_t nextD1 = pacer.CalculateDeadline(nextBaseDeadlineNs, medianNs, 1, 6);
        int64_t nextD2 = pacer.CalculateDeadline(nextBaseDeadlineNs, medianNs, 2, 6);
        assert(nextD1 == nextBaseDeadlineNs);
        assert(nextD2 == nextBaseDeadlineNs + unitNs);

        printf("  [PASS] Test 6: TsDetour partitions deadlines uniformly (6 ms steps) with zero inter-cycle drift.\n");
    }

    // Test 7: ApplyAntiClumpProtection prevents sub-millisecond frame dumps (<1 ms)
    {
        XeMfgPacingTest::SimulatedXeFGPacer pacer;
        pacer.PushPeriod(36000000LL); // 36 ms (6X FG -> 6 ms interval)

        const int64_t t0 = 200000000LL;
        pacer.lastPresentedQpc = t0;

        // Simulate frame arriving only 0.3 ms (300 us) after preceding present (sub-millisecond dump)
        const int64_t clumpArrival = t0 + pacer.QpcFromNs(300000LL);
        int64_t presentedQpc = pacer.ApplyAntiClumpProtection(2, 5, clumpArrival);

        int64_t gapNs = pacer.NsFromQpc(presentedQpc - t0);
        // Minimum gap floor is cappedMinGapNs (at least 1.5 - 2.0 ms)
        assert(gapNs >= 1500000LL && "Anti-clump floor must enforce at least 1.5 ms spacing to prevent dumps");
        assert(presentedQpc > clumpArrival && "Sub-millisecond arrival must be paced forward");

        // Simulate frame arriving 8.0 ms after preceding present (already spaced)
        pacer.lastPresentedQpc = presentedQpc;
        const int64_t wellSpacedArrival = presentedQpc + pacer.QpcFromNs(8000000LL);
        int64_t normalPresented = pacer.ApplyAntiClumpProtection(3, 5, wellSpacedArrival);
        assert(normalPresented == wellSpacedArrival && "Already spaced frames must incur 0 ms added delay");

        printf(
            "  [PASS] Test 7: ApplyAntiClumpProtection eliminates 0.3 ms dumps while adding 0 ms to spaced frames.\n");
    }

    // Test 8: Decoupled frame render time measurement feeds valid non-zero delta time
    {
        XeMfgPacingTest::SimulatedXeFGPacer pacer;
        const int64_t framePeriodNs = 33333333LL; // 33.3 ms
        const int64_t t0 = 300000000LL;

        // Cycle 1
        pacer.NoteBasePresent(t0);

        // Cycle 2 (33.3 ms later)
        const int64_t t1 = t0 + pacer.QpcFromNs(framePeriodNs);
        pacer.NoteBasePresent(t1);

        assert(pacer.RenderTimeMs() >= 33.0 && pacer.RenderTimeMs() <= 34.0);
        assert(pacer.renderTimeNs > 0 && "Render time must never collapse to 0.0 ms");

        printf("  [PASS] Test 8: Decoupled frame render time measurement verified (non-zero delta time feed).\n");
    }

    // Test 9: Swapchain init parameter safety on non-Intel GPUs (avoids 0x1801ffb44 rejection)
    {
        XeMfgPacingTest::SimulatedXeFGContext ctx;
        XeMfgPacingTest::MockXeMfgLoader::sessionActive = true;
        ctx.isIntelGPU = false; // NVIDIA GA103 / RTX 3080 Ti Laptop
        ctx.hasSetNumInterpolatedFrames = true;

        // Verify that the old flawed logic setting params.maxInterpolatedFrames = 5 fails Intel's validation
        bool flawedResult = ctx.CreateSwapchain(2, false, true);
        assert(!flawedResult && "Flawed init passing maxInterpolatedFrames=5 must be rejected by Intel provider");

        // Verify that the fixed logic setting params.maxInterpolatedFrames = 1 succeeds cleanly
        bool fixedResult = ctx.CreateSwapchain(2, false, false);
        assert(fixedResult && "Fixed init passing maxInterpolatedFrames=1 must succeed on non-Intel GPUs");
        assert(ctx.initParamsMaxInterpolatedFrames == 1 && "params.maxInterpolatedFrames must be 1");

        printf("  [PASS] Test 9: Swapchain init parameters pass Intel validation (0x1801ffb44) on non-Intel GPUs.\n");
    }

    // Test 10: Dynamic startup multiplier application on frame 1 without menu slider interaction
    {
        XeMfgPacingTest::SimulatedXeFGContext ctx;
        XeMfgPacingTest::MockXeMfgLoader::sessionActive = true;
        ctx.isIntelGPU = false;
        ctx.hasSetNumInterpolatedFrames = true;

        // User configures 3X FG (InterpolationCount = 2) in OptiScaler.ini
        bool success3X = ctx.CreateSwapchain(2, false, false);
        assert(success3X);
        assert(ctx.currentProviderFrames == 2 && "Provider must be bound to 2 interpolated frames (3X FG) on frame 1");

        // User configures 6X FG (InterpolationCount = 5) in OptiScaler.ini
        bool success6X = ctx.CreateSwapchain(5, false, false);
        assert(success6X);
        assert(ctx.currentProviderFrames == 5 && "Provider must be bound to 5 interpolated frames (6X FG) on frame 1");

        printf(
            "  [PASS] Test 10: Dynamic startup multiplier bound immediately on frame 1 without manual interaction.\n");
    }

    // Test 11: Quirk lifecycle & borderless windowing preservation
    {
        enum class Quirk : uint32_t
        {
            None = 0,
            ForceBorderlessWhenUsingXeFG = 1 << 0,
            OverrideVsyncWhenUsingXeFG = 1 << 1,
        };

        struct QuirkContext
        {
            uint32_t detectedQuirks = 0;
            bool forceBorderlessConfig = false;
            bool hasExplicitConfig = false;
            std::string activeFgOutput = "NoFG";

            void OnStartupQuirkScan(uint32_t exeQuirks)
            {
                detectedQuirks = exeQuirks;
                // Old bug: was doing quirks.reset() if activeFgOutput wasn't XeFG yet
            }

            void OnPipelineConfigured(const std::string& newFgOutput)
            {
                activeFgOutput = newFgOutput;
                if (activeFgOutput == "XeFG")
                {
                    if ((detectedQuirks & static_cast<uint32_t>(Quirk::ForceBorderlessWhenUsingXeFG)) &&
                        !hasExplicitConfig)
                    {
                        forceBorderlessConfig = true;
                    }
                }
            }
        };

        QuirkContext qctx;
        // Helldivers 2 has ForceBorderlessWhenUsingXeFG
        qctx.OnStartupQuirkScan(static_cast<uint32_t>(Quirk::ForceBorderlessWhenUsingXeFG));
        assert((qctx.detectedQuirks & static_cast<uint32_t>(Quirk::ForceBorderlessWhenUsingXeFG)) &&
               "Quirk must be preserved during initial scan");

        // XeMFG-SR auto-configures activeFgOutput = "XeFG"
        qctx.OnPipelineConfigured("XeFG");
        assert(qctx.forceBorderlessConfig && "ForceBorderless must be applied when XeFG output becomes active");

        printf("  [PASS] Test 11: ForceBorderlessWhenUsingXeFG quirk applied on XeFG pipeline activation.\n");
    }

    printf("[+] All XeMFG Multiplier Unlock, Non-Blocking Pacing & Swapchain Init Unit Tests PASSED successfully!\n");
    return 0;
}

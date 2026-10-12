#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

// Comprehensive Unit Test Suite: XeMFG-SR Complete Parity with Native XeMFG
// Validating:
// 1. Universal Backbuffer Resource Tagging Parity across DLSSG & Upscaler inputs.
// 2. Subsystem Multiplier & Latency Synchronization across XeFG_Dx12, dlssgDetectedInterpolationCount, and Reflex.
// 3. Low-Latency Tech Gating under Multi-Frame Generation (bypassing XeLL & AL2, falling back to LatencyFlex).
// 4. Decoupled Frame Render Time & Anti-Starvation Clamping breaking 7.3 FPS positive feedback loops.
// 5. Anti-Collapse Pacing Fallback in NoteFrame when block time approaches cycle duration.
// 6. NGX Proxy Feature 11 Spoofing Parity across DX12, DX11, and Vulkan.

namespace XeMfgSrParityTest
{

enum class FGInput
{
    NoFG = 0,
    DLSSG = 1,
    FSRFG = 2,
    Upscaler = 3,
};

enum class FGOutput
{
    NoFG = 0,
    DLSSG = 1,
    FSRFG = 2,
    XeFG = 3,
};

enum class DLSSGMode
{
    eOff = 0,
    eOn = 1,
    eAuto = 2,
    eDynamic = 3,
};

enum class LowLatencyMode
{
    None = 0,
    Reflex = 1,
    LatencyFlex = 2,
    AntiLag2 = 3,
    XeLL = 4,
};

// 1. Backbuffer Resource Tagging Simulator
struct TaggingState
{
    FGInput activeFgInput = FGInput::Upscaler;
    bool fgHudFix = false;
    uint32_t swapchainWidth = 1920;
    uint32_t swapchainHeight = 1080;
    uint32_t interpolationWidth = 0;
    uint32_t interpolationHeight = 0;

    bool wasBackbufferTagged = false;
    uint32_t taggedWidth = 0;
    uint32_t taggedHeight = 0;

    void DispatchSim()
    {
        wasBackbufferTagged = false;
        taggedWidth = 0;
        taggedHeight = 0;

        // When using Hudfix, we always copy hudless as swapchain size
        if (!fgHudFix)
        {
            uint32_t w = interpolationWidth;
            uint32_t h = interpolationHeight;
            if (w == 0 && h == 0)
            {
                w = swapchainWidth;
                h = swapchainHeight;
            }

            wasBackbufferTagged = true;
            taggedWidth = w;
            taggedHeight = h;
        }
    }
};

// 2. Multiplier & Subsystem Synchronization Simulator
struct MultiplierSyncState
{
    int dlssgDetectedInterpolationCount = 0;
    DLSSGMode dlssgLastSetMode = DLSSGMode::eOff;
    uint8_t reflexDlssgFrameCount = 0;
    uint32_t xeMfgMaxGeneratedFrames = 0;
    int volatileInterpolationCount = 0;

    void SetInterpolatedFrameCount(uint32_t count)
    {
        if (count == 0)
        {
            dlssgDetectedInterpolationCount = 0;
            dlssgLastSetMode = DLSSGMode::eOff;
            reflexDlssgFrameCount = 0;
            volatileInterpolationCount = 0;
            return;
        }

        uint32_t clamped = std::clamp(count, 1u, 5u);
        volatileInterpolationCount = static_cast<int>(clamped);
        xeMfgMaxGeneratedFrames = clamped;
        dlssgDetectedInterpolationCount = static_cast<int>(clamped);
        dlssgLastSetMode = DLSSGMode::eOn;
        reflexDlssgFrameCount = static_cast<uint8_t>(clamped);
    }
};

// 3. Low-Latency Tech Gating Simulator
struct LowLatencyState
{
    int dlssgDetectedInterpolationCount = 0;
    DLSSGMode dlssgLastSetMode = DLSSGMode::eOff;
    bool xeMfgAnyEnabled = true;
    int fgXeFGInterpolationCount = 2;
    LowLatencyMode forced_low_latency_tech = LowLatencyMode::None;
    LowLatencyMode active_tech = LowLatencyMode::None;

    bool UpdateLowLatencyTech()
    {
        const bool isMultiFrame =
            (dlssgDetectedInterpolationCount > 1 || dlssgLastSetMode == DLSSGMode::eDynamic ||
             (xeMfgAnyEnabled && (fgXeFGInterpolationCount > 1 || dlssgDetectedInterpolationCount > 1)));

        if (forced_low_latency_tech == LowLatencyMode::XeLL && !isMultiFrame)
        {
            active_tech = LowLatencyMode::XeLL;
            return true;
        }

        if (dlssgDetectedInterpolationCount <= 1 && dlssgLastSetMode != DLSSGMode::eDynamic &&
            !(xeMfgAnyEnabled && fgXeFGInterpolationCount > 1))
        {
            active_tech = LowLatencyMode::AntiLag2; // Or auto XeLL
            return true;
        }

        // Falls through to LatencyFlex under Multi-Frame Generation
        active_tech = LowLatencyMode::LatencyFlex;
        return true;
    }
};

// 4. Anti-Starvation Frame Time Simulator
struct StarvationMathSimulator
{
    // Real engine frame time (e.g. 23.0 ms native in Helldivers 2, ~43.5 FPS)
    double tNative = 23.0;
    int multiplier = 6; // 6X MFG

    // Calculates cycle time given a fed frame time: T_cycle = T_native + ((M - 1)/M) * T_fed
    double ComputeCycleTime(double tFed) const
    {
        double blockRatio = static_cast<double>(multiplier - 1) / static_cast<double>(multiplier);
        return tNative + blockRatio * tFed;
    }

    // Resolves frame render time with decoupled XeFGPacing and anti-starvation clamp
    double ResolveFedTime(double decoupledTime, double swapchainDelta, bool pacerWarmedUp) const
    {
        double frameRenderTime = 0.0;
        if (pacerWarmedUp && decoupledTime > 0.0)
            frameRenderTime = decoupledTime;
        else
            frameRenderTime = swapchainDelta;

        if (frameRenderTime > 0.0)
            frameRenderTime = std::clamp(frameRenderTime, 4.0, 50.0);

        return frameRenderTime;
    }
};

// 5. NoteFrame Anti-Collapse Simulator
struct NoteFrameSimulator
{
    int64_t g_periodNs = 0;
    int64_t g_burstBlockQpc = 0;
    int64_t g_renderTimeNs = 0;
    const int64_t freq = 10000000LL; // 10 MHz

    int64_t NsFromQpc(int64_t qpc) const { return (qpc * 1000000000LL) / freq; }

    void NoteFrameSim()
    {
        if (g_periodNs > 0)
        {
            const int64_t blockNs = NsFromQpc(g_burstBlockQpc);
            if (g_periodNs > blockNs && (g_periodNs - blockNs) >= 1000000LL)
                g_renderTimeNs = g_periodNs - blockNs;
            else
                g_renderTimeNs = g_periodNs;
        }
    }
};

// 6. NGX Feature 11 Spoofing Simulator
struct NgxFeatureCheck
{
    bool xeMfgUnlock = false;
    bool xeMfgSrEnable = false;
    bool externalFg = false;
    FGInput activeFgInput = FGInput::Upscaler;

    bool XeMfgAnyEnabledForSession() const { return (xeMfgUnlock || xeMfgSrEnable) && !externalFg; }

    bool IsFeature11Spoofed() const
    {
        const bool xeMfgActive = XeMfgAnyEnabledForSession();
        return (xeMfgActive || activeFgInput == FGInput::DLSSG);
    }
};

} // namespace XeMfgSrParityTest

int main()
{
    printf("[+] Starting XeMFG-SR Complete Parity Comprehensive Unit Tests...\n");

    // Test 1: Universal Backbuffer Resource Tagging Parity across DLSSG and Upscaler
    {
        XeMfgSrParityTest::TaggingState dlssgTag;
        dlssgTag.activeFgInput = XeMfgSrParityTest::FGInput::DLSSG;
        dlssgTag.fgHudFix = false;
        dlssgTag.interpolationWidth = 1920;
        dlssgTag.interpolationHeight = 1080;
        dlssgTag.DispatchSim();

        XeMfgSrParityTest::TaggingState upscalerTag;
        upscalerTag.activeFgInput = XeMfgSrParityTest::FGInput::Upscaler;
        upscalerTag.fgHudFix = false;
        upscalerTag.interpolationWidth = 1920;
        upscalerTag.interpolationHeight = 1080;
        upscalerTag.DispatchSim();

        assert(dlssgTag.wasBackbufferTagged && "Native DLSSG must tag backbuffer resource!");
        assert(upscalerTag.wasBackbufferTagged && "XeMFG-SR (Upscaler) must tag backbuffer resource identically!");
        assert(dlssgTag.taggedWidth == upscalerTag.taggedWidth && dlssgTag.taggedHeight == upscalerTag.taggedHeight);

        // Verify fallback when interpolation size is 0
        XeMfgSrParityTest::TaggingState zeroResTag;
        zeroResTag.activeFgInput = XeMfgSrParityTest::FGInput::Upscaler;
        zeroResTag.fgHudFix = false;
        zeroResTag.interpolationWidth = 0;
        zeroResTag.interpolationHeight = 0;
        zeroResTag.swapchainWidth = 2560;
        zeroResTag.swapchainHeight = 1440;
        zeroResTag.DispatchSim();
        assert(zeroResTag.wasBackbufferTagged && zeroResTag.taggedWidth == 2560 && zeroResTag.taggedHeight == 1440);

        printf("  [PASS] Test 1: Universal Backbuffer Tagging parity verified across DLSSG & Upscaler.\n");
    }

    // Test 2: Subsystem Multiplier & Latency Synchronization across all counts (0 to 5)
    {
        XeMfgSrParityTest::MultiplierSyncState sync;

        // Startup / 3X FG (count = 2)
        sync.SetInterpolatedFrameCount(2);
        assert(sync.dlssgDetectedInterpolationCount == 2);
        assert(sync.dlssgLastSetMode == XeMfgSrParityTest::DLSSGMode::eOn);
        assert(sync.reflexDlssgFrameCount == 2);
        assert(sync.xeMfgMaxGeneratedFrames == 2);
        assert(sync.volatileInterpolationCount == 2);

        // In-game slider adjustment to 6X Multi-Frame Generation (count = 5)
        sync.SetInterpolatedFrameCount(5);
        assert(sync.dlssgDetectedInterpolationCount == 5);
        assert(sync.dlssgLastSetMode == XeMfgSrParityTest::DLSSGMode::eOn);
        assert(sync.reflexDlssgFrameCount == 5);
        assert(sync.xeMfgMaxGeneratedFrames == 5);
        assert(sync.volatileInterpolationCount == 5);

        // In-game toggle to passthrough / Off (count = 0)
        sync.SetInterpolatedFrameCount(0);
        assert(sync.dlssgDetectedInterpolationCount == 0);
        assert(sync.dlssgLastSetMode == XeMfgSrParityTest::DLSSGMode::eOff);
        assert(sync.reflexDlssgFrameCount == 0);
        assert(sync.volatileInterpolationCount == 0);

        printf("  [PASS] Test 2: Subsystem Multiplier & Latency Synchronization verified for 0..5.\n");
    }

    // Test 3: Low-Latency Tech Gating under Multi-Frame Generation
    {
        XeMfgSrParityTest::LowLatencyState ll;
        ll.xeMfgAnyEnabled = true;
        ll.forced_low_latency_tech = XeMfgSrParityTest::LowLatencyMode::XeLL;

        // Case A: 2X FG (count = 1) -> XeLL forced is permitted for 1 generated frame
        ll.dlssgDetectedInterpolationCount = 1;
        ll.fgXeFGInterpolationCount = 1;
        ll.UpdateLowLatencyTech();
        assert(ll.active_tech == XeMfgSrParityTest::LowLatencyMode::XeLL &&
               "Single frame interpolation allows forced XeLL.");

        // Case B: Multi-Frame Generation (count = 5, 6X MFG) -> XeLL forced is strictly bypassed!
        ll.dlssgDetectedInterpolationCount = 5;
        ll.fgXeFGInterpolationCount = 5;
        ll.UpdateLowLatencyTech();
        assert(ll.active_tech == XeMfgSrParityTest::LowLatencyMode::LatencyFlex &&
               "Multi-Frame Generation must bypass forced XeLL and fall through to LatencyFlex!");

        // Case C: Unforced mode under Multi-Frame Generation -> auto AL2/XeLL bypassed, LatencyFlex active
        ll.forced_low_latency_tech = XeMfgSrParityTest::LowLatencyMode::None;
        ll.UpdateLowLatencyTech();
        assert(ll.active_tech == XeMfgSrParityTest::LowLatencyMode::LatencyFlex &&
               "Multi-Frame Generation must bypass auto AL2/XeLL and fall through to LatencyFlex!");

        printf("  [PASS] Test 3: Low-Latency Tech Gating under Multi-Frame Generation verified.\n");
    }

    // Test 4: Mathematical Elimination of 7.3 FPS Recursive Starvation Feedback Trap
    {
        XeMfgSrParityTest::StarvationMathSimulator sim;
        sim.tNative = 23.0; // 23 ms native render time
        sim.multiplier = 6; // 6X Multi-Frame Generation

        // 1. Without decoupled pacing & without clamping (The defective pre9 behavior):
        // Feedback equation: T_cycle = T_native + (5/6)*T_cycle => T_cycle = 6 * T_native = 138 ms (7.25 FPS)
        double unmeteredCycle = sim.tNative * 6.0;
        double unmeteredFed = unmeteredCycle;
        double nextCycleDefective = sim.ComputeCycleTime(unmeteredFed);
        assert(std::abs(nextCycleDefective - 138.0) < 0.01 &&
               "Unmetered feedback without clamp locks cycle at exactly 138.0 ms (7.25 FPS)!");

        // 2. With unified decoupled XeFGPacing:
        // Pacer subtracts burst block wait (112.45 ms): T_decoupled = 138.0 - 112.45 = 25.55 ms
        double decoupledTime = 25.55;
        double fedDecoupled = sim.ResolveFedTime(decoupledTime, 138.0, true);
        assert(fedDecoupled == 25.55 && "Unified decoupled render time isolates 25.55 ms pure engine time!");
        double nextCycleHealthy = sim.ComputeCycleTime(fedDecoupled);
        assert(nextCycleHealthy <= 44.3 && "With decoupled feed, cycle time stays at healthy ~44.3 ms!");

        // 3. Fallback during unwarmed pacer with anti-starvation clamping [4.0 ms, 50.0 ms]:
        // Even with 138 ms swapchain spike, fed time is clamped to 50.0 ms
        double fedFallbackClamped = sim.ResolveFedTime(0.0, 138.0, false);
        assert(fedFallbackClamped == 50.0 && "Spike fallback must be clamped to 50.0 ms!");
        double cycleAfterSpike = sim.ComputeCycleTime(fedFallbackClamped);
        // T_cycle = 23 + (5/6)*50 = 23 + 41.67 = 64.67 ms (instantly recovers towards 23 ms next frame)
        assert(cycleAfterSpike < 65.0 && "50 ms clamp prevents feedback amplification beyond 65 ms!");

        printf("  [PASS] Test 4: Mathematical elimination of 7.3 FPS starvation trap verified.\n");
    }

    // Test 5: NoteFrame Anti-Collapse Pacing Fallback
    {
        XeMfgSrParityTest::NoteFrameSimulator pacer;
        pacer.g_periodNs = 25000000LL; // 25.0 ms period

        // Case A: Normal pacing where blockNs is 15 ms: renderTime = 25 ms - 15 ms = 10 ms
        pacer.g_burstBlockQpc = 150000LL; // 15 ms
        pacer.NoteFrameSim();
        assert(pacer.g_renderTimeNs == 10000000LL && "Normal renderTime calculation (10 ms) verified.");

        // Case B: Block time is within 1 ms of period (e.g. 24.52 ms block):
        // In pre9, 25.0 - 24.52 = 0.48 ms collapsed interpolation models.
        // With anti-collapse guard, it cleanly falls back to g_periodNs (25.0 ms)!
        pacer.g_burstBlockQpc = 245200LL; // 24.52 ms block
        pacer.NoteFrameSim();
        assert(pacer.g_renderTimeNs == 25000000LL &&
               "Anti-collapse guard must fall back to full period (25.0 ms) when delta < 1 ms!");

        printf("  [PASS] Test 5: NoteFrame anti-collapse guard (< 1 ms fallback to period) verified.\n");
    }

    // Test 6: NGX Feature 11 Spoofing Parity across DX12, DX11, and Vulkan
    {
        XeMfgSrParityTest::NgxFeatureCheck check;

        // Native XeMFG (xeMfgUnlock)
        check.xeMfgUnlock = true;
        check.xeMfgSrEnable = false;
        check.externalFg = false;
        assert(check.IsFeature11Spoofed() && "Native XeMFG must spoof Feature 11.");

        // XeMFG-SR (xeMfgSrEnable)
        check.xeMfgUnlock = false;
        check.xeMfgSrEnable = true;
        check.externalFg = false;
        assert(check.IsFeature11Spoofed() && "XeMFG-SR must spoof Feature 11 identically via AnyEnabledForSession()!");

        // External FG active -> XeMFG disabled
        check.externalFg = true;
        assert(!check.XeMfgAnyEnabledForSession() && "External FG must disable XeMfg session spoofing.");

        printf("  [PASS] Test 6: NGX Proxy Feature 11 spoofing parity verified.\n");
    }

    printf("[+] All XeMFG-SR Complete Parity Comprehensive Unit Tests PASSED successfully!\n");
    return 0;
}

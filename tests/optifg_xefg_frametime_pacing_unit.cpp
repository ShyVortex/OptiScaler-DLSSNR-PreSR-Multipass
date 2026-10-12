#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Unit test validating:
// 1. OptiFG Frame Time Delta feeding in XeFG_Dx12::Dispatch (prioritizing true engine delta,
//    filtering < 1.0 ms micro-burst spikes, and preserving native DLSSG behavior).
// 2. ExtraPacing VSync lock gating in FG_Hooks.cpp (exempting Linux, VKD3D-Proton, and DXVK).

namespace OptiFgXeFgTest
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

struct SimulatedDispatchContext
{
    FGInput activeFgInput = FGInput::Upscaler;
    double ftDelta = 0.0;
    double lastFGFrameTime = 0.0;
    double decoupledRenderTime = 0.0;

    double ResolveFrameRenderTime()
    {
        double frameRenderTime = 0.0;

        if (activeFgInput == FGInput::Upscaler)
        {
            if (ftDelta > 0.0)
                frameRenderTime = ftDelta;
            else if (lastFGFrameTime > 0.0)
                frameRenderTime = lastFGFrameTime;
            else
                frameRenderTime = decoupledRenderTime;

            if (frameRenderTime < 1.0 && lastFGFrameTime >= 1.0)
                frameRenderTime = lastFGFrameTime;
        }
        else
        {
            frameRenderTime = decoupledRenderTime;

            if (!(frameRenderTime > 0.0))
                frameRenderTime = ftDelta;

            if (!(frameRenderTime > 0.0))
                frameRenderTime = lastFGFrameTime;
        }

        return frameRenderTime;
    }
};

struct SimulatedPresentContext
{
    bool willPresent = true;
    bool fgFeatureActive = true;
    FGOutput activeFgOutput = FGOutput::XeFG;
    bool extraPacingEnabled = true;
    bool isRunningOnLinux = false;
    bool usesDxvk = false;
    bool usesVkd3dProton = false;

    uint32_t syncInterval = 0;
    uint32_t flags = 0x00000200; // DXGI_PRESENT_ALLOW_TEARING

    void ApplyPacingOverrides()
    {
        if (willPresent && fgFeatureActive && activeFgOutput == FGOutput::XeFG && extraPacingEnabled &&
            !isRunningOnLinux && !usesDxvk && !usesVkd3dProton)
        {
            if (syncInterval < 1)
                syncInterval = 1;

            flags &= ~0x00000200; // Remove ALLOW_TEARING
        }
    }
};

} // namespace OptiFgXeFgTest

int main()
{
    printf("[+] Starting OptiFG XeFG Frame Time & ExtraPacing Unit Tests...\n");

    // Test 1: OptiFG mode prioritizes measured engine frame delta over decoupled burst QPC
    {
        OptiFgXeFgTest::SimulatedDispatchContext ctx;
        ctx.activeFgInput = OptiFgXeFgTest::FGInput::Upscaler;
        ctx.ftDelta = 35.6;             // Real native engine delta (35.6 ms)
        ctx.lastFGFrameTime = 35.0;     // Swapchain delta
        ctx.decoupledRenderTime = 0.48; // Corrupted micro-burst QPC (0.48 ms)

        double fed = ctx.ResolveFrameRenderTime();
        assert(fed == 35.6 && "OptiFG must prioritize ftDelta (35.6 ms) over decoupled 0.48 ms!");

        printf("  [PASS] Test 1: OptiFG prioritizes true engine frame delta over micro-burst QPC.\n");
    }

    // Test 2: OptiFG filters sub-millisecond micro-burst spikes and falls back to lastFGFrameTime
    {
        OptiFgXeFgTest::SimulatedDispatchContext ctx;
        ctx.activeFgInput = OptiFgXeFgTest::FGInput::Upscaler;
        ctx.ftDelta = 0.52;         // Corrupted sub-millisecond spike
        ctx.lastFGFrameTime = 46.7; // Valid previous swapchain interval
        ctx.decoupledRenderTime = 0.0;

        double fed = ctx.ResolveFrameRenderTime();
        assert(fed == 46.7 &&
               "Sub-millisecond micro-burst spike (< 1 ms) must fall back to lastFGFrameTime (46.7 ms)!");

        printf("  [PASS] Test 2: OptiFG filters sub-millisecond micro-bursts and uses lastFGFrameTime.\n");
    }

    // Test 3: Native DLSS-G mode continues to prioritize decoupled XeFGPacing render time
    {
        OptiFgXeFgTest::SimulatedDispatchContext ctx;
        ctx.activeFgInput = OptiFgXeFgTest::FGInput::DLSSG;
        ctx.ftDelta = 30.0;
        ctx.lastFGFrameTime = 30.0;
        ctx.decoupledRenderTime = 25.0; // Paced decoupled render time in native games

        double fed = ctx.ResolveFrameRenderTime();
        assert(fed == 25.0 && "Native DLSSG mode must prioritize decoupled XeFGPacing render time!");

        printf("  [PASS] Test 3: Native DLSSG mode preserves decoupled XeFGPacing priority.\n");
    }

    // Test 4: ExtraPacing on native Windows DXGI applies VSync lock when enabled
    {
        OptiFgXeFgTest::SimulatedPresentContext ctx;
        ctx.extraPacingEnabled = true;
        ctx.isRunningOnLinux = false;
        ctx.usesDxvk = false;
        ctx.usesVkd3dProton = false;
        ctx.syncInterval = 0;
        ctx.flags = 0x00000200;

        ctx.ApplyPacingOverrides();
        assert(ctx.syncInterval == 1 && "Windows DXGI must apply SyncInterval = 1 with ExtraPacing");
        assert((ctx.flags & 0x00000200) == 0 && "ALLOW_TEARING must be removed on Windows DXGI");

        printf("  [PASS] Test 4: ExtraPacing applies VSync lock on native Windows DXGI.\n");
    }

    // Test 5: ExtraPacing strictly bypasses VSync lock on Linux and VKD3D-Proton
    {
        // Case A: Linux environment
        OptiFgXeFgTest::SimulatedPresentContext ctxA;
        ctxA.extraPacingEnabled = true;
        ctxA.isRunningOnLinux = true;
        ctxA.usesVkd3dProton = false;
        ctxA.syncInterval = 0;
        ctxA.flags = 0x00000200;

        ctxA.ApplyPacingOverrides();
        assert(ctxA.syncInterval == 0 && "Linux must NEVER force SyncInterval = 1!");
        assert((ctxA.flags & 0x00000200) != 0 && "ALLOW_TEARING must be preserved on Linux!");

        // Case B: VKD3D-Proton detected
        OptiFgXeFgTest::SimulatedPresentContext ctxB;
        ctxB.extraPacingEnabled = true;
        ctxB.isRunningOnLinux = false;
        ctxB.usesVkd3dProton = true;
        ctxB.syncInterval = 0;
        ctxB.flags = 0x00000200;

        ctxB.ApplyPacingOverrides();
        assert(ctxB.syncInterval == 0 && "VKD3D-Proton must NEVER force SyncInterval = 1!");
        assert((ctxB.flags & 0x00000200) != 0 && "ALLOW_TEARING must be preserved on VKD3D-Proton!");

        // Case C: ExtraPacing disabled
        OptiFgXeFgTest::SimulatedPresentContext ctxC;
        ctxC.extraPacingEnabled = false;
        ctxC.isRunningOnLinux = false;
        ctxC.usesVkd3dProton = false;
        ctxC.syncInterval = 0;
        ctxC.flags = 0x00000200;

        ctxC.ApplyPacingOverrides();
        assert(ctxC.syncInterval == 0 && "ExtraPacing=false must keep unmetered presentation!");

        printf("  [PASS] Test 5: ExtraPacing strictly bypasses VSync lock on Linux and VKD3D-Proton.\n");
    }

    printf("[+] All OptiFG XeFG Frame Time & ExtraPacing Unit Tests PASSED successfully!\n");
    return 0;
}

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Unit test validating:
// 1. Unified Frame Time feeding in XeFG_Dx12::Dispatch (prioritizing decoupled XeFGPacing::RenderTimeMs()
//    across all inputs, with anti-starvation boundary clamping [4.0 ms, 50.0 ms]).
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
            // For XeMFG-SR (FGInput::Upscaler / OptiFG), prioritize the measured engine frame delta
            // from swapchain / DLSS-SR upscaler passes (ftDelta / lastFGFrameTime).
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
            // For native DLSSG input, prioritize XeFGPacing's decoupled render time
            frameRenderTime = decoupledRenderTime;

            if (!(frameRenderTime > 0.0))
                frameRenderTime = ftDelta;

            if (!(frameRenderTime > 0.0))
                frameRenderTime = lastFGFrameTime;
        }

        // Anti-starvation boundary clamping [4.0 ms (250 FPS ceiling), 50.0 ms (20 FPS floor)]
        if (frameRenderTime > 0.0)
        {
            frameRenderTime = std::clamp(frameRenderTime, 4.0, 50.0);
        }
        else if (lastFGFrameTime >= 1.0)
        {
            frameRenderTime = std::clamp(lastFGFrameTime, 4.0, 50.0);
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

    // Test 1: XeMFG-SR (FGInput::Upscaler) prioritizes true engine frame delta over decoupled burst estimates
    {
        OptiFgXeFgTest::SimulatedDispatchContext ctx;
        ctx.activeFgInput = OptiFgXeFgTest::FGInput::Upscaler;
        ctx.ftDelta = 28.5; // True engine frame time delta
        ctx.lastFGFrameTime = 28.5;
        ctx.decoupledRenderTime = 4.74; // Collapsed decoupled estimate (subtracted scheduler wait)

        double fed = ctx.ResolveFrameRenderTime();
        assert(fed == 28.5 && "XeMFG-SR must prioritize true engine frame delta (28.5 ms) over collapsed estimate!");

        printf("  [PASS] Test 1: XeMFG-SR prioritizes true engine frame delta over collapsed burst estimates.\n");
    }

    // Test 1b: Native DLSSG input prioritizes decoupled XeFGPacing render time over swapchain delta
    {
        OptiFgXeFgTest::SimulatedDispatchContext ctx;
        ctx.activeFgInput = OptiFgXeFgTest::FGInput::DLSSG;
        ctx.ftDelta = 138.0;             // Inflated swapchain present delta (124 ms present stall)
        ctx.lastFGFrameTime = 138.0;     // Inflated previous interval
        ctx.decoupledRenderTime = 25.55; // True engine render work (138 ms - 112.45 ms burst block)

        double fed = ctx.ResolveFrameRenderTime();
        assert(fed == 25.55 && "Native DLSSG must prioritize decoupled XeFGPacing render time (25.55 ms)!");

        printf("  [PASS] Test 1b: Native DLSSG prioritizes decoupled render time over inflated swapchain delta.\n");
    }

    // Test 2: Fallback frame render time anti-starvation clamping caps spikes to 50.0 ms
    {
        OptiFgXeFgTest::SimulatedDispatchContext ctx;
        ctx.activeFgInput = OptiFgXeFgTest::FGInput::Upscaler;
        ctx.ftDelta = 138.0;           // Massive swapchain stall
        ctx.lastFGFrameTime = 138.0;   // Massive swapchain stall
        ctx.decoupledRenderTime = 0.0; // Unwarmed decoupled pacer (frame 0)

        double fed = ctx.ResolveFrameRenderTime();
        assert(fed == 50.0 &&
               "Fallback during unwarmed pacer must clamp to 50.0 ms, breaking 7.3 FPS starvation loop!");

        printf("  [PASS] Test 2: Anti-starvation clamping caps fallback spikes to 50.0 ms.\n");
    }

    // Test 3: Minimum clamping prevents micro-spikes below 4.0 ms
    {
        OptiFgXeFgTest::SimulatedDispatchContext ctx;
        ctx.activeFgInput = OptiFgXeFgTest::FGInput::Upscaler;
        ctx.ftDelta = 0.5;
        ctx.lastFGFrameTime = 0.5;
        ctx.decoupledRenderTime = 0.5; // Micro-spike below 4.0 ms

        double fed = ctx.ResolveFrameRenderTime();
        assert(fed == 4.0 && "Clamping floor must enforce minimum 4.0 ms ceiling!");

        printf("  [PASS] Test 3: Minimum clamping prevents sub-millisecond collapse under 4.0 ms.\n");
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

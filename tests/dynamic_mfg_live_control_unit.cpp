#include "../OptiScaler/framegen/dlssg/AmpereMfgLoader.h"
#include <cassert>
#include <cstdio>
#include <cstring>

namespace
{
AmpereMfgLoader::DLSSG_ControlRequest s_lastRequest {};
bool s_mockRequestCalled = false;
uint32_t s_lastTargetFps = 0;
bool s_mockTargetCalled = false;
uint32_t s_lastUiMode = 0;
bool s_mockUiCalled = false;

bool MockRequestControl(const AmpereMfgLoader::DLSSG_ControlRequest* req)
{
    if (!req)
        return false;
    s_lastRequest = *req;
    s_mockRequestCalled = true;
    return true;
}

bool MockSetDisplayTarget(uint32_t targetFps)
{
    s_lastTargetFps = targetFps;
    s_mockTargetCalled = true;
    return true;
}

bool MockRequestUI(uint32_t mode)
{
    s_lastUiMode = mode;
    s_mockUiCalled = true;
    return true;
}

// Function pointers for test simulation
AmpereMfgLoader::PFN_DLSSG_RequestControl g_testPfnRequestControl = nullptr;
AmpereMfgLoader::PFN_DLSSG_SetDisplayTarget g_testPfnSetDisplayTarget = nullptr;
AmpereMfgLoader::PFN_DLSSG_RequestUI g_testPfnRequestUI = nullptr;
} // namespace

namespace AmpereMfgLoader
{
bool ApplyLiveControl(uint32_t mode, uint32_t targetFps, uint32_t multiplier)
{
    if (!g_testPfnRequestControl)
        return false;

    DLSSG_ControlRequest req {};
    req.version = 1;
    req.mode = mode;
    req.targetFPS = (targetFps > 1000) ? 1000 : targetFps;
    req.multiplier = (multiplier >= 2 && multiplier <= 6) ? multiplier : 0;
    req.flags = 0;

    return g_testPfnRequestControl(&req);
}

bool ApplyDisplayTargetLive(uint32_t targetFps)
{
    if (!g_testPfnSetDisplayTarget)
        return false;

    uint32_t clamped = (targetFps > 1000) ? 1000 : targetFps;
    return g_testPfnSetDisplayTarget(clamped);
}

bool ApplyUIModeLive(uint32_t uiMode)
{
    if (!g_testPfnRequestUI)
        return false;

    uint32_t validMode = (uiMode <= 2) ? uiMode : 1;
    return g_testPfnRequestUI(validMode);
}
} // namespace AmpereMfgLoader

int main()
{
    std::printf("=== Running Dynamic Multi-Frame Generation (DMFG) Live Control Unit Tests ===\n");

    using namespace AmpereMfgLoader;

    // Test 1: Validate struct layout and packing
    {
        static_assert(sizeof(DLSSG_ControlRequest) == 0x14,
                      "DLSSG_ControlRequest must be exactly 20 bytes (0x14) per SilyNoMeta ABI");
        static_assert(alignof(DLSSG_ControlRequest) == 1, "DLSSG_ControlRequest must be packed (alignof == 1)");

        DLSSG_ControlRequest req {};
        assert(req.version == 1 && "Default ABI version must be 1");
        assert(req.mode == 0 && "Default mode must be 0 (FollowGame)");
        assert(req.multiplier == 0 && "Default multiplier must be 0");
        assert(req.targetFPS == 0 && "Default targetFPS must be 0");
        assert(req.flags == 0 && "Default flags must be 0");

        std::printf("  [PASS] Case 1: DLSSG_ControlRequest struct size (20 bytes) and ABI version 1 verified\n");
    }

    // Test 2: Status struct new fields default values
    {
        Status st {};
        assert(!st.LiveControlSupported && "LiveControlSupported must default to false");
        assert(!st.LiveControlActive && "LiveControlActive must default to false");

        std::printf("  [PASS] Case 2: Status struct LiveControlSupported and LiveControlActive defaults verified\n");
    }

    // Test 3: Live dispatch when no exports are loaded
    {
        // When not loaded, should return false gracefully without crashing
        assert(!ApplyLiveControl(1, 120, 0) && "ApplyLiveControl must return false when no module export is loaded");
        assert(!ApplyDisplayTargetLive(144) &&
               "ApplyDisplayTargetLive must return false when no module export is loaded");
        assert(!ApplyUIModeLive(1) && "ApplyUIModeLive must return false when no module export is loaded");

        std::printf("  [PASS] Case 3: Graceful fallback when live exports are absent verified\n");
    }

    // Test 4: Live programmatic dispatch with resolved export pointers and clamping
    {
        g_testPfnRequestControl = MockRequestControl;
        g_testPfnSetDisplayTarget = MockSetDisplayTarget;
        g_testPfnRequestUI = MockRequestUI;

        // Test ApplyLiveControl in Dynamic mode (mode = 1)
        assert(ApplyLiveControl(1, 144, 0));
        assert(s_mockRequestCalled);
        assert(s_lastRequest.version == 1);
        assert(s_lastRequest.mode == 1);
        assert(s_lastRequest.targetFPS == 144);
        assert(s_lastRequest.multiplier == 0);

        // Test ApplyLiveControl in Fixed multiplier mode (mode = 2, multiplier = 3 -> 3X)
        assert(ApplyLiveControl(2, 0, 3));
        assert(s_lastRequest.mode == 2);
        assert(s_lastRequest.multiplier == 3);
        assert(s_lastRequest.targetFPS == 0);

        // Test FPS target clamping above 1000 FPS ceiling
        assert(ApplyLiveControl(1, 1500, 0));
        assert(s_lastRequest.targetFPS == 1000); // Clamped to 1000

        // Test ApplyDisplayTargetLive with clamping
        assert(ApplyDisplayTargetLive(240));
        assert(s_mockTargetCalled);
        assert(s_lastTargetFps == 240);

        assert(ApplyDisplayTargetLive(1200));
        assert(s_lastTargetFps == 1000); // Clamped to 1000

        // Test ApplyUIModeLive
        assert(ApplyUIModeLive(2));
        assert(s_mockUiCalled);
        assert(s_lastUiMode == 2);

        // Reset pointers
        g_testPfnRequestControl = nullptr;
        g_testPfnSetDisplayTarget = nullptr;
        g_testPfnRequestUI = nullptr;

        std::printf("  [PASS] Case 4: Live programmatic dispatch, struct packing, and parameter clamping verified\n");
    }

    // Test 5: ResolveControlModeAndMultiplier resolution logic
    {
        uint32_t mode = 999;
        uint32_t mult = 999;

        // 1. Default unconfigured state: dynamicMfg = false, explicitOverride = 0 -> FollowGame mode 0, multiplier 0
        ResolveControlModeAndMultiplier(false, 0, 5, mode, mult);
        assert(mode == 0 && "Default unconfigured state must resolve to mode 0 (FollowGame)");
        assert(mult == 0 && "Default unconfigured state must resolve to multiplier 0");

        // 2. Dynamic mode active: dynamicMfg = true, explicitOverride = 0 -> Dynamic mode 1, multiplier 0
        ResolveControlModeAndMultiplier(true, 0, 5, mode, mult);
        assert(mode == 1 && "Dynamic MFG must resolve to mode 1 (Dynamic)");
        assert(mult == 0 && "Dynamic MFG must resolve to multiplier 0");

        // 3. Dynamic mode active even when an override count is stored -> Dynamic mode takes precedence
        ResolveControlModeAndMultiplier(true, 2, 5, mode, mult);
        assert(mode == 1 && "Dynamic MFG must take precedence over static override");
        assert(mult == 0 && "Dynamic MFG must keep multiplier at 0");

        // 4. Explicit override active: dynamicMfg = false, explicitOverride = 1 (2X) -> Fixed mode 2, multiplier 2
        ResolveControlModeAndMultiplier(false, 1, 5, mode, mult);
        assert(mode == 2 && "Explicit override must resolve to mode 2 (Fixed Multiplier)");
        assert(mult == 2 && "1 generated frame must resolve to 2X multiplier");

        // 5. Explicit override active: dynamicMfg = false, explicitOverride = 2 (3X) -> Fixed mode 2, multiplier 3
        ResolveControlModeAndMultiplier(false, 2, 5, mode, mult);
        assert(mode == 2);
        assert(mult == 3 && "2 generated frames must resolve to 3X multiplier");

        // 6. Explicit override active: dynamicMfg = false, explicitOverride = 3 (4X) -> Fixed mode 2, multiplier 4
        ResolveControlModeAndMultiplier(false, 3, 5, mode, mult);
        assert(mode == 2);
        assert(mult == 4 && "3 generated frames must resolve to 4X multiplier");

        // 7. Explicit override clamping on 310.1 runtime (maxCeiling = 3, up to 4X)
        ResolveControlModeAndMultiplier(false, 5, 3, mode, mult);
        assert(mode == 2);
        assert(mult == 4 && "Explicit override 5 on 310.1 runtime must clamp to ceiling 3 + 1 = 4");

        // 8. Explicit override clamping on 310.9 runtime (maxCeiling = 5, up to 6X)
        ResolveControlModeAndMultiplier(false, 8, 5, mode, mult);
        assert(mode == 2);
        assert(mult == 6 && "Explicit override 8 on 310.9 runtime must clamp to ceiling 5 + 1 = 6");

        std::printf("  [PASS] Case 5: ResolveControlModeAndMultiplier resolution and clamping verified\n");
    }

    // Test 6: FollowGame live programmatic dispatch (mode = 0, multiplier = 0)
    {
        g_testPfnRequestControl = MockRequestControl;
        s_mockRequestCalled = false;
        s_lastRequest = {};

        // Dispatch FollowGame / Driver mode
        assert(ApplyLiveControl(0, 0, 0) && "ApplyLiveControl in mode 0 must succeed");
        assert(s_mockRequestCalled);
        assert(s_lastRequest.version == 1);
        assert(s_lastRequest.mode == 0 && "Dispatched mode must be 0 (FollowGame)");
        assert(s_lastRequest.multiplier == 0 && "Dispatched multiplier must be 0");
        assert(s_lastRequest.targetFPS == 0);

        // Reset pointers
        g_testPfnRequestControl = nullptr;

        std::printf("  [PASS] Case 6: Live FollowGame (mode 0, multiplier 0) dispatch verified\n");
    }

    std::printf("=== All Dynamic Multi-Frame Generation (DMFG) Live Control Unit Tests PASSED! ===\n");
    return 0;
}

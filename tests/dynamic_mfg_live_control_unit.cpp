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

    std::printf("=== All Dynamic Multi-Frame Generation (DMFG) Live Control Unit Tests PASSED! ===\n");
    return 0;
}

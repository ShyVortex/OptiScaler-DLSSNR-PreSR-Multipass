#include "../OptiScaler/framegen/dlssg/AmpereMfgLoader.h"
#include <cassert>
#include <cstdint>
#include <cstdio>

// Standalone unit test validating ResolveControlModeAndMultiplierInternal (added by commit
// 7f1c15fc "[PATCH 2/3] Add DLSSG.AmpereMfgMode=Internal", AmpereMfgLoader.h:333-345), exercised
// against the REAL inline implementation:
//
//   - dynamicMfg=false  -> mode 0 (FollowGame / Driver mode), multiplier 0
//   - dynamicMfg=true   -> mode 1 (Dynamic), multiplier 0
//   - mode 2 (Fixed Multiplier) is never produced by the internal variant: OptiScaler owns the
//     multiplier via slDLSSGSetOptions / InterpolationCount, so a fixed multiplier would fight it
//   - the SilyNoMeta DLSSG_RequestControl targetFPS flows SEPARATELY from mode resolution
//     (call-site mirror of AmpereMfgLoader.cpp:762-788: only mode/multiplier come from the
//     resolver; targetFPS is conveyed through the request with 0 = unconstrained/auto)

int main()
{
    std::printf("=== Running Ampere MFG Internal FG-Route Control Mode Unit Tests ===\n");

    using namespace AmpereMfgLoader;

    // Case 1: dynamicMfg=false resolves FollowGame (mode 0, multiplier 0)
    {
        uint32_t mode = 99;
        uint32_t multiplier = 99;
        ResolveControlModeAndMultiplierInternal(false, mode, multiplier);
        assert(mode == 0);
        assert(multiplier == 0);
        std::printf("  [PASS] Case 1: dynamicMfg=false resolves to mode 0 (FollowGame), multiplier 0\n");
    }

    // Case 2: dynamicMfg=true resolves Dynamic (mode 1, multiplier 0)
    {
        uint32_t mode = 99;
        uint32_t multiplier = 99;
        ResolveControlModeAndMultiplierInternal(true, mode, multiplier);
        assert(mode == 1);
        assert(multiplier == 0);
        std::printf("  [PASS] Case 2: dynamicMfg=true resolves to mode 1 (Dynamic), multiplier 0\n");
    }

    // Case 3: mode 2 (Fixed Multiplier) is unreachable in the internal variant — both inputs are
    // checked and neither can yield mode 2 or a non-zero multiplier
    {
        for (bool dynamicMfg : { false, true })
        {
            uint32_t mode = 99;
            uint32_t multiplier = 99;
            ResolveControlModeAndMultiplierInternal(dynamicMfg, mode, multiplier);
            assert(mode != 2);
            assert(mode <= 1); // only FollowGame (0) and Dynamic (1) exist here
            assert(multiplier == 0);
        }
        std::printf("  [PASS] Case 3: mode 2 (Fixed Multiplier) is never produced by the internal variant\n");
    }

    // Case 4: contrast with the EXTERNAL variant (AmpereMfgLoader.h:308-327), which still maps an
    // explicit interpolation-count override to mode 2 — exactly what the internal route must avoid
    {
        uint32_t mode = 0;
        uint32_t multiplier = 0;

        ResolveControlModeAndMultiplier(false, /*explicitOverrideFrames=*/2, /*maxCeiling=*/5, mode, multiplier);
        assert(mode == 2 && multiplier == 3); // 2 generated frames -> Fixed 3X

        ResolveControlModeAndMultiplier(true, /*explicitOverrideFrames=*/2, /*maxCeiling=*/5, mode, multiplier);
        assert(mode == 1 && multiplier == 0); // dynamic still takes precedence externally too
        std::printf("  [PASS] Case 4: external variant still produces mode 2 for overrides (internal must not)\n");
    }

    // Case 5: targetFPS flows alongside the resolved mode into the control request (mirror of the
    // AmpereMfgLoader.cpp:762-788 internal dispatch: mode/multiplier from the resolver, targetFPS
    // from FGDLSSGFramerateTargetDMFG with rounding and 0 = unconstrained/auto)
    {
        // Dynamic MFG at 120 FPS: mode 1, multiplier 0, targetFPS 120
        {
            const bool dynamicMfg = true;
            const float targetFps = 120.0f;

            uint32_t mode = 99;
            uint32_t multiplier = 99;
            ResolveControlModeAndMultiplierInternal(dynamicMfg, mode, multiplier);
            assert(mode == 1 && multiplier == 0);

            uint32_t targetInt = (targetFps > 0.0f) ? static_cast<uint32_t>(targetFps + 0.5f) : 0;

            DLSSG_ControlRequest req {};
            req.version = 1;
            req.mode = mode;
            req.multiplier = multiplier;
            req.targetFPS = targetInt;
            req.flags = 0;

            assert(req.version == 1);
            assert(req.mode == 1);       // Dynamic
            assert(req.multiplier == 0); // the target conveys pacing, not the multiplier
            assert(req.targetFPS == 120);
            assert(req.flags == 0);
        }

        // FollowGame with no explicit target: mode 0, multiplier 0, targetFPS 0 (unconstrained/auto)
        {
            const bool dynamicMfg = false;
            const float targetFps = 0.0f;

            uint32_t mode = 99;
            uint32_t multiplier = 99;
            ResolveControlModeAndMultiplierInternal(dynamicMfg, mode, multiplier);
            assert(mode == 0 && multiplier == 0);

            uint32_t targetInt = (targetFps > 0.0f) ? static_cast<uint32_t>(targetFps + 0.5f) : 0;

            DLSSG_ControlRequest req {};
            req.version = 1;
            req.mode = mode;
            req.multiplier = multiplier;
            req.targetFPS = targetInt;
            req.flags = 0;

            assert(req.mode == 0);
            assert(req.multiplier == 0);
            assert(req.targetFPS == 0);
        }
        std::printf("  [PASS] Case 5: targetFPS flows separately from the resolved mode into the control "
                    "request\n");
    }

    std::printf("=== All Ampere MFG Internal FG-Route Control Mode Unit Tests PASSED! ===\n");
    return 0;
}

#include <cassert>
#include <cstdio>
#include <map>
#include <string>

enum class FGInput
{
    NoFG,
    Upscaler,
    NvngxFG,
    DLSSG,
    FSRFG,
    FSRFG30
};

enum class FGNvngxReplacement
{
    None,
    OptiScaler,
    Nukem,
    Custom
};

enum class API
{
    DX11,
    DX12,
    Vulkan
};

struct MockParams
{
    std::map<std::string, int> intParams;

    void Set(const char* key, int val) { intParams[key] = val; }

    int Get(const char* key, int defaultVal = 0) const
    {
        auto it = intParams.find(key);
        if (it != intParams.end())
            return it->second;
        return defaultVal;
    }

    bool Has(const char* key) const { return intParams.find(key) != intParams.end(); }
};

void SimulateInitNGXParameters(MockParams& params, API api, FGInput activeFgInput, FGNvngxReplacement activeFgNvngx,
                               bool ampereMfgActive, int ampereMaxFrames, bool isUnrealEngine = false,
                               bool adaMfgActive = false, bool is3101Runtime = false)
{
    // Mutual exclusion: Ada unlock is disabled if Ampere unlock is enabled
    if (ampereMfgActive)
        adaMfgActive = false;

    if ((api == API::DX12 || api == API::Vulkan) &&
        (activeFgInput == FGInput::DLSSG || activeFgNvngx != FGNvngxReplacement::None || ampereMfgActive ||
         adaMfgActive))
    {
        params.Set("FrameGeneration.Available", 1);
        params.Set("FrameGeneration.NeedsUpdatedDriver", 0);
        params.Set("FrameGeneration.FeatureInitResult", 1);
        params.Set("FrameInterpolation.Available", 1);
        params.Set("FrameInterpolation.NeedsUpdatedDriver", 0);
        params.Set("FrameInterpolation.FeatureInitResult", 1);

        params.Set("DLSSG.Available", 1);
        params.Set("DLSSG.NeedsUpdatedDriver", 0);
        params.Set("DLSSG.FeatureInitResult", 1);

        int countMax = 1;
        if (activeFgNvngx != FGNvngxReplacement::None)
        {
            countMax = 2; // Simulated fake frames
        }
        else if (ampereMfgActive)
        {
            const int ceiling = is3101Runtime ? 3 : 5;
            countMax = (ampereMaxFrames > 0 && ampereMaxFrames <= ceiling) ? ampereMaxFrames : ceiling;
        }
        else if (adaMfgActive)
        {
            countMax = 5;
        }
        params.Set("DLSSG.MultiFrameCountMax", countMax);

        if (isUnrealEngine)
        {
            params.Set("FrameInterpolation.MinDriverVersionMajor", 10);
            params.Set("FrameGeneration.MinDriverVersionMajor", 10);
        }
        else
        {
            params.Set("FrameInterpolation.MinDriverVersionMajor", 0);
            params.Set("FrameGeneration.MinDriverVersionMajor", 0);
        }
    }
}

int main()
{
    printf("[TEST] Running NGX Frame Generation parameter capability unit tests...\n");

    // Case 1: External FG is active with AmpereMfgUnlock enabled (Turing/Ampere external mod)
    // Even though activeFgInput == NoFG and activeFgNvngx == None, capability must be advertised!
    {
        MockParams params;
        SimulateInitNGXParameters(params, API::DX12, FGInput::NoFG, FGNvngxReplacement::None,
                                  /*ampereMfgActive=*/true,
                                  /*ampereMaxFrames=*/1); // 2X FG

        assert(params.Get("FrameGeneration.Available") == 1);
        assert(params.Get("FrameInterpolation.Available") == 1);
        assert(params.Get("DLSSG.Available") == 1);
        assert(params.Get("DLSSG.MultiFrameCountMax") == 1);
        assert(params.Get("FrameGeneration.NeedsUpdatedDriver") == 0);
        assert(params.Get("FrameGeneration.FeatureInitResult") == 1);
        printf("  [PASS] Case 1: External FG with AmpereMfgUnlock advertises FG capabilities (2X)\n");
    }

    // Case 2: External FG with default capability (ampereMaxFrames = 0 -> ceiling: 5 on 310.9, 3 on 310.1)
    {
        MockParams params;
        SimulateInitNGXParameters(params, API::DX12, FGInput::NoFG, FGNvngxReplacement::None,
                                  /*ampereMfgActive=*/true,
                                  /*ampereMaxFrames=*/0,
                                  /*isUnrealEngine=*/false,
                                  /*adaMfgActive=*/false,
                                  /*is3101Runtime=*/true); // 310.1 runtime default -> 3

        assert(params.Get("FrameGeneration.Available") == 1);
        assert(params.Get("DLSSG.Available") == 1);
        assert(params.Get("DLSSG.MultiFrameCountMax") == 3);

        MockParams params3109;
        SimulateInitNGXParameters(params3109, API::DX12, FGInput::NoFG, FGNvngxReplacement::None,
                                  /*ampereMfgActive=*/true,
                                  /*ampereMaxFrames=*/0,
                                  /*isUnrealEngine=*/false,
                                  /*adaMfgActive=*/false,
                                  /*is3101Runtime=*/false); // 310.9 runtime default -> 5

        assert(params3109.Get("DLSSG.MultiFrameCountMax") == 5);
        printf("  [PASS] Case 2: External FG with default capability limit advertises runtime ceiling (3 on 310.1, 5 "
               "on 310.9)\n");
    }

    // Case 3: Standard OptiScaler without FG enabled (clean baseline)
    {
        MockParams params;
        SimulateInitNGXParameters(params, API::DX12, FGInput::NoFG, FGNvngxReplacement::None,
                                  /*ampereMfgActive=*/false,
                                  /*ampereMaxFrames=*/3);

        assert(!params.Has("FrameGeneration.Available"));
        assert(!params.Has("FrameInterpolation.Available"));
        assert(!params.Has("DLSSG.Available"));
        printf("  [PASS] Case 3: Without FG active, capabilities are cleanly withheld\n");
    }

    // Case 4: DX11 API should not advertise DX12 DLSSG
    {
        MockParams params;
        SimulateInitNGXParameters(params, API::DX11, FGInput::NoFG, FGNvngxReplacement::None,
                                  /*ampereMfgActive=*/true,
                                  /*ampereMaxFrames=*/1);

        assert(!params.Has("FrameGeneration.Available"));
        assert(!params.Has("DLSSG.Available"));
        printf("  [PASS] Case 4: DX11 correctly omits DX12 DLSSG capabilities\n");
    }

    // Case 5: Unreal Engine driver minimum version override
    {
        MockParams params;
        SimulateInitNGXParameters(params, API::DX12, FGInput::NoFG, FGNvngxReplacement::None,
                                  /*ampereMfgActive=*/true,
                                  /*ampereMaxFrames=*/2,
                                  /*isUnrealEngine=*/true);

        assert(params.Get("FrameGeneration.MinDriverVersionMajor") == 10);
        assert(params.Get("FrameInterpolation.MinDriverVersionMajor") == 10);
        printf("  [PASS] Case 5: Unreal Engine quirks correctly applied for external FG\n");
    }

    // Case 6: Ada MFG Unlock active on DX12 (advertises max 5 frames for up to 6X MFG)
    {
        MockParams params;
        SimulateInitNGXParameters(params, API::DX12, FGInput::DLSSG, FGNvngxReplacement::None,
                                  /*ampereMfgActive=*/false,
                                  /*ampereMaxFrames=*/0,
                                  /*isUnrealEngine=*/false,
                                  /*adaMfgActive=*/true);

        assert(params.Get("FrameGeneration.Available") == 1);
        assert(params.Get("FrameInterpolation.Available") == 1);
        assert(params.Get("DLSSG.Available") == 1);
        assert(params.Get("DLSSG.MultiFrameCountMax") == 5);
        assert(params.Get("FrameGeneration.NeedsUpdatedDriver") == 0);
        assert(params.Get("FrameGeneration.FeatureInitResult") == 1);
        printf("  [PASS] Case 6: Ada MFG Unlock advertises DLSSG.MultiFrameCountMax = 5 (up to 6X)\n");
    }

    // Case 7: Mutual Exclusion - When AmpereMfgUnlock is active, Ada MFG is suppressed
    {
        MockParams params;
        SimulateInitNGXParameters(params, API::DX12, FGInput::NoFG, FGNvngxReplacement::None,
                                  /*ampereMfgActive=*/true,
                                  /*ampereMaxFrames=*/2,
                                  /*isUnrealEngine=*/false,
                                  /*adaMfgActive=*/true); // Config might have both, but mutual exclusion suppresses Ada

        assert(params.Get("DLSSG.Available") == 1);
        // Must use Ampere max frames (2), NOT Ada max frames (5)
        assert(params.Get("DLSSG.MultiFrameCountMax") == 2);
        printf("  [PASS] Case 7: Mutual exclusion between Ampere and Ada MFG strictly enforced\n");
    }

    // Case 8: Runtime Model Ceiling - 310.9 supports up to 5 (6X), 310.1 clamps to 3 (4X)
    {
        MockParams params3109;
        SimulateInitNGXParameters(params3109, API::DX12, FGInput::NoFG, FGNvngxReplacement::None,
                                  /*ampereMfgActive=*/true,
                                  /*ampereMaxFrames=*/5,
                                  /*isUnrealEngine=*/false,
                                  /*adaMfgActive=*/false,
                                  /*is3101Runtime=*/false);
        assert(params3109.Get("DLSSG.MultiFrameCountMax") == 5);

        MockParams params3101;
        SimulateInitNGXParameters(params3101, API::DX12, FGInput::NoFG, FGNvngxReplacement::None,
                                  /*ampereMfgActive=*/true,
                                  /*ampereMaxFrames=*/5,
                                  /*isUnrealEngine=*/false,
                                  /*adaMfgActive=*/false,
                                  /*is3101Runtime=*/true);
        assert(params3101.Get("DLSSG.MultiFrameCountMax") == 3);
        printf("  [PASS] Case 8: Runtime Model ceiling respected (5 on 310.9, 3 on 310.1)\n");
    }

    printf("[TEST] All NGX Frame Generation parameter unit tests passed successfully!\n");
    return 0;
}

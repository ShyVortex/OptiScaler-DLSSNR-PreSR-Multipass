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

    void Set(const char* key, int val)
    {
        intParams[key] = val;
    }

    int Get(const char* key, int defaultVal = 0) const
    {
        auto it = intParams.find(key);
        if (it != intParams.end())
            return it->second;
        return defaultVal;
    }

    bool Has(const char* key) const
    {
        return intParams.find(key) != intParams.end();
    }
};

void SimulateInitNGXParameters(
    MockParams& params,
    API api,
    FGInput activeFgInput,
    FGNvngxReplacement activeFgNvngx,
    bool ampereMfgActive,
    int ampereMaxFrames,
    bool isUnrealEngine = false,
    bool adaMfgActive = false)
{
    // Mutual exclusion: Ada unlock is disabled if Ampere unlock is enabled
    if (ampereMfgActive)
        adaMfgActive = false;

    if ((api == API::DX12 || api == API::Vulkan) &&
        (activeFgInput == FGInput::DLSSG ||
         activeFgNvngx != FGNvngxReplacement::None ||
         ampereMfgActive || adaMfgActive))
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
            countMax = (ampereMaxFrames > 0 && ampereMaxFrames <= 3) ? ampereMaxFrames : 3;
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
        SimulateInitNGXParameters(
            params,
            API::DX12,
            FGInput::NoFG,
            FGNvngxReplacement::None,
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

    // Case 2: External FG with default 3X/4X capability (ampereMaxFrames = 3 or 0)
    {
        MockParams params;
        SimulateInitNGXParameters(
            params,
            API::DX12,
            FGInput::NoFG,
            FGNvngxReplacement::None,
            /*ampereMfgActive=*/true,
            /*ampereMaxFrames=*/0); // Runtime default -> clamped to 3

        assert(params.Get("FrameGeneration.Available") == 1);
        assert(params.Get("DLSSG.Available") == 1);
        assert(params.Get("DLSSG.MultiFrameCountMax") == 3);
        printf("  [PASS] Case 2: External FG with default capability limit advertises max 3\n");
    }

    // Case 3: Standard OptiScaler without FG enabled (clean baseline)
    {
        MockParams params;
        SimulateInitNGXParameters(
            params,
            API::DX12,
            FGInput::NoFG,
            FGNvngxReplacement::None,
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
        SimulateInitNGXParameters(
            params,
            API::DX11,
            FGInput::NoFG,
            FGNvngxReplacement::None,
            /*ampereMfgActive=*/true,
            /*ampereMaxFrames=*/1);

        assert(!params.Has("FrameGeneration.Available"));
        assert(!params.Has("DLSSG.Available"));
        printf("  [PASS] Case 4: DX11 correctly omits DX12 DLSSG capabilities\n");
    }

    // Case 5: Unreal Engine driver minimum version override
    {
        MockParams params;
        SimulateInitNGXParameters(
            params,
            API::DX12,
            FGInput::NoFG,
            FGNvngxReplacement::None,
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
        SimulateInitNGXParameters(
            params,
            API::DX12,
            FGInput::DLSSG,
            FGNvngxReplacement::None,
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
        SimulateInitNGXParameters(
            params,
            API::DX12,
            FGInput::NoFG,
            FGNvngxReplacement::None,
            /*ampereMfgActive=*/true,
            /*ampereMaxFrames=*/2,
            /*isUnrealEngine=*/false,
            /*adaMfgActive=*/true); // Config might have both, but mutual exclusion suppresses Ada

        assert(params.Get("DLSSG.Available") == 1);
        // Must use Ampere max frames (2), NOT Ada max frames (5)
        assert(params.Get("DLSSG.MultiFrameCountMax") == 2);
        printf("  [PASS] Case 7: Mutual exclusion between Ampere and Ada MFG strictly enforced\n");
    }

    printf("[TEST] All NGX Frame Generation parameter unit tests passed successfully!\n");
    return 0;
}

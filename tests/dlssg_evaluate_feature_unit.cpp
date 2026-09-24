#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <optional>

// Mock NVSDK_NGX types
enum NVSDK_NGX_Feature
{
    NVSDK_NGX_Feature_SuperSampling = 1,
    NVSDK_NGX_Feature_FrameGeneration = 2,
    NVSDK_NGX_Feature_RayReconstruction = 3
};

enum NVSDK_NGX_Result
{
    NVSDK_NGX_Result_Success = 0,
    NVSDK_NGX_Result_Fail = 1
};

struct MockParameters
{
    std::unordered_map<std::string, int> intParams;
    std::unordered_map<std::string, float> floatParams;

    NVSDK_NGX_Result Get(const char* name, int* outVal) const
    {
        auto it = intParams.find(name);
        if (it != intParams.end())
        {
            *outVal = it->second;
            return NVSDK_NGX_Result_Success;
        }
        return NVSDK_NGX_Result_Fail;
    }

    void Set(const char* name, int val) { intParams[name] = val; }

    NVSDK_NGX_Result Get(const char* name, float* outVal) const
    {
        auto it = floatParams.find(name);
        if (it != floatParams.end())
        {
            *outVal = it->second;
            return NVSDK_NGX_Result_Success;
        }
        return NVSDK_NGX_Result_Fail;
    }

    void Set(const char* name, float val) { floatParams[name] = val; }
};

// Mock Config
struct MockConfig
{
    std::optional<int> FGDLSSGOverrideInterpolationCount = std::nullopt;
};

// Mock State
struct MockState
{
    std::optional<int> dlssgMfgMax = std::nullopt;
    int dlssgDetectedInterpolationCount = 0;
};

// Mock ReflexHooks
struct MockReflexHooks
{
    static int dlssgFrameCount;
    static void setDlssgFrameCount(int count) { dlssgFrameCount = count; }
};

int MockReflexHooks::dlssgFrameCount = 0;

// Mock MfgUnlock
struct MockMfgUnlock
{
    static bool enabledForSession;
    static bool EnabledForSession() { return enabledForSession; }
};

bool MockMfgUnlock::enabledForSession = false;

// Simulates the evaluate logic from NVNGX_DLSS_Dx12.cpp
void SimulateEvaluateFeature(NVSDK_NGX_Feature feature, MockParameters* inParameters, MockState& state,
                             const MockConfig& cfg)
{
    if (feature == NVSDK_NGX_Feature_FrameGeneration)
    {
        int frameCount = 0;
        inParameters->Get("DLSSG.MultiFrameCount", &frameCount);

        if (MockMfgUnlock::EnabledForSession() && !state.dlssgMfgMax.has_value())
            state.dlssgMfgMax = 5;

        if (cfg.FGDLSSGOverrideInterpolationCount.has_value())
        {
            frameCount = cfg.FGDLSSGOverrideInterpolationCount.value();
            inParameters->Set("DLSSG.MultiFrameCount", frameCount);
        }
        else if (frameCount <= 0)
        {
            frameCount = 1;
            inParameters->Set("DLSSG.MultiFrameCount", frameCount);
        }

        state.dlssgDetectedInterpolationCount = frameCount;
        MockReflexHooks::setDlssgFrameCount(frameCount);
    }
}

int main()
{
    printf("=== Running DLSSG Evaluate Feature Unit Tests ===\n");

    // Case 1: Override 3X (interpolation count = 2) with Ada MFG active
    {
        MockParameters params;
        params.Set("DLSSG.MultiFrameCount", 1); // Game initially passes 1 (2X)

        MockConfig cfg;
        cfg.FGDLSSGOverrideInterpolationCount = 2; // User requested 3X

        MockState state;
        MockMfgUnlock::enabledForSession = true;
        MockReflexHooks::dlssgFrameCount = 0;

        SimulateEvaluateFeature(NVSDK_NGX_Feature_FrameGeneration, &params, state, cfg);

        int updatedCount = 0;
        assert(params.Get("DLSSG.MultiFrameCount", &updatedCount) == NVSDK_NGX_Result_Success);
        assert(updatedCount == 2);
        assert(MockReflexHooks::dlssgFrameCount == 2);
        assert(state.dlssgDetectedInterpolationCount == 2);
        assert(state.dlssgMfgMax.has_value() && state.dlssgMfgMax.value() == 5);
        printf("  [PASS] Case 1: Override 3X propagates DLSSG.MultiFrameCount=2 to NGX and Reflex\n");
    }

    // Case 2: Override 4X (interpolation count = 3) with Ada MFG active
    {
        MockParameters params;
        params.Set("DLSSG.MultiFrameCount", 1);

        MockConfig cfg;
        cfg.FGDLSSGOverrideInterpolationCount = 3; // 4X

        MockState state;
        MockMfgUnlock::enabledForSession = true;
        MockReflexHooks::dlssgFrameCount = 0;

        SimulateEvaluateFeature(NVSDK_NGX_Feature_FrameGeneration, &params, state, cfg);

        int updatedCount = 0;
        assert(params.Get("DLSSG.MultiFrameCount", &updatedCount) == NVSDK_NGX_Result_Success);
        assert(updatedCount == 3);
        assert(MockReflexHooks::dlssgFrameCount == 3);
        assert(state.dlssgDetectedInterpolationCount == 3);
        printf("  [PASS] Case 2: Override 4X propagates DLSSG.MultiFrameCount=3 to NGX and Reflex\n");
    }

    // Case 3: Override Off (interpolation count = 0)
    {
        MockParameters params;
        params.Set("DLSSG.MultiFrameCount", 1);

        MockConfig cfg;
        cfg.FGDLSSGOverrideInterpolationCount = 0; // Off

        MockState state;
        MockMfgUnlock::enabledForSession = true;
        MockReflexHooks::dlssgFrameCount = 1;

        SimulateEvaluateFeature(NVSDK_NGX_Feature_FrameGeneration, &params, state, cfg);

        int updatedCount = 0;
        assert(params.Get("DLSSG.MultiFrameCount", &updatedCount) == NVSDK_NGX_Result_Success);
        assert(updatedCount == 0);
        assert(MockReflexHooks::dlssgFrameCount == 0);
        assert(state.dlssgDetectedInterpolationCount == 0);
        printf("  [PASS] Case 3: Override Off sets DLSSG.MultiFrameCount=0 and shuts down Reflex pacing\n");
    }

    // Case 4: No override (Default) preserves game-provided parameters
    {
        MockParameters params;
        params.Set("DLSSG.MultiFrameCount", 1); // Game requested 1

        MockConfig cfg; // nullopt

        MockState state;
        MockMfgUnlock::enabledForSession = true;
        MockReflexHooks::dlssgFrameCount = 0;

        SimulateEvaluateFeature(NVSDK_NGX_Feature_FrameGeneration, &params, state, cfg);

        int count = 0;
        assert(params.Get("DLSSG.MultiFrameCount", &count) == NVSDK_NGX_Result_Success);
        assert(count == 1);
        assert(MockReflexHooks::dlssgFrameCount == 1);
        assert(state.dlssgDetectedInterpolationCount == 1);
        printf("  [PASS] Case 4: Default preserves game-provided parameters\n");
    }

    // Case 5: Non-FG feature does not modify DLSSG parameters
    {
        MockParameters params;
        params.Set("DLSSG.MultiFrameCount", 1);

        MockConfig cfg;
        cfg.FGDLSSGOverrideInterpolationCount = 3;

        MockState state;
        MockMfgUnlock::enabledForSession = true;
        MockReflexHooks::dlssgFrameCount = 0;

        SimulateEvaluateFeature(NVSDK_NGX_Feature_SuperSampling, &params, state, cfg);

        int count = 0;
        assert(params.Get("DLSSG.MultiFrameCount", &count) == NVSDK_NGX_Result_Success);
        assert(count == 1);                            // Unchanged!
        assert(MockReflexHooks::dlssgFrameCount == 0); // Unchanged!
        printf("  [PASS] Case 5: SuperSampling evaluation leaves DLSSG parameters untouched\n");
    }

    // Case 6: Unpopulated MultiFrameCount defaults to 1 (2X FG)
    {
        MockParameters params; // No DLSSG.MultiFrameCount set
        MockConfig cfg;        // No override set

        MockState state;
        MockMfgUnlock::enabledForSession = true;
        MockReflexHooks::dlssgFrameCount = 0;

        SimulateEvaluateFeature(NVSDK_NGX_Feature_FrameGeneration, &params, state, cfg);

        int count = 0;
        assert(params.Get("DLSSG.MultiFrameCount", &count) == NVSDK_NGX_Result_Success);
        assert(count == 1 && "Unpopulated MultiFrameCount must default to 1");
        assert(MockReflexHooks::dlssgFrameCount == 1);
        assert(state.dlssgDetectedInterpolationCount == 1);
        printf("  [PASS] Case 6: Unpopulated MultiFrameCount automatically defaults to 1 (2X FG)\n");
    }

    printf("=== All DLSSG Evaluate Feature Unit Tests PASSED! ===\n");
    return 0;
}

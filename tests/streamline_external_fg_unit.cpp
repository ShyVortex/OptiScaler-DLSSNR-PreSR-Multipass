#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace sl
{
using Feature = uint32_t;
constexpr Feature kFeatureDLSS_G = 1000;
constexpr Feature kFeatureReflex = 1001;

enum class Result
{
    eOk = 0,
    eErrorFeatureNotSupported = -1,
    eErrorInvalidParameter = -2
};

struct FeatureVersion
{
    uint32_t major, minor, patch;
};
} // namespace sl

enum class FGInput
{
    NoFG,
    Upscaler,
    NvngxFG,
    DLSSG,
    FSRFG,
    FSRFG30
};

// Fake original function pointers
static void* const REAL_DLSSG_GET_STATE = reinterpret_cast<void*>(0x5001);
static void* const REAL_DLSSG_SET_OPTIONS = reinterpret_cast<void*>(0x5002);
static void* const DUMMY_DLSSG_GET_STATE = reinterpret_cast<void*>(0x6001);
static void* const DUMMY_DLSSG_SET_OPTIONS = reinterpret_cast<void*>(0x6002);

sl::Result MockOriginalGetFeatureFunction(sl::Feature feature, const char* name, void*& func)
{
    if (feature == sl::kFeatureDLSS_G)
    {
        if (strcmp(name, "slDLSSGGetState") == 0)
        {
            func = REAL_DLSSG_GET_STATE;
            return sl::Result::eOk;
        }
        if (strcmp(name, "slDLSSGSetOptions") == 0)
        {
            func = REAL_DLSSG_SET_OPTIONS;
            return sl::Result::eOk;
        }
    }
    return sl::Result::eErrorFeatureNotSupported;
}

sl::Result SimulatedHkslGetFeatureFunction(bool externalFrameGeneration, sl::Feature feature, const char* functionName,
                                           void*& function)
{
    if (!externalFrameGeneration && feature == sl::kFeatureDLSS_G)
    {
        if (strcmp(functionName, "slDLSSGSetOptions") == 0)
        {
            function = DUMMY_DLSSG_SET_OPTIONS;
            return sl::Result::eOk;
        }

        if (strcmp(functionName, "slDLSSGGetState") == 0)
        {
            function = DUMMY_DLSSG_GET_STATE;
            return sl::Result::eOk;
        }
    }

    return MockOriginalGetFeatureFunction(feature, functionName, function);
}

bool ShouldAttachStreamlineDlssgHooks(FGInput activeFgInput, bool ampereMfgActive)
{
    return (activeFgInput == FGInput::DLSSG || ampereMfgActive);
}

sl::Result SimulatedHkslIsFeatureSupported(sl::Feature feature)
{
    if (feature == sl::kFeatureDLSS_G)
        return sl::Result::eOk;

    return sl::Result::eErrorFeatureNotSupported;
}

sl::Result SimulatedHkslIsFeatureLoaded(sl::Feature feature, bool& loaded)
{
    if (feature == sl::kFeatureDLSS_G)
    {
        loaded = true;
        return sl::Result::eOk;
    }

    loaded = false;
    return sl::Result::eErrorFeatureNotSupported;
}

int main()
{
    printf("[TEST] Running Streamline External FG hook unit tests...\n");

    // Case 1: External FG with AmpereMfgUnlock (activeFgInput is NoFG, but ampereMfgActive is true)
    {
        bool shouldAttach = ShouldAttachStreamlineDlssgHooks(FGInput::NoFG, /*ampereMfgActive=*/true);
        assert(shouldAttach == true);
        printf("  [PASS] Case 1: Streamline DLSSG capability hooks are attached for External FG\n");
    }

    // Case 2: Feature query returns eOk for DLSSG when hooked
    {
        assert(SimulatedHkslIsFeatureSupported(sl::kFeatureDLSS_G) == sl::Result::eOk);
        bool loaded = false;
        assert(SimulatedHkslIsFeatureLoaded(sl::kFeatureDLSS_G, loaded) == sl::Result::eOk);
        assert(loaded == true);
        printf("  [PASS] Case 2: DLSSG feature is reported supported and loaded\n");
    }

    // Case 3: External FG delegates getFeatureFunction to real sl.dlss_g.dll
    {
        void* getStateFunc = nullptr;
        void* setOptionsFunc = nullptr;
        sl::Result r1 = SimulatedHkslGetFeatureFunction(
            /*externalFrameGeneration=*/true, sl::kFeatureDLSS_G, "slDLSSGGetState", getStateFunc);
        assert(r1 == sl::Result::eOk);
        assert(getStateFunc == REAL_DLSSG_GET_STATE);

        sl::Result r2 = SimulatedHkslGetFeatureFunction(
            /*externalFrameGeneration=*/true, sl::kFeatureDLSS_G, "slDLSSGSetOptions", setOptionsFunc);
        assert(r2 == sl::Result::eOk);
        assert(setOptionsFunc == REAL_DLSSG_SET_OPTIONS);
        printf("  [PASS] Case 3: External FG routes functions to real sl.dlss_g runtime, not dummy\n");
    }

    // Case 4: Internal DLSSG emulation uses dummy placeholders
    {
        void* getStateFunc = nullptr;
        void* setOptionsFunc = nullptr;
        sl::Result r1 = SimulatedHkslGetFeatureFunction(
            /*externalFrameGeneration=*/false, sl::kFeatureDLSS_G, "slDLSSGGetState", getStateFunc);
        assert(r1 == sl::Result::eOk);
        assert(getStateFunc == DUMMY_DLSSG_GET_STATE);

        sl::Result r2 = SimulatedHkslGetFeatureFunction(
            /*externalFrameGeneration=*/false, sl::kFeatureDLSS_G, "slDLSSGSetOptions", setOptionsFunc);
        assert(r2 == sl::Result::eOk);
        assert(setOptionsFunc == DUMMY_DLSSG_SET_OPTIONS);
        printf("  [PASS] Case 4: Internal DLSSG emulation continues to use internal dummies\n");
    }

    // Case 5: Standard OptiScaler without FG does not attach DLSSG hooks
    {
        bool shouldAttach = ShouldAttachStreamlineDlssgHooks(FGInput::NoFG, /*ampereMfgActive=*/false);
        assert(shouldAttach == false);
        printf("  [PASS] Case 5: When FG is disabled, hooks are not attached\n");
    }

    printf("[TEST] All Streamline External FG hook unit tests passed successfully!\n");
    return 0;
}

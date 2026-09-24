#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

#include "../OptiScaler/framegen/dlssg/AmpereMfgLoader.h"

namespace sl
{
enum Feature
{
    kFeatureDLSS = 0,
    kFeatureDLSS_G = 1000,
    kFeatureDLSS_RR = 1001,
    kFeatureReflex = 2,
    kFeaturePCL = 3
};

enum Result
{
    eOk = 0,
    eErrorFeatureMissing = 1,
    eErrorInvalidParameter = 2
};

enum Boolean
{
    eFalse = 0,
    eTrue = 1
};

struct ViewportHandle
{
    uint32_t id = 0;
};

struct DLSSGOptions
{
    uint32_t flags = 0;
};

struct DLSSGState
{
    uint32_t structVersion = 1;
    uint32_t numFramesActuallyPresented = 0;
    uint64_t estimatedVRAMUsageInBytes = 0;
    uint32_t numFramesToGenerateMax = 0;
    Boolean bReserved4 = eFalse;
    Boolean bIsVsyncSupportAvailable = eFalse;
    uint64_t inputsProcessingCompletionFence = 0;
    uint64_t lastPresentInputsProcessingCompletionFenceValue = 0;
    Boolean bIsDynamicMFGSupported = eFalse;
};
} // namespace sl

enum class FGInput
{
    NoFG = 0,
    OptiScaler = 1,
    DLSSG = 2,
    NvngxFG = 3
};

struct MockConfig
{
    std::optional<bool> FGDLSSGAmpereMfgUnlock = false;
    std::optional<bool> FGDLSSGOverrideForceDMFG = false;
    std::optional<bool> FGDLSSGForceDMFG = false;
};

struct MockState
{
    FGInput activeFgInput = FGInput::NoFG;
    bool externalFrameGeneration = false;
};

bool ShouldBypassHookDlssg(const MockState& state, const MockConfig& config)
{
    const bool ampereMfgActive = config.FGDLSSGAmpereMfgUnlock.value_or(false);
    return state.externalFrameGeneration && !ampereMfgActive;
}

struct MockPluginJson
{
    bool hasVsyncSupported = true;
    bool vsyncSupported = false;
    bool hasHwsRequired = true;
    bool hwsRequired = true;
};

void PatchPluginJsonRequirements(MockPluginJson& json, const MockState& state, const MockConfig& config)
{
    const bool ampereMfgActive = config.FGDLSSGAmpereMfgUnlock.value_or(false);
    if (state.activeFgInput == FGInput::DLSSG || state.activeFgInput == FGInput::NvngxFG || ampereMfgActive)
    {
        if (json.hasVsyncSupported)
            json.vsyncSupported = true;
        if (json.hasHwsRequired)
            json.hwsRequired = false;
    }
}

static sl::Result dummy_slDLSSGGetState(const sl::ViewportHandle& viewport, sl::DLSSGState& state,
                                        const sl::DLSSGOptions* options, const MockConfig& config)
{
    state.numFramesActuallyPresented = 1;
    if (state.structVersion >= 2)
    {
        state.numFramesToGenerateMax = 1;
        state.bIsVsyncSupportAvailable = sl::Boolean::eTrue;
    }
    if (state.structVersion >= 4)
    {
        const bool dynamicMfg =
            config.FGDLSSGOverrideForceDMFG.value_or(false) || config.FGDLSSGForceDMFG.value_or(false);
        if (dynamicMfg)
            state.bIsDynamicMFGSupported = sl::Boolean::eTrue;
    }
    state.estimatedVRAMUsageInBytes = 300 * 1024 * 1024;
    return sl::Result::eOk;
}

static sl::Result dummy_slDLSSGSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options)
{
    return sl::Result::eOk;
}

sl::Result Mock_hkslGetFeatureFunction(sl::Feature feature, const char* functionName, void*& function,
                                       const MockState& state, const MockConfig& config, void* underlyingFunction,
                                       sl::Result underlyingResult)
{
    if (!state.externalFrameGeneration && feature == sl::kFeatureDLSS_G)
    {
        if (std::strcmp(functionName, "slDLSSGSetOptions") == 0)
        {
            function = reinterpret_cast<void*>(&dummy_slDLSSGSetOptions);
            return sl::Result::eOk;
        }
        if (std::strcmp(functionName, "slDLSSGGetState") == 0)
        {
            function = reinterpret_cast<void*>(&dummy_slDLSSGGetState);
            return sl::Result::eOk;
        }
    }

    function = underlyingFunction;
    auto result = underlyingResult;

    if (feature == sl::kFeatureDLSS_G && (result != sl::Result::eOk || function == nullptr))
    {
        if (std::strcmp(functionName, "slDLSSGSetOptions") == 0)
        {
            function = reinterpret_cast<void*>(&dummy_slDLSSGSetOptions);
            return sl::Result::eOk;
        }
        if (std::strcmp(functionName, "slDLSSGGetState") == 0)
        {
            function = reinterpret_cast<void*>(&dummy_slDLSSGGetState);
            return sl::Result::eOk;
        }
    }

    return result;
}

int main()
{
    std::puts("Running Streamline External MFG Hooks and Linux HWS Compatibility tests...");

    // Test 1: HookDlssg gating logic
    {
        MockConfig config;
        MockState state;

        // Baseline: Internal FG mode (external == false) -> should NOT bypass
        state.externalFrameGeneration = false;
        config.FGDLSSGAmpereMfgUnlock = false;
        assert(!ShouldBypassHookDlssg(state, config) && "Internal FG mode must not bypass hookDlssg");

        // Plain external FG (external == true, ampere == false) -> MUST bypass
        state.externalFrameGeneration = true;
        config.FGDLSSGAmpereMfgUnlock = false;
        assert(ShouldBypassHookDlssg(state, config) && "Plain external FG must bypass hookDlssg");

        // External FG with Ampere/Turing MFG active -> MUST NOT bypass
        state.externalFrameGeneration = true;
        config.FGDLSSGAmpereMfgUnlock = true;
        assert(!ShouldBypassHookDlssg(state, config) &&
               "Ampere MFG active in external mode MUST NOT bypass hookDlssg!");

        std::puts("  [PASS] Test 1: hookDlssg gating correctly allows Ampere/Turing MFG in external mode");
    }

    // Test 2: Plugin JSON HWS and VSync requirement stripping
    {
        MockConfig config;
        MockState state;
        MockPluginJson json;

        // Scenario A: External FG with Ampere MFG active (activeFgInput is NoFG)
        state.activeFgInput = FGInput::NoFG;
        state.externalFrameGeneration = true;
        config.FGDLSSGAmpereMfgUnlock = true;

        json.hwsRequired = true;
        json.vsyncSupported = false;

        PatchPluginJsonRequirements(json, state, config);

        assert(!json.hwsRequired &&
               "ampereMfgActive MUST strip /external/hws/required to false for Linux/Proton compatibility!");
        assert(json.vsyncSupported && "ampereMfgActive MUST set /vsync/supported to true!");

        // Scenario B: Plain external mode without Ampere MFG
        config.FGDLSSGAmpereMfgUnlock = false;
        json.hwsRequired = true;
        json.vsyncSupported = false;

        PatchPluginJsonRequirements(json, state, config);
        assert(json.hwsRequired && "Plain external without Ampere MFG must not alter hwsRequired");
        assert(!json.vsyncSupported && "Plain external without Ampere MFG must not alter vsyncSupported");

        // Scenario C: Internal FG with DLSSG input
        state.activeFgInput = FGInput::DLSSG;
        json.hwsRequired = true;
        json.vsyncSupported = false;

        PatchPluginJsonRequirements(json, state, config);
        assert(!json.hwsRequired && "DLSSG input must strip /external/hws/required");
        assert(json.vsyncSupported && "DLSSG input must set /vsync/supported to true");

        std::puts("  [PASS] Test 2: Plugin JSON requirements correctly patched when ampereMfgActive is true");
    }

    // Test 3: hkslGetFeatureFunction resolution and fallback
    {
        MockConfig config;
        MockState state;
        void* resolvedFunction = nullptr;

        // Subtest A: Internal FG mode returns dummy functions directly
        state.externalFrameGeneration = false;
        sl::Result res = Mock_hkslGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", resolvedFunction, state,
                                                     config, nullptr, sl::Result::eErrorFeatureMissing);
        assert(res == sl::Result::eOk && "Internal FG must return eOk");
        assert(resolvedFunction == reinterpret_cast<void*>(&dummy_slDLSSGGetState) &&
               "Internal FG must return dummy_slDLSSGGetState");

        // Subtest B: External FG mode with valid plugin function returns underlying pointer
        state.externalFrameGeneration = true;
        int dummyUnderlyingPlugin = 42;
        resolvedFunction = nullptr;
        res = Mock_hkslGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", resolvedFunction, state, config,
                                          &dummyUnderlyingPlugin, sl::Result::eOk);
        assert(res == sl::Result::eOk && "External FG with valid plugin must return eOk");
        assert(resolvedFunction == &dummyUnderlyingPlugin && "External FG must return underlying plugin pointer");

        // Subtest C: External FG mode where underlying plugin is null/missing -> falls back to dummy function
        resolvedFunction = nullptr;
        res = Mock_hkslGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", resolvedFunction, state, config,
                                          nullptr, sl::Result::eErrorFeatureMissing);
        assert(res == sl::Result::eOk && "Missing plugin in external FG must fall back to eOk");
        assert(resolvedFunction == reinterpret_cast<void*>(&dummy_slDLSSGGetState) &&
               "Missing plugin in external FG must fall back to dummy_slDLSSGGetState");

        // Subtest D: External FG mode with slDLSSGSetOptions missing -> falls back to dummy function
        resolvedFunction = nullptr;
        res = Mock_hkslGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", resolvedFunction, state, config,
                                          nullptr, sl::Result::eErrorFeatureMissing);
        assert(res == sl::Result::eOk && "Missing plugin in external FG must fall back to eOk for SetOptions");
        assert(resolvedFunction == reinterpret_cast<void*>(&dummy_slDLSSGSetOptions) &&
               "Missing plugin in external FG must fall back to dummy_slDLSSGSetOptions");

        std::puts("  [PASS] Test 3: hkslGetFeatureFunction gracefully falls back when plugin functions are missing");
    }

    // Test 4: dummy_slDLSSGGetState structVersion 4 Dynamic MFG reporting
    {
        MockConfig config;
        sl::ViewportHandle vp;
        sl::DLSSGState stateV4;
        stateV4.structVersion = 4;

        // When DMFG is disabled
        config.FGDLSSGOverrideForceDMFG = false;
        config.FGDLSSGForceDMFG = false;
        dummy_slDLSSGGetState(vp, stateV4, nullptr, config);
        assert(stateV4.bIsDynamicMFGSupported == sl::eFalse && "DMFG supported must be false when config is false");

        // When DMFG is enabled via OverrideForceDMFG
        config.FGDLSSGOverrideForceDMFG = true;
        dummy_slDLSSGGetState(vp, stateV4, nullptr, config);
        assert(stateV4.bIsDynamicMFGSupported == sl::eTrue &&
               "DMFG supported must be true when OverrideForceDMFG is true");

        // When DMFG is enabled via ForceDMFG
        config.FGDLSSGOverrideForceDMFG = false;
        config.FGDLSSGForceDMFG = true;
        dummy_slDLSSGGetState(vp, stateV4, nullptr, config);
        assert(stateV4.bIsDynamicMFGSupported == sl::eTrue && "DMFG supported must be true when ForceDMFG is true");

        std::puts("  [PASS] Test 4: dummy_slDLSSGGetState correctly advertises Dynamic MFG on structVersion >= 4");
    }

    // Test 5: TryResolveDrsMultiFrameSetting with Dynamic MFG
    {
        uint32_t outValue = 0;
        constexpr uint32_t DRS_OVERRIDE_DLSSG_MULTI_FRAME_COUNT_ID = 0x104D6667;
        constexpr uint32_t DRS_OVERRIDE_MAX_DLSSG_DYNAMIC_MULTI_FRAME_COUNT_ID = 0x10562D0F;

        // Subtest A: Dynamic MFG enabled with configuredMaxFrames == 1
        // Must advertise maxCeiling (5) for dynamic ceiling override, so Streamline shows DMFG in settings
        outValue = 0;
        bool res = AmpereMfgLoader::TryResolveDrsMultiFrameSetting(
            DRS_OVERRIDE_MAX_DLSSG_DYNAMIC_MULTI_FRAME_COUNT_ID,
            /*configuredMaxFrames=*/1, /*onLinux=*/true, /*mfgUnlockEnabled=*/true, outValue, /*maxCeiling=*/5,
            /*explicitOverrideCount=*/0, /*dynamicMfg=*/true);
        assert(res && "DMFG dynamic ceiling query must succeed even when configuredMaxFrames == 1");
        assert(outValue == 5 && "DMFG dynamic ceiling must report maxCeiling (5)");

        // Subtest B: Dynamic MFG enabled with configuredMaxFrames == 1 for static multiplier query
        // Must NOT force static 1 when Dynamic MFG is enabled, allowing dynamic pacing to manage frames
        outValue = 0;
        res = AmpereMfgLoader::TryResolveDrsMultiFrameSetting(DRS_OVERRIDE_DLSSG_MULTI_FRAME_COUNT_ID,
                                                              /*configuredMaxFrames=*/1, /*onLinux=*/true,
                                                              /*mfgUnlockEnabled=*/true, outValue, /*maxCeiling=*/5,
                                                              /*explicitOverrideCount=*/0, /*dynamicMfg=*/true);
        assert(!res && "DMFG must not force static multiplier == 1 when dynamicMfg is active");

        // Subtest C: Static MFG mode with configuredMaxFrames == 1
        // Dynamic ceiling override must return false (Streamline disables DMFG)
        outValue = 0;
        res = AmpereMfgLoader::TryResolveDrsMultiFrameSetting(DRS_OVERRIDE_MAX_DLSSG_DYNAMIC_MULTI_FRAME_COUNT_ID,
                                                              /*configuredMaxFrames=*/1, /*onLinux=*/true,
                                                              /*mfgUnlockEnabled=*/true, outValue, /*maxCeiling=*/5,
                                                              /*explicitOverrideCount=*/0, /*dynamicMfg=*/false);
        assert(!res && "Static MFG with maxFrames == 1 must return false for dynamic ceiling query");

        // Static multiplier query must force 1 for Linux 2X FG elevation workaround
        outValue = 0;
        res = AmpereMfgLoader::TryResolveDrsMultiFrameSetting(DRS_OVERRIDE_DLSSG_MULTI_FRAME_COUNT_ID,
                                                              /*configuredMaxFrames=*/1, /*onLinux=*/true,
                                                              /*mfgUnlockEnabled=*/true, outValue, /*maxCeiling=*/5,
                                                              /*explicitOverrideCount=*/0, /*dynamicMfg=*/false);
        assert(res && "Static MFG with maxFrames == 1 must force static multiplier == 1");
        assert(outValue == 1 && "Static multiplier must report 1");

        // Subtest D: Explicit override takes precedence over dynamicMfg
        outValue = 0;
        res = AmpereMfgLoader::TryResolveDrsMultiFrameSetting(DRS_OVERRIDE_DLSSG_MULTI_FRAME_COUNT_ID,
                                                              /*configuredMaxFrames=*/1, /*onLinux=*/true,
                                                              /*mfgUnlockEnabled=*/true, outValue, /*maxCeiling=*/5,
                                                              /*explicitOverrideCount=*/4, /*dynamicMfg=*/true);
        assert(res && "Explicit override must succeed");
        assert(outValue == 4 && "Explicit override must clamp to requested value");

        std::puts("  [PASS] Test 5: TryResolveDrsMultiFrameSetting correctly handles Dynamic MFG ceiling and static "
                  "multiplier overrides");
    }

    std::puts("All Streamline External MFG Hooks and Linux HWS Compatibility tests passed successfully!");
    return 0;
}

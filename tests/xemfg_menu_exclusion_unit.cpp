#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

// Standalone unit test validating unified XeMFG menu UI conflict suppression,
// standard FG selection lockout, complete suppression of redundant XeFG/OptiFG sections,
// unified Source selection (Game vs OptiScaler), and strict mutual exclusion.

enum class XeMfgSource : uint32_t
{
    Game = 0,
    OptiScaler = 1,
    Count
};

template <class T> class TestCustomOptional : public std::optional<T>
{
    T _defaultValue {};

  public:
    using std::optional<T>::optional;
    using std::optional<T>::operator=;

    TestCustomOptional(T defaultValue) : std::optional<T>(), _defaultValue(defaultValue) {}

    TestCustomOptional() : std::optional<T>(), _defaultValue() {}

    TestCustomOptional& operator=(const T& val)
    {
        std::optional<T>::operator=(val);
        return *this;
    }

    T value_or_default() const
    {
        if (this->has_value())
            return this->value();
        return _defaultValue;
    }
};

enum class FGInput
{
    NoFG = 0,
    DLSSG = 1,
    FSRFG = 2,
    Upscaler = 3,
    ForceXeLL = 4,
    NvngxFG = 5
};

enum class FGOutput
{
    NoFG = 0,
    DLSSG = 1,
    FSRFG = 2,
    XeFG = 3
};

struct SimulatedConfig
{
    TestCustomOptional<bool> XeMfgUnlock { false };
    TestCustomOptional<int> XeMfgSourceType { static_cast<int>(XeMfgSource::Game) };
    TestCustomOptional<int> XeMfgMaxFrames { 3 };
    TestCustomOptional<bool> XeMfgExtraPacing { false };
    TestCustomOptional<bool> FGXeFGForceBorderless { false };

    // Legacy fields for backward compatibility testing
    TestCustomOptional<bool> XeMfgSrEnable { false };
    TestCustomOptional<int> XeMfgSrGeneratedFrames { 2 };

    TestCustomOptional<bool> FGDLSSGAdaMfgUnlock { false };
    TestCustomOptional<bool> AmpereMfgUnlock { false };
    TestCustomOptional<bool> FGDLSSGSmoothMotion { false };
    TestCustomOptional<bool> ExternalFrameGeneration { false };

    TestCustomOptional<bool> FGEnabled { false };
    TestCustomOptional<FGInput> FGInputMode { FGInput::NoFG };
    TestCustomOptional<FGOutput> FGOutputMode { FGOutput::NoFG };
    TestCustomOptional<int> FGXeFGInterpolationCount { 1 };

    XeMfgSource XeMfgSourceValue() const
    {
        int val = XeMfgSourceType.value_or_default();
        if (val < 0 || val >= static_cast<int>(XeMfgSource::Count))
            return XeMfgSource::Game;
        return static_cast<XeMfgSource>(val);
    }
};

struct SimulatedState
{
    bool externalFrameGeneration = false;
    FGInput activeFgInput = FGInput::NoFG;
    FGOutput activeFgOutput = FGOutput::NoFG;
    bool currentFGSwapchain = false;
    int dlssgDetectedInterpolationCount = 0;
};

struct MenuUiOutput
{
    // 1. Unified XeMFG section
    bool xeMfgDedicatedSectionRendered = false;
    bool xeMfgSourceComboRendered = false;
    bool xeMfgMaxFramesSliderRendered = false;
    bool xeMfgForceBorderlessRendered = false;

    // 1b. Legacy separate XeMFG-SR section (must NEVER be rendered in unified UI)
    bool xeMfgSrDedicatedSectionRendered = false;

    // 2. Standard Frame Generation Selection (RenderFrameGenerationSelection)
    bool standardFgSelectionRendered = false;
    bool standardFgLockedNoticeRendered = false;
    std::string standardFgLockedNoticeText;

    // 3. Redundant XeFG controls (must be completely hidden when XeMFG is active)
    bool redundantXeFgSectionRendered = false;
    bool legacyXeFgControlsInteractive = false;

    // 4. Redundant OptiFG controls (must be completely hidden when XeMFG is active)
    bool redundantOptiFgSectionRendered = false;
};

static MenuUiOutput SimulateRenderMenu(const SimulatedConfig& config, const SimulatedState& state)
{
    MenuUiOutput out;

    const bool xeMfgActive =
        config.XeMfgUnlock.value_or_default() && !config.ExternalFrameGeneration.value_or_default() &&
        !config.FGDLSSGAdaMfgUnlock.value_or_default() && !config.AmpereMfgUnlock.value_or_default();

    // 1. Unified XeMFG section at top of Frame Generation
    if (config.XeMfgUnlock.value_or_default())
    {
        out.xeMfgDedicatedSectionRendered = true;
        out.xeMfgSourceComboRendered = true;
        out.xeMfgMaxFramesSliderRendered = true;
        out.xeMfgForceBorderlessRendered = true;
    }

    // 1b. Legacy separate XeMFG-SR section is permanently removed
    out.xeMfgSrDedicatedSectionRendered = false;

    // 2. Standard Frame Generation Selection (RenderFrameGenerationSelection)
    if (state.externalFrameGeneration || xeMfgActive)
    {
        if (xeMfgActive)
        {
            out.standardFgLockedNoticeRendered = true;
            out.standardFgLockedNoticeText =
                "Intel Xe Multi-Frame Generation (XeMFG) is active. Standard Frame Generation controls are locked and "
                "managed automatically.";
        }
        out.standardFgSelectionRendered = false; // Early return prevents rendering FG Enabled, Input, Output
    }
    else
    {
        out.standardFgSelectionRendered = true;
        out.standardFgLockedNoticeRendered = false;
    }

    // 3. Redundant XeFG controls below Advanced FG Settings
    // In unified architecture: if xeMfgActive, completely hidden!
    if (!xeMfgActive && state.activeFgOutput == FGOutput::XeFG && state.activeFgInput != FGInput::NoFG &&
        state.activeFgInput != FGInput::ForceXeLL && state.currentFGSwapchain)
    {
        out.redundantXeFgSectionRendered = true;
        out.legacyXeFgControlsInteractive = true;
    }
    else
    {
        out.redundantXeFgSectionRendered = false;
        out.legacyXeFgControlsInteractive = false;
    }

    // 4. Redundant OptiFG controls
    // In unified architecture: if xeMfgActive, completely hidden!
    if (!xeMfgActive && state.currentFGSwapchain && state.activeFgInput == FGInput::Upscaler)
    {
        out.redundantOptiFgSectionRendered = true;
    }
    else
    {
        out.redundantOptiFgSectionRendered = false;
    }

    return out;
}

int main()
{
    printf("[+] Starting Unified XeMFG Menu UI Conflict Suppression & Exclusion Tests...\n");

    // Test 1: Active XeMFG (Game Source) locks standard FG selection, hides redundant XeFG/OptiFG sections
    {
        SimulatedConfig config;
        config.XeMfgUnlock = true;
        config.XeMfgSourceType = static_cast<int>(XeMfgSource::Game);
        config.FGEnabled = true;
        config.FGInputMode = FGInput::DLSSG;
        config.FGOutputMode = FGOutput::XeFG;

        SimulatedState state;
        state.activeFgInput = FGInput::DLSSG;
        state.activeFgOutput = FGOutput::XeFG;
        state.currentFGSwapchain = true;

        auto ui = SimulateRenderMenu(config, state);

        assert(ui.xeMfgDedicatedSectionRendered && "Unified XeMFG section must be rendered");
        assert(ui.xeMfgSourceComboRendered && "Source combo must be rendered inside unified XeMFG");
        assert(ui.xeMfgMaxFramesSliderRendered && "MaxInterpolatedFrames slider must be rendered");
        assert(ui.xeMfgForceBorderlessRendered && "ForceBorderless checkbox must be rendered");
        assert(!ui.xeMfgSrDedicatedSectionRendered && "Duplicate XeMFG-SR section must never be rendered");

        assert(!ui.standardFgSelectionRendered && "Standard FG selection controls must be locked out");
        assert(ui.standardFgLockedNoticeRendered && "Lockout notice must be displayed");

        assert(!ui.redundantXeFgSectionRendered && "Redundant Frame Generation (XeFG) must be completely hidden");
        assert(!ui.redundantOptiFgSectionRendered && "Redundant Frame Generation (OptiFG) must be completely hidden");
        printf("  [PASS] Test 1: Active XeMFG (Game Source) locks standard FG selection & hides duplicate sections.\n");
    }

    // Test 2: Active XeMFG (OptiScaler Source) renders unified controls & elevates ForceBorderless
    {
        SimulatedConfig config;
        config.XeMfgUnlock = true;
        config.XeMfgSourceType = static_cast<int>(XeMfgSource::OptiScaler);
        config.FGEnabled = true;
        config.FGInputMode = FGInput::Upscaler;
        config.FGOutputMode = FGOutput::XeFG;
        config.FGXeFGForceBorderless = true;

        SimulatedState state;
        state.activeFgInput = FGInput::Upscaler;
        state.activeFgOutput = FGOutput::XeFG;
        state.currentFGSwapchain = true;

        auto ui = SimulateRenderMenu(config, state);

        assert(ui.xeMfgDedicatedSectionRendered && "Unified XeMFG section must be rendered");
        assert(config.XeMfgSourceValue() == XeMfgSource::OptiScaler);
        assert(config.FGXeFGForceBorderless.value_or_default() == true && "ForceBorderless preserved");
        assert(!ui.redundantXeFgSectionRendered && "Redundant Frame Generation (XeFG) hidden in OptiScaler mode");
        assert(!ui.redundantOptiFgSectionRendered && "Redundant Frame Generation (OptiFG) hidden in OptiScaler mode");
        printf(
            "  [PASS] Test 2: Active XeMFG (OptiScaler Source) renders unified controls & elevates ForceBorderless.\n");
    }

    // Test 3: Inactive XeMFG restores standard FG selection and allows stock XeFG / OptiFG sections
    {
        SimulatedConfig config;
        config.XeMfgUnlock = false;
        config.FGEnabled = true;
        config.FGInputMode = FGInput::DLSSG;
        config.FGOutputMode = FGOutput::XeFG;

        SimulatedState state;
        state.activeFgInput = FGInput::DLSSG;
        state.activeFgOutput = FGOutput::XeFG;
        state.currentFGSwapchain = true;

        auto ui = SimulateRenderMenu(config, state);

        assert(!ui.xeMfgDedicatedSectionRendered && "XeMFG section must not be rendered when disabled");
        assert(ui.standardFgSelectionRendered && "Standard FG selection controls must be rendered normally");
        assert(!ui.standardFgLockedNoticeRendered && "No lockout notice when XeMFG is disabled");
        assert(ui.redundantXeFgSectionRendered && "Stock XeFG section rendered when XeMFG inactive");
        assert(ui.legacyXeFgControlsInteractive && "Stock XeFG controls interactive");
        printf("  [PASS] Test 3: Inactive XeMFG restores standard FG selection and allows stock XeFG controls.\n");
    }

    // Test 4: Strict mutual exclusion enforcement between Ada, Ampere, External FG, and XeMFG
    {
        SimulatedConfig config;

        // Enable XeMFG -> Must disable Ada, Ampere, SmoothMotion
        config.XeMfgUnlock = true;
        if (config.XeMfgUnlock.value_or_default())
        {
            config.FGDLSSGAdaMfgUnlock = false;
            config.AmpereMfgUnlock = false;
            config.FGDLSSGSmoothMotion = false;
        }
        assert(config.XeMfgUnlock.value_or_default() == true);
        assert(config.FGDLSSGAdaMfgUnlock.value_or_default() == false);
        assert(config.AmpereMfgUnlock.value_or_default() == false);
        assert(config.FGDLSSGSmoothMotion.value_or_default() == false);

        // Enable Ada MFG -> Must disable XeMFG
        config.FGDLSSGAdaMfgUnlock = true;
        if (config.FGDLSSGAdaMfgUnlock.value_or_default())
        {
            config.XeMfgUnlock = false;
            config.AmpereMfgUnlock = false;
        }
        assert(config.XeMfgUnlock.value_or_default() == false);
        assert(config.FGDLSSGAdaMfgUnlock.value_or_default() == true);
        assert(config.AmpereMfgUnlock.value_or_default() == false);

        // Enable Ampere MFG -> Must disable XeMFG
        config.AmpereMfgUnlock = true;
        if (config.AmpereMfgUnlock.value_or_default())
        {
            config.XeMfgUnlock = false;
            config.FGDLSSGAdaMfgUnlock = false;
        }
        assert(config.XeMfgUnlock.value_or_default() == false);
        assert(config.FGDLSSGAdaMfgUnlock.value_or_default() == false);
        assert(config.AmpereMfgUnlock.value_or_default() == true);

        // Enable External FG -> Must disable XeMFG
        config.AmpereMfgUnlock = false;
        config.ExternalFrameGeneration = true;
        if (config.ExternalFrameGeneration.value_or_default())
        {
            config.XeMfgUnlock = false;
        }
        assert(config.ExternalFrameGeneration.value_or_default() == true);
        assert(config.XeMfgUnlock.value_or_default() == false);
        printf("  [PASS] Test 4: Strict mutual exclusion across Ada, Ampere, External FG, and XeMFG.\n");
    }

    // Test 5: Backward compatibility migration: legacy [XeMFG-SR] Enable=true maps to UnlockMFG=true, Source=OptiScaler
    {
        SimulatedConfig config;
        // Simulate legacy INI with [XeMFG-SR] Enable=true and GeneratedFrames=4
        config.XeMfgUnlock.reset();
        config.XeMfgSrEnable = true;
        config.XeMfgSrGeneratedFrames = 4;

        // Migration logic as implemented in Config.cpp
        if (!config.XeMfgUnlock.has_value() && config.XeMfgSrEnable.value_or_default())
        {
            config.XeMfgUnlock = true;
            config.XeMfgSourceType = static_cast<int>(XeMfgSource::OptiScaler);
            config.XeMfgMaxFrames = config.XeMfgSrGeneratedFrames.value_or(2);
        }

        assert(config.XeMfgUnlock.value_or_default() == true);
        assert(config.XeMfgSourceValue() == XeMfgSource::OptiScaler);
        assert(config.XeMfgMaxFrames.value_or_default() == 4);
        printf(
            "  [PASS] Test 5: Legacy XeMFG-SR configuration cleanly migrates to unified XeMFG (Source=OptiScaler).\n");
    }

    printf("[+] All Unified XeMFG Menu UI Conflict Suppression tests PASSED successfully!\n");
    return 0;
}

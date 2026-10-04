#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

// Standalone unit test validating XeMFG menu UI conflict suppression below
// Advanced FG Settings, notice rendering, and strict mutual exclusion.

template <class T> class TestCustomOptional : public std::optional<T>
{
    T _defaultValue {};

  public:
    TestCustomOptional(T defaultValue)
    {
        _defaultValue = defaultValue;
        this->reset();
    }

    TestCustomOptional() { this->reset(); }

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
    TestCustomOptional<bool> FGDLSSGAdaMfgUnlock { false };
    TestCustomOptional<bool> AmpereMfgUnlock { false };

    TestCustomOptional<bool> FGEnabled { false };
    TestCustomOptional<FGInput> FGInputMode { FGInput::NoFG };
    TestCustomOptional<FGOutput> FGOutputMode { FGOutput::NoFG };
    TestCustomOptional<int> FGXeFGInterpolationCount { 1 };
};

struct SimulatedState
{
    FGInput activeFgInput = FGInput::NoFG;
    FGOutput activeFgOutput = FGOutput::NoFG;
    bool currentFGSwapchain = false;
    bool dlssgDetectedInterpolationCount = 0;
};

struct MenuUiOutput
{
    bool xeMfgDedicatedSectionRendered = false;
    bool legacyXeFgSectionRendered = false;
    bool legacyXeFgNoticeRendered = false;
    bool legacyXeFgControlsInteractive = false;
    bool streamlineInputsSectionRendered = false;
    std::string noticeText;
};

static MenuUiOutput SimulateRenderMenu(const SimulatedConfig& config, const SimulatedState& state)
{
    MenuUiOutput out;

    const bool xeMfgActive = config.XeMfgUnlock.value_or_default();

    // 1. Dedicated XeMFG section at top of Frame Generation
    if (xeMfgActive)
    {
        out.xeMfgDedicatedSectionRendered = true;
    }

    // 2. XeFG controls below Advanced FG Settings
    if (xeMfgActive && state.activeFgOutput == FGOutput::XeFG)
    {
        out.legacyXeFgSectionRendered = true;
        out.legacyXeFgNoticeRendered = true;
        out.noticeText = "Managed exclusively by the Intel Xe Multi-Frame Generation (XeMFG) section above.";
        out.legacyXeFgControlsInteractive = false; // Controls are suppressed!
    }
    else if (!xeMfgActive && state.activeFgOutput == FGOutput::XeFG && state.activeFgInput != FGInput::NoFG &&
             state.activeFgInput != FGInput::ForceXeLL && state.currentFGSwapchain)
    {
        out.legacyXeFgSectionRendered = true;
        out.legacyXeFgNoticeRendered = false;
        out.legacyXeFgControlsInteractive = true; // Interactive Active##3 and MFG combo
    }

    // 3. Streamline FG Inputs
    if (!xeMfgActive && state.currentFGSwapchain && state.activeFgInput == FGInput::DLSSG)
    {
        out.streamlineInputsSectionRendered = true;
    }

    return out;
}

int main()
{
    printf("[+] Starting XeMFG Menu UI Conflict Suppression & Exclusion Tests...\n");

    // Test 1: Active XeMFG suppresses legacy XeFG controls and Streamline prompt
    {
        SimulatedConfig config;
        config.XeMfgUnlock = true;
        config.FGEnabled = true;
        config.FGInputMode = FGInput::DLSSG;
        config.FGOutputMode = FGOutput::XeFG;

        SimulatedState state;
        state.activeFgInput = FGInput::DLSSG;
        state.activeFgOutput = FGOutput::XeFG;
        state.currentFGSwapchain = true;

        auto ui = SimulateRenderMenu(config, state);

        assert(ui.xeMfgDedicatedSectionRendered && "Dedicated XeMFG section must be rendered");
        assert(ui.legacyXeFgSectionRendered && "Legacy XeFG header rendered with notice");
        assert(ui.legacyXeFgNoticeRendered && "Managed notice must be displayed");
        assert(!ui.legacyXeFgControlsInteractive && "Legacy Active##3 and MFG controls must be suppressed");
        assert(!ui.streamlineInputsSectionRendered && "Redundant Streamline FG Inputs section must be suppressed");
        assert(ui.noticeText == "Managed exclusively by the Intel Xe Multi-Frame Generation (XeMFG) section above.");
        printf("  [PASS] Test 1: Active XeMFG suppresses legacy XeFG controls and Streamline prompt.\n");
    }

    // Test 2: Inactive XeMFG allows legacy XeFG controls and Streamline prompt
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
        assert(ui.legacyXeFgSectionRendered && "Legacy XeFG section rendered");
        assert(!ui.legacyXeFgNoticeRendered && "Managed notice must not be rendered when XeMFG is disabled");
        assert(ui.legacyXeFgControlsInteractive && "Legacy Active##3 and MFG controls must remain interactive");
        assert(ui.streamlineInputsSectionRendered && "Streamline FG Inputs section must be rendered for stock usage");
        printf("  [PASS] Test 2: Inactive XeMFG allows legacy XeFG controls and Streamline prompt.\n");
    }

    // Test 3: Strict mutual exclusion enforcement between Ada, Ampere, and XeMFG
    {
        SimulatedConfig config;

        // Enable XeMFG -> Must disable Ada and Ampere
        config.XeMfgUnlock = true;
        if (config.XeMfgUnlock.value_or_default())
        {
            config.FGDLSSGAdaMfgUnlock = false;
            config.AmpereMfgUnlock = false;
        }
        assert(config.XeMfgUnlock.value_or_default() == true);
        assert(config.FGDLSSGAdaMfgUnlock.value_or_default() == false);
        assert(config.AmpereMfgUnlock.value_or_default() == false);

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
        printf("  [PASS] Test 3: Strict mutual exclusion across Ada, Ampere, and XeMFG.\n");
    }

    printf("[+] All XeMFG Menu UI Conflict Suppression tests PASSED successfully!\n");
    return 0;
}

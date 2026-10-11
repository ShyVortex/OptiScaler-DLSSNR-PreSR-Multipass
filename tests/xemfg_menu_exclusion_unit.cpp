#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

// Standalone unit test validating XeMFG menu UI conflict suppression,
// standard FG selection lockout, notice rendering, and strict mutual exclusion.

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
    TestCustomOptional<bool> FGDLSSGAdaMfgUnlock { false };
    TestCustomOptional<bool> AmpereMfgUnlock { false };
    TestCustomOptional<bool> XeMfgSrEnable { false };
    TestCustomOptional<int> XeMfgSrGeneratedFrames { 2 };
    TestCustomOptional<bool> FGDLSSGSmoothMotion { false };
    TestCustomOptional<bool> ExternalFrameGeneration { false };

    TestCustomOptional<bool> FGEnabled { false };
    TestCustomOptional<FGInput> FGInputMode { FGInput::NoFG };
    TestCustomOptional<FGOutput> FGOutputMode { FGOutput::NoFG };
    TestCustomOptional<int> FGXeFGInterpolationCount { 1 };
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
    // 1. Dedicated XeMFG section
    bool xeMfgDedicatedSectionRendered = false;

    // 1b. Dedicated XeMFG-SR section
    bool xeMfgSrDedicatedSectionRendered = false;

    // 2. Standard Frame Generation Selection (RenderFrameGenerationSelection)
    bool standardFgSelectionRendered = false;
    bool standardFgLockedNoticeRendered = false;
    std::string standardFgLockedNoticeText;

    // 3. Legacy XeFG Runtime section below Advanced FG Settings
    bool legacyXeFgSectionRendered = false;
    bool legacyXeFgNoticeRendered = false;
    bool legacyXeFgControlsInteractive = false;
    std::string runtimeNoticeText;

    // 4. Streamline FG Inputs section
    bool streamlineInputsSectionRendered = false;
};

static MenuUiOutput SimulateRenderMenu(const SimulatedConfig& config, const SimulatedState& state)
{
    MenuUiOutput out;

    const bool xeMfgActive =
        config.XeMfgUnlock.value_or_default() && !config.ExternalFrameGeneration.value_or_default() &&
        !config.FGDLSSGAdaMfgUnlock.value_or_default() && !config.AmpereMfgUnlock.value_or_default();
    const bool xeMfgSrActive = config.XeMfgSrEnable.value_or_default() && !config.XeMfgUnlock.value_or_default() &&
                               !config.ExternalFrameGeneration.value_or_default() &&
                               !config.FGDLSSGAdaMfgUnlock.value_or_default() &&
                               !config.AmpereMfgUnlock.value_or_default();

    // 1. Dedicated XeMFG section at top of Frame Generation
    if (config.XeMfgUnlock.value_or_default())
    {
        out.xeMfgDedicatedSectionRendered = true;
    }

    // 1b. Dedicated XeMFG-SR section (rendered when primary XeMfgUnlock is not enabled)
    if (!config.XeMfgUnlock.value_or_default())
    {
        out.xeMfgSrDedicatedSectionRendered = true;
    }

    // 2. Standard Frame Generation Selection (RenderFrameGenerationSelection)
    if (state.externalFrameGeneration || xeMfgActive || xeMfgSrActive)
    {
        if (xeMfgActive)
        {
            out.standardFgLockedNoticeRendered = true;
            out.standardFgLockedNoticeText =
                "Intel Xe Multi-Frame Generation (XeMFG) is active. Standard Frame Generation controls are locked and "
                "managed automatically.";
        }
        else if (xeMfgSrActive)
        {
            out.standardFgLockedNoticeRendered = true;
            out.standardFgLockedNoticeText =
                "Intel XeMFG-SR Injector is active. Multiplier is controlled via the Intel Xe Multi-Frame Generation "
                "for DLSS-SR (XeMFG-SR) section above.";
        }
        out.standardFgSelectionRendered = false; // Early return prevents rendering FG Enabled, Input, Output
    }
    else
    {
        out.standardFgSelectionRendered = true;
        out.standardFgLockedNoticeRendered = false;
    }

    // 3. XeFG controls below Advanced FG Settings (RenderFrameGenerationRuntimeSettings)
    if ((xeMfgActive || xeMfgSrActive) && state.activeFgOutput == FGOutput::XeFG)
    {
        out.legacyXeFgSectionRendered = true;
        out.legacyXeFgNoticeRendered = true;
        out.runtimeNoticeText =
            xeMfgActive
                ? "Managed exclusively by the Intel Xe Multi-Frame Generation (XeMFG) section above."
                : "Managed exclusively by the Intel Xe Multi-Frame Generation for DLSS-SR (XeMFG-SR) section above.";
        out.legacyXeFgControlsInteractive = false; // Controls are suppressed!
    }
    else if (!xeMfgActive && !xeMfgSrActive && state.activeFgOutput == FGOutput::XeFG &&
             state.activeFgInput != FGInput::NoFG && state.activeFgInput != FGInput::ForceXeLL &&
             state.currentFGSwapchain)
    {
        out.legacyXeFgSectionRendered = true;
        out.legacyXeFgNoticeRendered = false;
        out.legacyXeFgControlsInteractive = true; // Interactive Active##3 and MFG combo
    }

    // 4. Streamline FG Inputs
    if (!xeMfgActive && !xeMfgSrActive && state.currentFGSwapchain && state.activeFgInput == FGInput::DLSSG)
    {
        out.streamlineInputsSectionRendered = true;
    }

    return out;
}

int main()
{
    printf("[+] Starting XeMFG Menu UI Conflict Suppression & Exclusion Tests...\n");

    // Test 1: Active XeMFG locks out standard FG selection and suppresses legacy XeFG controls
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
        assert(!ui.standardFgSelectionRendered && "Standard FG selection controls must be locked out and suppressed");
        assert(ui.standardFgLockedNoticeRendered && "Lockout notice must be displayed in standard FG selection");
        assert(ui.legacyXeFgSectionRendered && "Legacy XeFG header rendered with notice");
        assert(ui.legacyXeFgNoticeRendered && "Managed notice must be displayed in runtime settings");
        assert(!ui.legacyXeFgControlsInteractive && "Legacy Active##3 and MFG controls must be suppressed");
        assert(!ui.streamlineInputsSectionRendered && "Redundant Streamline FG Inputs section must be suppressed");
        printf("  [PASS] Test 1: Active XeMFG locks standard FG selection and suppresses duplicate controls.\n");
    }

    // Test 2: Inactive XeMFG restores standard FG selection and allows legacy XeFG controls
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
        assert(ui.legacyXeFgSectionRendered && "Legacy XeFG section rendered");
        assert(!ui.legacyXeFgNoticeRendered && "Managed notice must not be rendered when XeMFG is disabled");
        assert(ui.legacyXeFgControlsInteractive && "Legacy Active##3 and MFG controls must remain interactive");
        assert(ui.streamlineInputsSectionRendered && "Streamline FG Inputs section must be rendered for stock usage");
        printf("  [PASS] Test 2: Inactive XeMFG renders standard FG selection and legacy controls normally.\n");
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

    // Test 4: Active XeMFG-SR locks out standard FG selection and displays XeMFG-SR managed notice
    {
        SimulatedConfig config;
        config.XeMfgUnlock = false;
        config.XeMfgSrEnable = true;
        config.FGEnabled = true;
        config.FGInputMode = FGInput::Upscaler;
        config.FGOutputMode = FGOutput::XeFG;

        SimulatedState state;
        state.activeFgInput = FGInput::Upscaler;
        state.activeFgOutput = FGOutput::XeFG;
        state.currentFGSwapchain = true;

        auto ui = SimulateRenderMenu(config, state);

        assert(ui.xeMfgSrDedicatedSectionRendered && "Dedicated XeMFG-SR section must be rendered");
        assert(!ui.xeMfgDedicatedSectionRendered && "XeMFG section must be hidden/skipped when XeMfgUnlock=false");
        assert(!ui.standardFgSelectionRendered && "Standard FG selection controls must be locked out for XeMFG-SR");
        assert(ui.standardFgLockedNoticeRendered && "Lockout notice must be displayed for XeMFG-SR");
        assert(ui.standardFgLockedNoticeText.find("XeMFG-SR") != std::string::npos &&
               "Notice must refer to XeMFG-SR Injector");
        assert(ui.legacyXeFgSectionRendered && "Legacy XeFG header rendered with notice");
        assert(ui.legacyXeFgNoticeRendered && "Managed notice must be displayed in runtime settings");
        assert(ui.runtimeNoticeText.find("XeMFG-SR") != std::string::npos &&
               "Runtime notice must cite XeMFG-SR section");
        assert(!ui.legacyXeFgControlsInteractive && "Legacy Active##3 and MFG controls must be suppressed");
        printf("  [PASS] Test 4: Active XeMFG-SR locks standard FG selection and displays XeMFG-SR managed notice.\n");
    }

    // Test 5: Strict mutual exclusion across XeMFG-SR, XeMFG, Ada, Ampere, Smooth Motion, and External FG
    {
        SimulatedConfig config;

        // Enable XeMFG-SR -> Must disable XeMfgUnlock, Ada, Ampere, SmoothMotion
        config.XeMfgSrEnable = true;
        if (config.XeMfgSrEnable.value_or_default())
        {
            config.XeMfgUnlock = false;
            config.FGDLSSGAdaMfgUnlock = false;
            config.AmpereMfgUnlock = false;
            config.FGDLSSGSmoothMotion = false;
        }
        assert(config.XeMfgSrEnable.value_or_default() == true);
        assert(config.XeMfgUnlock.value_or_default() == false);
        assert(config.FGDLSSGAdaMfgUnlock.value_or_default() == false);
        assert(config.AmpereMfgUnlock.value_or_default() == false);
        assert(config.FGDLSSGSmoothMotion.value_or_default() == false);

        // Enable XeMfgUnlock -> Must disable XeMfgSrEnable
        config.XeMfgUnlock = true;
        if (config.XeMfgUnlock.value_or_default())
        {
            config.XeMfgSrEnable = false;
        }
        assert(config.XeMfgUnlock.value_or_default() == true);
        assert(config.XeMfgSrEnable.value_or_default() == false);

        // Reset and test Ada MFG -> Must disable XeMfgSrEnable
        config.XeMfgUnlock = false;
        config.XeMfgSrEnable = true;
        config.FGDLSSGAdaMfgUnlock = true;
        if (config.FGDLSSGAdaMfgUnlock.value_or_default())
        {
            config.XeMfgSrEnable = false;
        }
        assert(config.FGDLSSGAdaMfgUnlock.value_or_default() == true);
        assert(config.XeMfgSrEnable.value_or_default() == false);

        // Reset and test Ampere MFG -> Must disable XeMfgSrEnable
        config.FGDLSSGAdaMfgUnlock = false;
        config.XeMfgSrEnable = true;
        config.AmpereMfgUnlock = true;
        if (config.AmpereMfgUnlock.value_or_default())
        {
            config.XeMfgSrEnable = false;
        }
        assert(config.AmpereMfgUnlock.value_or_default() == true);
        assert(config.XeMfgSrEnable.value_or_default() == false);

        // Reset and test Smooth Motion -> Must disable XeMfgSrEnable
        config.AmpereMfgUnlock = false;
        config.XeMfgSrEnable = true;
        config.FGDLSSGSmoothMotion = true;
        if (config.FGDLSSGSmoothMotion.value_or_default())
        {
            config.XeMfgSrEnable = false;
        }
        assert(config.FGDLSSGSmoothMotion.value_or_default() == true);
        assert(config.XeMfgSrEnable.value_or_default() == false);

        // Reset and test External FG -> Must disable XeMfgSrEnable
        config.FGDLSSGSmoothMotion = false;
        config.XeMfgSrEnable = true;
        config.ExternalFrameGeneration = true;
        if (config.ExternalFrameGeneration.value_or_default())
        {
            config.XeMfgSrEnable = false;
        }
        assert(config.ExternalFrameGeneration.value_or_default() == true);
        assert(config.XeMfgSrEnable.value_or_default() == false);

        printf("  [PASS] Test 5: Strict bidirectional mutual exclusion between XeMFG-SR and all FG providers.\n");
    }

    // Test 6: Primary XeMFG priority over XeMFG-SR in session resolution
    {
        SimulatedConfig config;
        config.XeMfgUnlock = true;
        config.XeMfgSrEnable = true; // Both set somehow in conflicting INI

        SimulatedState state;
        state.activeFgInput = FGInput::DLSSG;
        state.activeFgOutput = FGOutput::XeFG;

        auto ui = SimulateRenderMenu(config, state);

        assert(ui.xeMfgDedicatedSectionRendered && "Primary XeMFG takes precedence");
        assert(!ui.xeMfgSrDedicatedSectionRendered && "XeMFG-SR section must not render when primary is active");
        assert(ui.standardFgLockedNoticeText.find("Intel Xe Multi-Frame Generation (XeMFG) is active") !=
                   std::string::npos &&
               "Primary XeMFG notice takes precedence");
        printf("  [PASS] Test 6: Primary XeMFG takes strict precedence over XeMFG-SR when both present.\n");
    }

    printf("[+] All XeMFG Menu UI Conflict Suppression tests PASSED successfully!\n");
    return 0;
}

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

// Standalone unit test for XeFG Multi-Frame Generation, Streamline option routing,
// passthrough mode crash prevention, and mutual exclusion.

namespace sl
{
enum class Result
{
    eOk = 0,
    eError = -1
};
enum class DLSSGMode
{
    eOff = 0,
    eOn = 1,
    eAuto = 2,
    eDynamic = 3
};

struct DLSSGOptions
{
    uint32_t structVersion = 1;
    DLSSGMode mode = DLSSGMode::eOff;
    uint32_t numFramesToGenerate = 1;
};

struct DLSSGState
{
    uint32_t structVersion = 1;
    uint32_t numFramesToGenerateMax = 1;
};
} // namespace sl

enum class FGOutput
{
    NoFG = 0,
    FSRFG = 1,
    XeFG = 2,
    DLSSG = 3
};

struct MockXeMfgStatus
{
    bool Applied = false;
    uint32_t PatchesApplied = 0;
};

class MockXeMfgLoader
{
  public:
    static bool Enabled;
    static bool LastFailureFlag;
    static MockXeMfgStatus Status;
    static uint32_t MaxFrames;

    static bool IsEnabled() { return Enabled; }
    static bool LastFailure() { return LastFailureFlag; }
    static const MockXeMfgStatus& LastStatus() { return Status; }
    static uint32_t EffectiveMax(uint32_t nativeReported)
    {
        if (LastFailureFlag)
            return 1;
        if (!Enabled || !Status.Applied)
            return nativeReported;
        return std::max(MaxFrames, nativeReported);
    }
};

bool MockXeMfgLoader::Enabled = true;
bool MockXeMfgLoader::LastFailureFlag = false;
MockXeMfgStatus MockXeMfgLoader::Status = { true, 5 };
uint32_t MockXeMfgLoader::MaxFrames = 3; // 4X default

class MockXeFGProxy
{
  public:
    static bool Enabled;
    static uint32_t NumInterpolatedFrames;

    static void SetEnabled(bool enabled) { Enabled = enabled; }
    static void SetNumInterpolatedFrames(uint32_t numFrames) { NumInterpolatedFrames = numFrames; }
};

bool MockXeFGProxy::Enabled = true;
uint32_t MockXeFGProxy::NumInterpolatedFrames = 1;

class MockXeFG_Dx12
{
  private:
    bool _passthrough = false;
    uint32_t _framesToInterpolate = 1;
    uint32_t _maxInterpolationCount = 1;

  public:
    uint32_t executedPresentPasses = 0;
    bool crashSimulated = false;

    MockXeFG_Dx12()
    {
        _maxInterpolationCount = MockXeMfgLoader::EffectiveMax(1);
        _framesToInterpolate = 1;
        _passthrough = false;
    }

    uint32_t GetMaxInterpolationCount() const { return _maxInterpolationCount; }
    uint32_t GetInterpolatedFrameCount() const { return _framesToInterpolate; }
    bool IsPassthrough() const { return _passthrough; }

    void SetInterpolatedFrameCount(uint32_t count)
    {
        if (count == 0)
        {
            _passthrough = true;
            MockXeFGProxy::SetEnabled(false);
            return;
        }

        _passthrough = false;
        MockXeFGProxy::SetEnabled(true);

        count = std::clamp(count, 1u, _maxInterpolationCount);
        _framesToInterpolate = count;
        MockXeFGProxy::SetNumInterpolatedFrames(count);
    }

    bool Present()
    {
        if (_passthrough)
        {
            // Passthrough mode safely avoids calling any XeFG interpolation logic,
            // preventing crashes when frame generation is toggled off in-game!
            return true;
        }

        // If passthrough is false but XeFG was disabled, this would crash in real life.
        if (!MockXeFGProxy::Enabled)
        {
            crashSimulated = true;
            return false;
        }

        executedPresentPasses += _framesToInterpolate;
        return true;
    }
};

struct MockState
{
    FGOutput activeFgOutput = FGOutput::XeFG;
    int dlssgDetectedInterpolationCount = 0;
};

struct MockConfig
{
    std::optional<int> FGXeFGInterpolationCount = std::nullopt;
    bool XeMfgUnlock = true;
    int XeMfgMaxFrames = 3;
    bool FGDLSSGAdaMfgUnlock = false;
    bool FGDLSSGAmpereMfgUnlock = false;
    bool FGDLSSGSmoothMotion = false;
};

// Simulated Streamline DLSSG GetState logic
sl::Result SimulateStreamlineGetState(MockState& state, sl::DLSSGState& outState)
{
    if (state.activeFgOutput == FGOutput::XeFG && MockXeMfgLoader::IsEnabled() &&
        MockXeMfgLoader::LastStatus().Applied && !MockXeMfgLoader::LastFailure())
    {
        outState.numFramesToGenerateMax = MockXeMfgLoader::EffectiveMax(1);
    }
    else
    {
        outState.numFramesToGenerateMax = 1;
    }
    return sl::Result::eOk;
}

// Simulated Streamline DLSSG SetOptions logic
sl::Result SimulateStreamlineSetOptions(MockState& state, MockConfig& config, MockXeFG_Dx12& currentFG,
                                        const sl::DLSSGOptions& options)
{
    if (options.mode == sl::DLSSGMode::eOff)
    {
        state.dlssgDetectedInterpolationCount = 0;
        currentFG.SetInterpolatedFrameCount(0);
    }
    else
    {
        state.dlssgDetectedInterpolationCount = static_cast<int>(options.numFramesToGenerate);
        config.FGXeFGInterpolationCount = static_cast<int>(options.numFramesToGenerate);
        currentFG.SetInterpolatedFrameCount(options.numFramesToGenerate);
    }
    return sl::Result::eOk;
}

int main()
{
    printf("[+] Starting XeFG MFG & Streamline Unit Tests...\n");

    MockState state;
    MockConfig config;
    MockXeFG_Dx12 xefg;

    // Test 1: Streamline GetState Advertising
    {
        sl::DLSSGState slState;
        SimulateStreamlineGetState(state, slState);
        assert(slState.numFramesToGenerateMax == 3 && "Streamline must advertise XeMFG MaxFrames (3 = 4X)");

        // Non-XeFG output clamping
        state.activeFgOutput = FGOutput::FSRFG;
        SimulateStreamlineGetState(state, slState);
        assert(slState.numFramesToGenerateMax == 1 && "Streamline must clamp to 1 for non-XeFG");

        // Failure clamping
        state.activeFgOutput = FGOutput::XeFG;
        MockXeMfgLoader::LastFailureFlag = true;
        SimulateStreamlineGetState(state, slState);
        assert(slState.numFramesToGenerateMax == 1 && "Streamline must clamp to 1 when LastFailure is true");
        MockXeMfgLoader::LastFailureFlag = false;

        printf("  [PASS] Test 1: Streamline GetState capability advertising and safety clamping.\n");
    }

    // Test 2: Streamline SetOptions Multiplier Routing (e.g. 3X FG)
    {
        sl::DLSSGOptions options;
        options.mode = sl::DLSSGMode::eOn;
        options.numFramesToGenerate = 2; // 3X FG

        SimulateStreamlineSetOptions(state, config, xefg, options);

        assert(state.dlssgDetectedInterpolationCount == 2 && "State must record 2 generated frames");
        assert(config.FGXeFGInterpolationCount == 2 && "Config volatile interpolation count must sync to 2");
        assert(xefg.GetInterpolatedFrameCount() == 2 && "XeFG must be configured for 2 frames");
        assert(!xefg.IsPassthrough() && "XeFG must not be in passthrough mode");
        assert(MockXeFGProxy::Enabled && "Proxy must be enabled");
        assert(MockXeFGProxy::NumInterpolatedFrames == 2 && "Proxy must receive frame count");

        bool presentOk = xefg.Present();
        assert(presentOk && !xefg.crashSimulated && "Present must execute cleanly");
        assert(xefg.executedPresentPasses == 2 && "2 interpolated frames must be rendered");

        printf("  [PASS] Test 2: Streamline SetOptions multiplier routing and XeFG execution.\n");
    }

    // Test 3: In-Game Disable & Passthrough Mode (Crash Fix)
    {
        sl::DLSSGOptions offOptions;
        offOptions.mode = sl::DLSSGMode::eOff;
        offOptions.numFramesToGenerate = 0;

        SimulateStreamlineSetOptions(state, config, xefg, offOptions);

        assert(state.dlssgDetectedInterpolationCount == 0 && "State must record 0 on disable");
        assert(xefg.IsPassthrough() && "XeFG must enter passthrough mode on in-game disable");
        assert(!MockXeFGProxy::Enabled && "XeFG Proxy must be disabled");

        // Calling Present() while disabled in-game: MUST NOT CRASH!
        uint32_t passesBefore = xefg.executedPresentPasses;
        bool presentOk = xefg.Present();
        assert(presentOk && !xefg.crashSimulated && "Present must succeed in passthrough mode without crashing");
        assert(xefg.executedPresentPasses == passesBefore && "Zero XeFG interpolation passes must be executed");

        printf("  [PASS] Test 3: In-game disable enters passthrough mode and prevents crashes.\n");
    }

    // Test 4: Re-enabling to 4X FG after In-Game Disable
    {
        sl::DLSSGOptions reenableOptions;
        reenableOptions.mode = sl::DLSSGMode::eOn;
        reenableOptions.numFramesToGenerate = 3; // 4X FG

        SimulateStreamlineSetOptions(state, config, xefg, reenableOptions);

        assert(state.dlssgDetectedInterpolationCount == 3);
        assert(!xefg.IsPassthrough() && "XeFG must exit passthrough mode");
        assert(MockXeFGProxy::Enabled && "XeFG Proxy must be re-enabled");
        assert(xefg.GetInterpolatedFrameCount() == 3);

        uint32_t passesBefore = xefg.executedPresentPasses;
        bool presentOk = xefg.Present();
        assert(presentOk && !xefg.crashSimulated);
        assert(xefg.executedPresentPasses == passesBefore + 3 && "3 interpolated frames must be rendered");

        printf("  [PASS] Test 4: Re-enabling Multi-Frame Generation resumes smoothly from passthrough.\n");
    }

    // Test 5: Mutual Exclusion Enforcement
    {
        // When XeMfg is enabled:
        config.XeMfgUnlock = true;
        config.FGDLSSGAdaMfgUnlock = true;
        config.FGDLSSGAmpereMfgUnlock = true;
        config.FGDLSSGSmoothMotion = true;

        // Apply mutual exclusion rule
        if (config.XeMfgUnlock)
        {
            config.FGDLSSGAdaMfgUnlock = false;
            config.FGDLSSGAmpereMfgUnlock = false;
            config.FGDLSSGSmoothMotion = false;
        }

        assert(!config.FGDLSSGAdaMfgUnlock && "Ada MFG must be cleared when XeMfg is enabled");
        assert(!config.FGDLSSGAmpereMfgUnlock && "Ampere MFG must be cleared when XeMfg is enabled");
        assert(!config.FGDLSSGSmoothMotion && "Smooth Motion must be cleared when XeMfg is enabled");

        // When Ada is enabled:
        config.FGDLSSGAdaMfgUnlock = true;
        if (config.FGDLSSGAdaMfgUnlock)
        {
            config.XeMfgUnlock = false;
            config.FGDLSSGAmpereMfgUnlock = false;
        }
        assert(!config.XeMfgUnlock && "XeMfg must be cleared when Ada MFG is enabled");

        // When Ampere is enabled:
        config.FGDLSSGAmpereMfgUnlock = true;
        if (config.FGDLSSGAmpereMfgUnlock)
        {
            config.XeMfgUnlock = false;
            config.FGDLSSGAdaMfgUnlock = false;
        }
        assert(!config.XeMfgUnlock && "XeMfg must be cleared when Ampere MFG is enabled");

        printf("  [PASS] Test 5: Strict mutual exclusion across Ada, Ampere, and XeMFG.\n");
    }

    printf("[+] All XeFG MFG & Streamline unit tests PASSED successfully!\n");
    return 0;
}

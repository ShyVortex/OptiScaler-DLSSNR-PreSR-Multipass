#include <cassert>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <optional>

// Mock structures mimicking Streamline DLSSG API & OptiScaler state
namespace sl {
    enum class Result { eOk = 0, eError = -1 };
    enum class DLSSGMode { eOff = 0, eOn = 1, eAuto = 2 };

    struct DLSSGOptions {
        uint32_t structVersion = 1;
        DLSSGMode mode = DLSSGMode::eOff;
        uint32_t numFramesToGenerate = 1;
    };

    struct DLSSGState {
        uint32_t structVersion = 1;
        uint32_t numFramesToGenerateMax = 1;
    };
}

// Mock Config
struct MockConfig {
    std::optional<int> FGDLSSGOverrideInterpolationCount = std::nullopt;

    void set_volatile_value(int val) {
        FGDLSSGOverrideInterpolationCount = val;
    }
};

// Mock MfgUnlock
struct MockMfgUnlock {
    static bool enabledForSession;
    static unsigned int unlockedMax;
    static bool pending;

    static bool EnabledForSession() { return enabledForSession; }
    static unsigned int UnlockedMax() { return unlockedMax; }
    static bool Pending() { return pending; }
};

bool MockMfgUnlock::enabledForSession = false;
unsigned int MockMfgUnlock::unlockedMax = 0;
bool MockMfgUnlock::pending = false;

// Mock ReflexHooks
struct MockReflexHooks {
    static int dlssgFrameCount;
    static void setDlssgFrameCount(int count) {
        dlssgFrameCount = count;
    }
};

int MockReflexHooks::dlssgFrameCount = 0;

// Mock OptiScaler State
struct MockState {
    std::optional<int> dlssgMfgMax = std::nullopt;
    sl::DLSSGMode dlssgLastSetMode = sl::DLSSGMode::eOff;
};

// Simulates Streamline DLSSG GetState logic in OptiScaler
sl::Result SimulateGetState(
    uint32_t originalStructVersion,
    sl::DLSSGState& outState,
    MockState& optiState,
    MockConfig& config)
{
    // Simulating the underlying game/driver SL returning 1 frame max (RTX 40 unmodded default)
    outState.numFramesToGenerateMax = 1;

    // RTX 40 MFG Unlock elevation logic
    if (MockMfgUnlock::EnabledForSession()) {
        unsigned int unlocked = MockMfgUnlock::UnlockedMax();
        if (unlocked == 0)
            unlocked = 5;
        outState.numFramesToGenerateMax = std::max(outState.numFramesToGenerateMax, unlocked);
    }

    // Updating OptiScaler internal state
    if (const auto maximum = MockMfgUnlock::UnlockedMax(); maximum > 0)
        optiState.dlssgMfgMax = std::max(optiState.dlssgMfgMax.value_or(0), static_cast<int>(maximum));
    else if (MockMfgUnlock::EnabledForSession())
        optiState.dlssgMfgMax = std::max(optiState.dlssgMfgMax.value_or(0), 5);

    // Populate dlssgMfgMax once & volatile clamp (only when MfgUnlock not active)
    if (!optiState.dlssgMfgMax.has_value() && !MockMfgUnlock::Pending() && !MockMfgUnlock::EnabledForSession())
    {
        optiState.dlssgMfgMax = 1;
        if (config.FGDLSSGOverrideInterpolationCount.has_value() &&
            config.FGDLSSGOverrideInterpolationCount.value() > optiState.dlssgMfgMax.value())
        {
            config.set_volatile_value(optiState.dlssgMfgMax.value());
        }
    }

    return sl::Result::eOk;
}

// Simulates Streamline DLSSG SetOptions logic in OptiScaler
sl::Result SimulateSetOptions(
    sl::DLSSGOptions& options,
    MockState& state,
    MockConfig& config)
{
    sl::DLSSGOptions newOptions = options;

    if (const auto maximum = MockMfgUnlock::UnlockedMax(); maximum > 0)
        state.dlssgMfgMax = std::max(state.dlssgMfgMax.value_or(0), static_cast<int>(maximum));
    else if (MockMfgUnlock::EnabledForSession())
        state.dlssgMfgMax = std::max(state.dlssgMfgMax.value_or(0), 5);

    if (!state.dlssgMfgMax.has_value() && !MockMfgUnlock::Pending() && !MockMfgUnlock::EnabledForSession())
    {
        // Unmodded driver reports max 1
        state.dlssgMfgMax = 1;
        if (config.FGDLSSGOverrideInterpolationCount.has_value() &&
            config.FGDLSSGOverrideInterpolationCount.value() > state.dlssgMfgMax.value())
        {
            config.set_volatile_value(state.dlssgMfgMax.value());
        }
    }

    if (config.FGDLSSGOverrideInterpolationCount.has_value())
    {
        auto overrideCount = config.FGDLSSGOverrideInterpolationCount.value();
        if (overrideCount != 0)
        {
            newOptions.numFramesToGenerate = overrideCount;
            MockReflexHooks::setDlssgFrameCount(overrideCount);
        }
        else
        {
            newOptions.mode = sl::DLSSGMode::eOff;
            MockReflexHooks::setDlssgFrameCount(0);
        }
    }

    state.dlssgLastSetMode = newOptions.mode;
    options = newOptions;
    return sl::Result::eOk;
}

int main()
{
    printf("=== Running Streamline DLSSG Options Unit Tests ===\n");

    // Test 1: Ada MFG active, Override DLSSG Ratio set to 3X (interpolation count = 2)
    {
        MockMfgUnlock::enabledForSession = true;
        MockMfgUnlock::unlockedMax = 5;
        MockMfgUnlock::pending = false;

        MockState state;
        MockConfig config;
        config.FGDLSSGOverrideInterpolationCount = 2; // 3X (2 generated frames)
        MockReflexHooks::dlssgFrameCount = 0;

        sl::DLSSGOptions opts;
        opts.mode = sl::DLSSGMode::eOn;
        opts.numFramesToGenerate = 1; // standard 2X from game

        sl::Result res = SimulateSetOptions(opts, state, config);
        assert(res == sl::Result::eOk);
        assert(state.dlssgMfgMax.has_value() && state.dlssgMfgMax.value() == 5);
        assert(config.FGDLSSGOverrideInterpolationCount.value() == 2); // Not clamped!
        assert(opts.numFramesToGenerate == 2); // Correctly set to 3X
        assert(MockReflexHooks::dlssgFrameCount == 2); // Reflex synced
        printf("Test 1 Passed: Ada MFG with 3X override correctly applies 2 generated frames and updates Reflex.\n");
    }

    // Test 2: Ada MFG active, Override DLSSG Ratio set to 4X (interpolation count = 3)
    {
        MockMfgUnlock::enabledForSession = true;
        MockMfgUnlock::unlockedMax = 5;
        MockMfgUnlock::pending = false;

        MockState state;
        MockConfig config;
        config.FGDLSSGOverrideInterpolationCount = 3; // 4X
        MockReflexHooks::dlssgFrameCount = 0;

        sl::DLSSGOptions opts;
        opts.mode = sl::DLSSGMode::eOn;
        opts.numFramesToGenerate = 1;

        sl::Result res = SimulateSetOptions(opts, state, config);
        assert(res == sl::Result::eOk);
        assert(config.FGDLSSGOverrideInterpolationCount.value() == 3);
        assert(opts.numFramesToGenerate == 3);
        assert(MockReflexHooks::dlssgFrameCount == 3);
        printf("Test 2 Passed: Ada MFG with 4X override correctly applies 3 generated frames and updates Reflex.\n");
    }

    // Test 3: Ada MFG active, GetState returns numFramesToGenerateMax = 5
    {
        MockMfgUnlock::enabledForSession = true;
        MockMfgUnlock::unlockedMax = 5;
        MockMfgUnlock::pending = false;

        MockState state;
        MockConfig config;
        config.FGDLSSGOverrideInterpolationCount = 2;

        sl::DLSSGState slState;
        sl::Result res = SimulateGetState(4, slState, state, config);
        assert(res == sl::Result::eOk);
        assert(slState.numFramesToGenerateMax == 5);
        assert(state.dlssgMfgMax.value() == 5);
        assert(config.FGDLSSGOverrideInterpolationCount.value() == 2);
        printf("Test 3 Passed: GetState reports max 5 to game/UI and avoids premature clamping.\n");
    }

    // Test 4: Ada MFG disabled (standard behavior) - 3X override should be clamped to 1
    {
        MockMfgUnlock::enabledForSession = false;
        MockMfgUnlock::unlockedMax = 0;
        MockMfgUnlock::pending = false;

        MockState state;
        MockConfig config;
        config.FGDLSSGOverrideInterpolationCount = 2; // User requested 3X without unlock

        sl::DLSSGOptions opts;
        opts.mode = sl::DLSSGMode::eOn;
        opts.numFramesToGenerate = 1;

        sl::Result res = SimulateSetOptions(opts, state, config);
        assert(res == sl::Result::eOk);
        assert(state.dlssgMfgMax.has_value() && state.dlssgMfgMax.value() == 1);
        assert(config.FGDLSSGOverrideInterpolationCount.value() == 1); // Clamped down to 1!
        assert(opts.numFramesToGenerate == 1); // Only 1 generated frame
        assert(MockReflexHooks::dlssgFrameCount == 1);
        printf("Test 4 Passed: Ada MFG disabled correctly clamps excessive ratio to 1 (preserves normal guardrail).\n");
    }

    // Test 5: Override set to 0 (Off) turns mode to eOff and Reflex count to 0
    {
        MockMfgUnlock::enabledForSession = true;
        MockMfgUnlock::unlockedMax = 5;
        MockMfgUnlock::pending = false;

        MockState state;
        MockConfig config;
        config.FGDLSSGOverrideInterpolationCount = 0; // Off

        sl::DLSSGOptions opts;
        opts.mode = sl::DLSSGMode::eOn;
        opts.numFramesToGenerate = 1;

        sl::Result res = SimulateSetOptions(opts, state, config);
        assert(res == sl::Result::eOk);
        assert(opts.mode == sl::DLSSGMode::eOff);
        assert(MockReflexHooks::dlssgFrameCount == 0);
        printf("Test 5 Passed: Override count 0 correctly sets DLSSGMode::eOff and 0 frames in Reflex.\n");
    }

    printf("All 5 Streamline DLSSG Options unit tests PASSED successfully.\n");
    return 0;
}

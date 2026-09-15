#include <cassert>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

// Definitions matching nvapi and Streamline headers
constexpr uint32_t NV_GPU_ARCHITECTURE_TU100 = 0x00000160;
constexpr uint32_t NV_GPU_ARCHITECTURE_GA100 = 0x00000170;
constexpr uint32_t NV_GPU_ARCHITECTURE_AD100 = 0x00000190;
constexpr uint32_t ARCH_MAX = 0xFFFFFFFF;

namespace sl {
    enum Feature {
        kFeatureDLSS = 0,
        kFeatureDLSS_G = 1000,
        kFeatureDLSS_RR = 1001,
        kFeatureReflex = 2,
        kFeaturePCL = 3
    };
}

enum class FGInput {
    NoFG = 0,
    OptiScaler = 1,
    DLSSG = 2,
    NvngxFG = 3
};

enum class FGNvngxReplacement {
    None = 0,
    FSR3 = 1,
    DLSSG = 2
};

struct MockState {
    FGInput activeFgInput = FGInput::NoFG;
    FGNvngxReplacement activeFgNvngx = FGNvngxReplacement::None;
    bool externalFrameGeneration = false;
};

struct MockConfig {
    std::optional<bool> StreamlineSpoofing = true;
    std::optional<bool> FGDLSSGAmpereMfgUnlock = false;
};

// Mock Streamline spoofing environment
struct MockStreamlineContext {
    uint32_t systemCapsArch = 0;
    uint32_t lastSetArch = 0;
    bool pluginLoadObservedSpoofedArch = false;
    uint32_t pluginLoadArchSeen = 0;

    void setArch(uint32_t arch) {
        lastSetArch = arch;
        systemCapsArch = arch;
    }

    uint32_t getSystemCapsArch() const {
        return systemCapsArch;
    }

    void spoofArch(uint32_t currentArch, sl::Feature feature, const MockState& state) {
        if (feature == sl::kFeatureDLSS) {
            if (currentArch < NV_GPU_ARCHITECTURE_TU100)
                return setArch(ARCH_MAX);
        } else if (feature == sl::kFeatureDLSS_RR) {
            return;
        } else if (feature == sl::kFeatureDLSS_G) {
            if (state.activeFgNvngx != FGNvngxReplacement::None) {
                // Not testing Dx12/Vulkan unavailability here
            }

            if (currentArch < NV_GPU_ARCHITECTURE_AD100)
                return setArch(ARCH_MAX);
        }
    }

    // Evaluates shouldSpoofArch as implemented in Streamline_Hooks.cpp
    static bool evaluateShouldSpoofArch(const MockConfig& config, const MockState& state) {
        const bool ampereMfgActive = config.FGDLSSGAmpereMfgUnlock.value_or(false);
        return config.StreamlineSpoofing.value_or(true) &&
            (state.activeFgInput == FGInput::NvngxFG ||
             state.activeFgInput == FGInput::DLSSG ||
             ampereMfgActive);
    }

    // Simulates hkdlssg_slOnPluginLoad
    bool simulatePluginLoad(const MockConfig& config, const MockState& state, uint32_t physicalArch) {
        setArch(physicalArch);

        const bool shouldSpoofArch = evaluateShouldSpoofArch(config, state);

        uint32_t currentArch = 0;
        if (shouldSpoofArch) {
            currentArch = getSystemCapsArch();
            spoofArch(currentArch, sl::kFeatureDLSS_G, state);
        }

        // Inside plugin load (sl.dlss_g.dll initialization check)
        pluginLoadArchSeen = getSystemCapsArch();
        pluginLoadObservedSpoofedArch = (pluginLoadArchSeen >= NV_GPU_ARCHITECTURE_AD100);

        // Restore original arch after load
        if (shouldSpoofArch) {
            setArch(currentArch);
        }

        return pluginLoadObservedSpoofedArch;
    }
};

int main() {
    std::printf("Running Streamline Turing & Ampere spoofing unit tests...\n");

    // Test 1: Baseline bug scenario - Turing GPU with external Ampere/Turing MFG, old logic without ampereMfgActive
    {
        MockConfig config;
        config.StreamlineSpoofing = true;
        config.FGDLSSGAmpereMfgUnlock = true;

        MockState state;
        state.externalFrameGeneration = true;
        state.activeFgInput = FGInput::NoFG; // External FG sets activeFgInput to NoFG

        // Old logic check: without ampereMfgActive
        bool oldShouldSpoofArch = config.StreamlineSpoofing.value_or(true) &&
            (state.activeFgInput == FGInput::NvngxFG || state.activeFgInput == FGInput::DLSSG);
        assert(!oldShouldSpoofArch && "Old logic must fail to trigger spoofing when activeFgInput is NoFG!");

        // New logic check: with ampereMfgActive
        bool newShouldSpoofArch = MockStreamlineContext::evaluateShouldSpoofArch(config, state);
        assert(newShouldSpoofArch && "New logic MUST trigger spoofing when FGDLSSGAmpereMfgUnlock is true even if activeFgInput is NoFG!");

        std::printf("  [PASS] Test 1: ampereMfgActive enables shouldSpoofArch during external FG mode\n");
    }

    // Test 2: Plugin load with Turing GPU (0x160) and Ampere MFG active
    {
        MockConfig config;
        config.StreamlineSpoofing = true;
        config.FGDLSSGAmpereMfgUnlock = true;

        MockState state;
        state.externalFrameGeneration = true;
        state.activeFgInput = FGInput::NoFG;

        MockStreamlineContext ctx;
        bool loaded = ctx.simulatePluginLoad(config, state, NV_GPU_ARCHITECTURE_TU100);

        assert(loaded && "Plugin load on Turing must observe spoofed Ada/Max arch!");
        assert(ctx.pluginLoadArchSeen == ARCH_MAX && "Plugin load must see ARCH_MAX during slOnPluginLoad!");
        assert(ctx.getSystemCapsArch() == NV_GPU_ARCHITECTURE_TU100 && "Original Turing arch must be restored after plugin load!");

        std::printf("  [PASS] Test 2: Turing GPU (0x160) successfully spoofed to ARCH_MAX and restored\n");
    }

    // Test 3: Plugin load with Ampere GPU (0x170) and Ampere MFG active
    {
        MockConfig config;
        config.StreamlineSpoofing = true;
        config.FGDLSSGAmpereMfgUnlock = true;

        MockState state;
        state.externalFrameGeneration = true;
        state.activeFgInput = FGInput::NoFG;

        MockStreamlineContext ctx;
        bool loaded = ctx.simulatePluginLoad(config, state, NV_GPU_ARCHITECTURE_GA100);

        assert(loaded && "Plugin load on Ampere must observe spoofed Ada/Max arch!");
        assert(ctx.pluginLoadArchSeen == ARCH_MAX && "Plugin load must see ARCH_MAX during slOnPluginLoad!");
        assert(ctx.getSystemCapsArch() == NV_GPU_ARCHITECTURE_GA100 && "Original Ampere arch must be restored after plugin load!");

        std::printf("  [PASS] Test 3: Ampere GPU (0x170) successfully spoofed to ARCH_MAX and restored\n");
    }

    // Test 4: Ada GPU (0x190) should not have arch modified
    {
        MockConfig config;
        config.StreamlineSpoofing = true;
        config.FGDLSSGAmpereMfgUnlock = false;

        MockState state;
        state.activeFgInput = FGInput::DLSSG;

        MockStreamlineContext ctx;
        bool loaded = ctx.simulatePluginLoad(config, state, NV_GPU_ARCHITECTURE_AD100);

        assert(loaded && "Ada GPU should satisfy DLSS-G requirements natively!");
        assert(ctx.pluginLoadArchSeen == NV_GPU_ARCHITECTURE_AD100 && "Ada arch must remain intact (no spoofing needed)!");
        assert(ctx.getSystemCapsArch() == NV_GPU_ARCHITECTURE_AD100 && "Arch unchanged after plugin load!");

        std::printf("  [PASS] Test 4: Ada GPU (0x190) arch remains untouched\n");
    }

    // Test 5: StreamlineSpoofing disabled by user in INI
    {
        MockConfig config;
        config.StreamlineSpoofing = false;
        config.FGDLSSGAmpereMfgUnlock = true;

        MockState state;
        state.externalFrameGeneration = true;
        state.activeFgInput = FGInput::NoFG;

        MockStreamlineContext ctx;
        bool loaded = ctx.simulatePluginLoad(config, state, NV_GPU_ARCHITECTURE_TU100);

        assert(!loaded && "When StreamlineSpoofing is disabled, spoofing should not occur!");
        assert(ctx.pluginLoadArchSeen == NV_GPU_ARCHITECTURE_TU100 && "Plugin load should see unmodified Turing arch!");
        assert(ctx.getSystemCapsArch() == NV_GPU_ARCHITECTURE_TU100 && "Arch remains unchanged!");

        std::printf("  [PASS] Test 5: StreamlineSpoofing=false respects user preference\n");
    }

    // Test 6: Standard internal OptiScaler FG modes still spoof properly
    {
        MockConfig config;
        config.StreamlineSpoofing = true;
        config.FGDLSSGAmpereMfgUnlock = false;

        MockState state;
        state.activeFgInput = FGInput::NvngxFG;

        MockStreamlineContext ctx;
        bool loaded = ctx.simulatePluginLoad(config, state, NV_GPU_ARCHITECTURE_TU100);

        assert(loaded && "Internal NvngxFG mode still spoofs Turing arch!");
        assert(ctx.pluginLoadArchSeen == ARCH_MAX);

        state.activeFgInput = FGInput::DLSSG;
        loaded = ctx.simulatePluginLoad(config, state, NV_GPU_ARCHITECTURE_TU100);
        assert(loaded && "Internal DLSSG mode still spoofs Turing arch!");
        assert(ctx.pluginLoadArchSeen == ARCH_MAX);

        std::printf("  [PASS] Test 6: Internal FG modes retain proper spoofing\n");
    }

    std::printf("All Streamline Turing & Ampere spoofing unit tests passed successfully!\n");
    return 0;
}

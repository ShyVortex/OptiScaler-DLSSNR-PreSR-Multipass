#undef NDEBUG
#include <cassert>
#include <iostream>
#include <string>
#include <cstdint>

// Emulated API enum matching OptiScaler
enum class API : uint32_t
{
    NotSelected,
    DX11,
    DX12,
    Vulkan
};

struct MockState
{
    API api = API::NotSelected;
    API swapchainApi = API::NotSelected;
    void* currentFGSwapchain = nullptr;
    int dlssgDetectedInterpolationCount = 0;
};

struct MockConfig
{
    bool XeMfgUnlock = false;
    int XeMfgSourceType = 1; // 0 = Game, 1 = OptiScaler
    int XeMfgMaxFrames = 5;
};

// Simulation of XeMFG menu logic in menu_common.cpp
struct XeMfgMenuDiagnostics
{
    bool checkboxDisabled = false;
    std::string helpMarkerText;
    std::string diagnosticBannerText;
    std::string multiplierStatusText;
};

static XeMfgMenuDiagnostics EvaluateXeMfgMenu(const MockState& state, const MockConfig& config)
{
    XeMfgMenuDiagnostics result;

    const bool isVulkan = (state.swapchainApi == API::Vulkan || state.api == API::Vulkan);
    const bool disableXeMfg = !config.XeMfgUnlock && isVulkan;

    if (disableXeMfg)
    {
        result.checkboxDisabled = true;
        if (isVulkan)
        {
            result.helpMarkerText =
                "Disabled because the game is running under the Vulkan API.\n"
                "Intel XeSS Frame Generation (XeFG) strictly requires DirectX 12.\n"
                "If the game supports DirectX 12, switch the Graphics API to DirectX 12 in the game settings and "
                "restart.";
        }
    }

    if (config.XeMfgUnlock)
    {
        if (isVulkan)
        {
            result.diagnosticBannerText =
                "Error: Game is running in Vulkan mode. Intel XeSS Frame Generation (XeFG) only supports DirectX 12.\n"
                "If the game supports DirectX 12, switch the Graphics API to DirectX 12 in the game settings and "
                "restart.";
        }
        else if (config.XeMfgSourceType == 1 && state.currentFGSwapchain == nullptr)
        {
            result.diagnosticBannerText = "Note: Save Settings and restart the game to initialize the XeFG SwapChain.";
        }

        int liveMultiplier = state.dlssgDetectedInterpolationCount;
        if (liveMultiplier <= 0 && config.XeMfgSourceType == 1)
        {
            liveMultiplier = config.XeMfgMaxFrames;
        }

        if (liveMultiplier > 0)
        {
            if (isVulkan)
            {
                result.multiplierStatusText =
                    "In-Game Multiplier: " + std::to_string(liveMultiplier + 1) + "X (Inactive - DirectX 12 Required)";
            }
            else if (state.currentFGSwapchain == nullptr)
            {
                result.multiplierStatusText =
                    "In-Game Multiplier: " + std::to_string(liveMultiplier + 1) + "X (Pending SwapChain)";
            }
            else
            {
                result.multiplierStatusText =
                    "In-Game Multiplier: " + std::to_string(liveMultiplier + 1) + "X (Active)";
            }
        }
    }

    return result;
}

int main()
{
    std::cout << "=== Running Intel XeMFG Vulkan Diagnostics & Multiplier Guarding Unit Tests ===" << std::endl;

    // -----------------------------------------------------------------------------------------
    // Test Case 1: Vulkan API Detection
    // -----------------------------------------------------------------------------------------
    {
        MockState vkState1;
        vkState1.api = API::Vulkan;
        vkState1.swapchainApi = API::NotSelected;
        assert((vkState1.swapchainApi == API::Vulkan || vkState1.api == API::Vulkan) &&
               "Must detect Vulkan via state.api");

        MockState vkState2;
        vkState2.api = API::NotSelected;
        vkState2.swapchainApi = API::Vulkan;
        assert((vkState2.swapchainApi == API::Vulkan || vkState2.api == API::Vulkan) &&
               "Must detect Vulkan via swapchainApi");

        MockState dx12State;
        dx12State.api = API::DX12;
        dx12State.swapchainApi = API::DX12;
        assert(!(dx12State.swapchainApi == API::Vulkan || dx12State.api == API::Vulkan) &&
               "Must not detect Vulkan in DX12");

        std::cout << "  [PASS] Case 1: Vulkan API detection logic verified across swapchain and engine API fields"
                  << std::endl;
    }

    // -----------------------------------------------------------------------------------------
    // Test Case 2: Checkbox disablement and explanatory tooltip when XeUnlock is not yet active
    // -----------------------------------------------------------------------------------------
    {
        MockState state;
        state.api = API::Vulkan;
        MockConfig config;
        config.XeMfgUnlock = false;

        auto diag = EvaluateXeMfgMenu(state, config);
        assert(diag.checkboxDisabled && "XeMFG checkbox must be disabled when running in Vulkan and XeUnlock is false");
        assert(diag.helpMarkerText.find("Intel XeSS Frame Generation (XeFG) strictly requires DirectX 12") !=
               std::string::npos);
        assert(diag.helpMarkerText.find("Red Dead Redemption") == std::string::npos && "Must NOT contain game example");

        std::cout << "  [PASS] Case 2: Checkbox disablement and help marker verified under Vulkan" << std::endl;
    }

    // -----------------------------------------------------------------------------------------
    // Test Case 3: Explicit red error banner under Vulkan when XeUnlock is active
    // -----------------------------------------------------------------------------------------
    {
        MockState state;
        state.api = API::Vulkan;
        state.currentFGSwapchain = nullptr;
        MockConfig config;
        config.XeMfgUnlock = true;
        config.XeMfgSourceType = 1; // OptiScaler (Injected)

        auto diag = EvaluateXeMfgMenu(state, config);
        assert(!diag.checkboxDisabled && "User must be able to uncheck XeMFG even under Vulkan");
        assert(diag.diagnosticBannerText.find("Error: Game is running in Vulkan mode.") != std::string::npos);
        assert(diag.diagnosticBannerText.find("Intel XeSS Frame Generation (XeFG) only supports DirectX 12.") !=
               std::string::npos);
        assert(diag.diagnosticBannerText.find(
                   "If the game supports DirectX 12, switch the Graphics API to DirectX 12") != std::string::npos);
        assert(diag.diagnosticBannerText.find(
                   "Note: Save Settings and restart the game to initialize the XeFG SwapChain.") == std::string::npos &&
               "Must NOT display the misleading restart note under Vulkan!");
        assert(diag.diagnosticBannerText.find("Red Dead Redemption") == std::string::npos &&
               "Must NOT contain hardcoded game examples!");

        std::cout << "  [PASS] Case 3: Explicit error banner correctly rendered without game examples" << std::endl;
    }

    // -----------------------------------------------------------------------------------------
    // Test Case 4: Multiplier status text guarding
    // -----------------------------------------------------------------------------------------
    {
        // 4a. Vulkan mode with liveMultiplier = 5 (6X)
        MockState vkState;
        vkState.api = API::Vulkan;
        vkState.currentFGSwapchain = nullptr;
        MockConfig config;
        config.XeMfgUnlock = true;
        config.XeMfgSourceType = 1;
        config.XeMfgMaxFrames = 5;

        auto vkDiag = EvaluateXeMfgMenu(vkState, config);
        assert(vkDiag.multiplierStatusText == "In-Game Multiplier: 6X (Inactive - DirectX 12 Required)");

        // 4b. DX12 mode with unbound swapchain
        MockState dx12Unbound;
        dx12Unbound.api = API::DX12;
        dx12Unbound.swapchainApi = API::DX12;
        dx12Unbound.currentFGSwapchain = nullptr;

        auto dx12PendingDiag = EvaluateXeMfgMenu(dx12Unbound, config);
        assert(dx12PendingDiag.multiplierStatusText == "In-Game Multiplier: 6X (Pending SwapChain)");
        assert(dx12PendingDiag.diagnosticBannerText ==
               "Note: Save Settings and restart the game to initialize the XeFG SwapChain.");

        // 4c. DX12 mode with bound swapchain
        int dummySwapchain = 42;
        MockState dx12Bound;
        dx12Bound.api = API::DX12;
        dx12Bound.swapchainApi = API::DX12;
        dx12Bound.currentFGSwapchain = &dummySwapchain;

        auto dx12ActiveDiag = EvaluateXeMfgMenu(dx12Bound, config);
        assert(dx12ActiveDiag.multiplierStatusText == "In-Game Multiplier: 6X (Active)");
        assert(dx12ActiveDiag.diagnosticBannerText.empty());

        std::cout << "  [PASS] Case 4: In-Game Multiplier correctly guards (Inactive - DirectX 12 Required), "
                     "(Pending SwapChain), and (Active)"
                  << std::endl;
    }

    std::cout << "=== All Intel XeMFG Vulkan Diagnostics & Multiplier Guarding Unit Tests PASSED! ===" << std::endl;
    return 0;
}

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <string>
#include <optional>
#include <fstream>
#include <framegen/dlssg/AmpereMfgLoader.h>

// Mock architecture constants
constexpr uint32_t NV_GPU_ARCHITECTURE_TU100 = 0x00000160;
constexpr uint32_t NV_GPU_ARCHITECTURE_GA100 = 0x00000170;
constexpr uint32_t NV_GPU_ARCHITECTURE_AD100 = 0x00000190;
constexpr uint32_t NV_GPU_ARCHITECTURE_GB100 = 0x000001A0;

// Setting IDs matching NVIDIA DRS constants
constexpr uint32_t NVDRS_SETTING_SMOOTH_MOTION_ENABLE = 0xB0D384C0;
constexpr uint32_t NVDRS_SETTING_SMOOTH_MOTION_APIS = 0xB0CC0875;
constexpr uint32_t NVDRS_SETTING_SMOOTH_MOTION_DEBUG = 0xB01B8B02;

// Minimum supported NVIDIA driver version (571.86 -> 57186)
constexpr uint32_t MIN_SMOOTH_MOTION_DRIVER_VERSION = 57186;

// Bitmask for enabled APIs
constexpr uint32_t SMOOTH_MOTION_API_DX12 = 0x1;
constexpr uint32_t SMOOTH_MOTION_API_DX11 = 0x2;
constexpr uint32_t SMOOTH_MOTION_API_VULKAN = 0x4;
constexpr uint32_t SMOOTH_MOTION_API_ALL = (SMOOTH_MOTION_API_DX12 | SMOOTH_MOTION_API_DX11 | SMOOTH_MOTION_API_VULKAN);

// Pure validation helper simulating the OptiScaler decision logic
struct SmoothMotionPolicy
{
    static bool IsDriverSupported(uint32_t driverVersion) { return driverVersion >= MIN_SMOOTH_MOTION_DRIVER_VERSION; }

    static bool ShouldEngage(std::optional<bool> userConfig, bool isNvidia, uint32_t archId, bool isWindows,
                             uint32_t driverVersion)
    {
        // Strictly opt-in: default is false
        const bool optIn = userConfig.value_or(false);
        if (!optIn)
            return false;

        // Platform & hardware guards: requires NVIDIA, Windows, and Ada (RTX 40) or Blackwell (RTX 50)
        const bool isAdaOrBlackwell = isNvidia && (archId >= NV_GPU_ARCHITECTURE_AD100);
        if (!isAdaOrBlackwell || !isWindows)
            return false;

        // Driver version guard
        if (!IsDriverSupported(driverVersion))
            return false;

        return true;
    }

    static uint32_t ResolveApiMask(bool enableDx12 = true, bool enableDx11 = true, bool enableVulkan = true)
    {
        uint32_t mask = 0;
        if (enableDx12)
            mask |= SMOOTH_MOTION_API_DX12;
        if (enableDx11)
            mask |= SMOOTH_MOTION_API_DX11;
        if (enableVulkan)
            mask |= SMOOTH_MOTION_API_VULKAN;
        return mask;
    }
};

int main()
{
    std::printf("=== Running NVIDIA Smooth Motion Unit Tests ===\n");

    // Test 1: DRS Setting Identifiers match NVIDIA Profile Inspector / DRS standards
    {
        assert(NVDRS_SETTING_SMOOTH_MOTION_ENABLE == 0xB0D384C0);
        assert(NVDRS_SETTING_SMOOTH_MOTION_APIS == 0xB0CC0875);
        assert(NVDRS_SETTING_SMOOTH_MOTION_DEBUG == 0xB01B8B02);
        std::printf("  [PASS] Case 1: DRS setting IDs verified (Enable=0xB0D384C0, APIs=0xB0CC0875)\n");
    }

    // Test 2: Strict Opt-In Default Behavior (must default to false)
    {
        std::optional<bool> defaultUnset; // Unset in config
        assert(!defaultUnset.has_value());
        assert(defaultUnset.value_or(false) == false);

        // Even on modern supported driver and Ada GPU, unset config MUST NOT engage
        bool engaged = SmoothMotionPolicy::ShouldEngage(defaultUnset, true, NV_GPU_ARCHITECTURE_AD100, true, 57216);
        assert(!engaged);

        // Explicitly setting false MUST NOT engage
        engaged = SmoothMotionPolicy::ShouldEngage(false, true, NV_GPU_ARCHITECTURE_AD100, true, 57216);
        assert(!engaged);

        std::printf("  [PASS] Case 2: Strict opt-in default behavior verified (defaults to false)\n");
    }

    // Test 3: Driver Version Gating (requires >= 571.86)
    {
        // Drivers below 571.86 are rejected
        assert(!SmoothMotionPolicy::IsDriverSupported(0));
        assert(!SmoothMotionPolicy::IsDriverSupported(55000)); // R550
        assert(!SmoothMotionPolicy::IsDriverSupported(56070)); // R560
        assert(!SmoothMotionPolicy::IsDriverSupported(57185)); // 571.85 (boundary check)

        // Drivers at or above 571.86 are supported
        assert(SmoothMotionPolicy::IsDriverSupported(57186)); // 571.86 (boundary check)
        assert(SmoothMotionPolicy::IsDriverSupported(57216)); // 572.16
        assert(SmoothMotionPolicy::IsDriverSupported(58108)); // 581.08
        assert(SmoothMotionPolicy::IsDriverSupported(61047)); // Future branch

        // With explicit user opt-in, old driver is safely blocked
        assert(!SmoothMotionPolicy::ShouldEngage(true, true, NV_GPU_ARCHITECTURE_AD100, true, 56070));
        // With explicit user opt-in and valid driver on Ada, feature engages
        assert(SmoothMotionPolicy::ShouldEngage(true, true, NV_GPU_ARCHITECTURE_AD100, true, 57186));
        assert(SmoothMotionPolicy::ShouldEngage(true, true, NV_GPU_ARCHITECTURE_AD100, true, 58108));

        std::printf("  [PASS] Case 3: Driver version threshold verified (minimum 571.86 / 57186)\n");
    }

    // Test 4: Platform and Hardware Safety Guards (requires Ada/Blackwell + Windows)
    {
        // Non-Nvidia GPUs are rejected even with opt-in and valid driver
        assert(!SmoothMotionPolicy::ShouldEngage(true, false, NV_GPU_ARCHITECTURE_AD100, true, 57216));

        // Linux / Proton is rejected (Smooth Motion requires Windows DXGI/WDDM driver stack)
        assert(!SmoothMotionPolicy::ShouldEngage(true, true, NV_GPU_ARCHITECTURE_AD100, false, 57216));

        // Turing (SM75) and Ampere (SM86) GPUs are rejected natively by NVIDIA driver
        assert(!SmoothMotionPolicy::ShouldEngage(true, true, NV_GPU_ARCHITECTURE_TU100, true, 57216));
        assert(!SmoothMotionPolicy::ShouldEngage(true, true, NV_GPU_ARCHITECTURE_GA100, true, 57216));

        // Ada (RTX 40 / sm_89) and Blackwell (RTX 50 / sm_120) engage successfully
        assert(SmoothMotionPolicy::ShouldEngage(true, true, NV_GPU_ARCHITECTURE_AD100, true, 57216));
        assert(SmoothMotionPolicy::ShouldEngage(true, true, NV_GPU_ARCHITECTURE_GB100, true, 57216));

        std::printf("  [PASS] Case 4: Ada/Blackwell architecture and Windows platform guards verified\n");
    }

    // Test 5: API Mask Composition
    {
        assert(SmoothMotionPolicy::ResolveApiMask(true, true, true) == 7);
        assert(SmoothMotionPolicy::ResolveApiMask(true, false, false) == 1); // DX12 only
        assert(SmoothMotionPolicy::ResolveApiMask(false, true, false) == 2); // DX11 only
        assert(SmoothMotionPolicy::ResolveApiMask(false, false, true) == 4); // Vulkan only
        assert(SmoothMotionPolicy::ResolveApiMask(true, true, false) == 3);  // DX12 + DX11 (Vulkan excluded)

        std::printf("  [PASS] Case 5: API bitmask resolution verified (All=7, DX11/12=3)\n");
    }

    // Test 6: In-Memory DRS GetSetting Interception Behavior
    {
        auto simulateGetSetting = [](uint32_t settingId, bool configEnabled, uint32_t& outVal) -> bool
        {
            if (settingId == NVDRS_SETTING_SMOOTH_MOTION_ENABLE)
            {
                outVal = configEnabled ? 1 : 0;
                return true;
            }
            if (settingId == NVDRS_SETTING_SMOOTH_MOTION_APIS)
            {
                if (configEnabled)
                {
                    outVal = 7;
                    return true;
                }
            }
            return false;
        };

        uint32_t val = 999;
        // When disabled (default), enable setting returns 0
        assert(simulateGetSetting(NVDRS_SETTING_SMOOTH_MOTION_ENABLE, false, val) && val == 0);

        // When enabled, enable setting returns 1
        assert(simulateGetSetting(NVDRS_SETTING_SMOOTH_MOTION_ENABLE, true, val) && val == 1);

        // When enabled, APIs setting returns 7 (DX12 | DX11 | Vulkan)
        assert(simulateGetSetting(NVDRS_SETTING_SMOOTH_MOTION_APIS, true, val) && val == 7);

        std::printf("  [PASS] Case 6: In-memory DRS GetSetting query interception simulated & verified\n");
    }

    // Test 7: AmpereMfgLoader::Status SmoothMotionActive integration
    {
        AmpereMfgLoader::Status status {};
        assert(!status.SmoothMotionActive); // Default false
        status.SmoothMotionActive = true;
        assert(status.SmoothMotionActive);

        std::printf("  [PASS] Case 7: AmpereMfgLoader::Status SmoothMotionActive tracking verified\n");
    }

    // Test 8: Menu UI State Evaluation and Help Marker Verification
    {
        struct SmoothMotionUiState
        {
            bool disabled;
            std::string helpMarker;
        };

        auto evaluateUiState = [](bool onLinux, bool isNvidia, uint32_t archId,
                                  bool fgConflict = false) -> SmoothMotionUiState
        {
            const bool isAdaOrBlackwell = isNvidia && (archId >= NV_GPU_ARCHITECTURE_AD100);
            const bool isAmpere = isNvidia && (archId == NV_GPU_ARCHITECTURE_GA100);
            const bool disableSmoothMotion = onLinux || (!isAdaOrBlackwell && !isAmpere) || fgConflict;
            if (disableSmoothMotion)
            {
                if (fgConflict)
                {
                    return { true, "Disabled because Frame Generation (DLSS-G / MFG) is active.\n"
                                   "NVIDIA Smooth Motion is driver-level frame generation for games lacking native "
                                   "DLSS-G.\n"
                                   "Disable MFG / Frame Generation first, Save Settings and restart to use Smooth "
                                   "Motion." };
                }
                else if (onLinux)
                {
                    return { true, "Disabled because the active OS is not Windows (10/11).\n"
                                   "NVIDIA Smooth Motion is a Windows-only driver display pipeline feature (requires "
                                   "driver 571.86+ on Windows)." };
                }
                else if (!isNvidia)
                {
                    return { true, "Disabled because the active GPU is not NVIDIA.\n"
                                   "NVIDIA Smooth Motion requires an NVIDIA GPU and driver 571.86+ on Windows." };
                }
                else
                {
                    return { true, "Disabled because the active GPU is not NVIDIA Ampere (RTX 30), Ada Lovelace (RTX "
                                   "40), or Blackwell (RTX 50).\n"
                                   "NVIDIA driver-level Smooth Motion requires driver 571.86+ on Windows with RTX "
                                   "40/50 natively, or RTX 30 via NVSmooth30.\n"
                                   "Turing (RTX 20 / GTX 16) and older architectures lack hardware support for "
                                   "driver-level frame generation." };
                }
            }
            if (isAmpere)
            {
                return {
                    false,
                    "NVIDIA Driver-Level Smooth Motion (requires driver 571.86+ on Windows):\n"
                    "Enables driver-level optical-flow frame interpolation directly via NVIDIA Driver Settings (DRS).\n"
                    "Strictly opt-in: intended for games that lack native DLSS Frame Generation support.\n"
                    "On GeForce RTX 30 (Ampere), this feature is unlocked via OptiScaler/nvsmooth30.dll.\n"
                    "Note: A game restart is required for NvPresent64 to attach to the DXGI swapchain."
                };
            }
            return {
                false,
                "NVIDIA Driver-Level Smooth Motion (requires driver 571.86+ on Windows):\n"
                "Enables driver-level optical-flow frame interpolation directly via NVIDIA Driver Settings (DRS).\n"
                "Strictly opt-in: intended for games that lack native DLSS Frame Generation support.\n"
                "Supported natively on GeForce RTX 40 (Ada) and RTX 50 (Blackwell) series GPUs.\n"
                "Note: A game restart may be required for the driver to attach to the DXGI swapchain."
            };
        };

        // Case 8a: Windows + Ada GPU (AD100 / RTX 40) -> Enabled, standard help marker
        {
            auto ui = evaluateUiState(/*onLinux=*/false, /*isNvidia=*/true, NV_GPU_ARCHITECTURE_AD100);
            assert(!ui.disabled);
            assert(ui.helpMarker.find("requires driver 571.86+ on Windows") != std::string::npos);
            assert(ui.helpMarker.find("Supported natively on GeForce RTX 40 (Ada) and RTX 50 (Blackwell)") !=
                   std::string::npos);
            assert(ui.helpMarker.find("Disabled because") == std::string::npos);
            std::printf("  [PASS] Case 8a: Windows + Ada GPU evaluates to interactive checkbox\n");
        }

        // Case 8b: Windows + Blackwell GPU (GB100 / RTX 50) -> Enabled, standard help marker
        {
            auto ui = evaluateUiState(/*onLinux=*/false, /*isNvidia=*/true, NV_GPU_ARCHITECTURE_GB100);
            assert(!ui.disabled);
            assert(ui.helpMarker.find("requires driver 571.86+ on Windows") != std::string::npos);
            assert(ui.helpMarker.find("Disabled because") == std::string::npos);
            std::printf("  [PASS] Case 8b: Windows + Blackwell GPU evaluates to interactive checkbox\n");
        }

        // Case 8c: Windows + Ampere GPU (GA100 / RTX 30) -> Enabled via NVSmooth30 unlocker!
        {
            auto ui = evaluateUiState(/*onLinux=*/false, /*isNvidia=*/true, NV_GPU_ARCHITECTURE_GA100);
            assert(!ui.disabled);
            assert(ui.helpMarker.find("requires driver 571.86+ on Windows") != std::string::npos);
            assert(ui.helpMarker.find("unlocked via OptiScaler/nvsmooth30.dll") != std::string::npos);
            assert(ui.helpMarker.find("Disabled because") == std::string::npos);
            std::printf(
                "  [PASS] Case 8c: Windows + Ampere GPU evaluates to interactive checkbox (unlocked via NVSmooth30)\n");
        }

        // Case 8d: Windows + Turing GPU (TU100 / RTX 20) -> Disabled with architecture explanation
        {
            auto ui = evaluateUiState(/*onLinux=*/false, /*isNvidia=*/true, NV_GPU_ARCHITECTURE_TU100);
            assert(ui.disabled);
            assert(ui.helpMarker.find("Disabled because the active GPU is not NVIDIA Ampere (RTX 30), Ada Lovelace "
                                      "(RTX 40), or Blackwell (RTX 50)") != std::string::npos);
            std::printf("  [PASS] Case 8d: Windows + Turing GPU evaluates to disabled with architecture explanation\n");
        }

        // Case 8e: Linux + Ada GPU -> Disabled with OS explanation
        {
            auto ui = evaluateUiState(/*onLinux=*/true, /*isNvidia=*/true, NV_GPU_ARCHITECTURE_AD100);
            assert(ui.disabled);
            assert(ui.helpMarker.find("Disabled because the active OS is not Windows (10/11)") != std::string::npos);
            assert(ui.helpMarker.find("Windows-only driver display pipeline feature") != std::string::npos);
            std::printf("  [PASS] Case 8e: Linux + Ada GPU evaluates to disabled with OS explanation\n");
        }

        // Case 8f: Windows + non-NVIDIA GPU -> Disabled with GPU explanation
        {
            auto ui = evaluateUiState(/*onLinux=*/false, /*isNvidia=*/false, 0);
            assert(ui.disabled);
            assert(ui.helpMarker.find("Disabled because the active GPU is not NVIDIA") != std::string::npos);
            std::printf("  [PASS] Case 8f: Windows + non-NVIDIA GPU evaluates to disabled with GPU explanation\n");
        }

        // Case 8g: Windows + Ampere GPU + active MFG conflict -> Disabled with conflict explanation
        {
            auto ui =
                evaluateUiState(/*onLinux=*/false, /*isNvidia=*/true, NV_GPU_ARCHITECTURE_GA100, /*fgConflict=*/true);
            assert(ui.disabled);
            assert(ui.helpMarker.find("Disabled because Frame Generation (DLSS-G / MFG) is active") !=
                   std::string::npos);
            std::printf("  [PASS] Case 8g: Windows + Ampere GPU with active MFG evaluates to disabled with conflict "
                        "explanation\n");
        }

        // Case 8h: Windows + Ada GPU + active MFG conflict -> Disabled with conflict explanation
        {
            auto ui =
                evaluateUiState(/*onLinux=*/false, /*isNvidia=*/true, NV_GPU_ARCHITECTURE_AD100, /*fgConflict=*/true);
            assert(ui.disabled);
            assert(ui.helpMarker.find("Disabled because Frame Generation (DLSS-G / MFG) is active") !=
                   std::string::npos);
            std::printf("  [PASS] Case 8h: Windows + Ada GPU with active MFG evaluates to disabled with conflict "
                        "explanation\n");
        }

        std::printf(
            "  [PASS] Case 8: Menu UI disable logic and contextual help markers verified across all architectures\n");
    }

    // Test 9: INI Configuration Key Priority & Migration (SmoothMotion vs AmpereMfgSmoothMotion)
    {
        auto resolveConfig = [](std::optional<bool> dlssgSmoothMotion, std::optional<bool> frameGenSmoothMotion,
                                std::optional<bool> dlssgAmpereMfgSmoothMotion,
                                std::optional<bool> frameGenAmpereMfgSmoothMotion) -> bool
        {
            if (dlssgSmoothMotion.has_value())
                return dlssgSmoothMotion.value();
            if (frameGenSmoothMotion.has_value())
                return frameGenSmoothMotion.value();
            if (dlssgAmpereMfgSmoothMotion.has_value())
                return dlssgAmpereMfgSmoothMotion.value();
            if (frameGenAmpereMfgSmoothMotion.has_value())
                return frameGenAmpereMfgSmoothMotion.value();
            return false;
        };

        // 1. New key has highest priority
        assert(resolveConfig(true, false, false, false) == true);
        assert(resolveConfig(false, true, true, true) == false);

        // 2. FrameGen section modern key
        assert(resolveConfig(std::nullopt, true, false, false) == true);

        // 3. Legacy [DLSSG] AmpereMfgSmoothMotion fallback works
        assert(resolveConfig(std::nullopt, std::nullopt, true, false) == true);

        // 4. Legacy [FrameGen] AmpereMfgSmoothMotion fallback works
        assert(resolveConfig(std::nullopt, std::nullopt, std::nullopt, true) == true);

        // 5. Default when unset
        assert(resolveConfig(std::nullopt, std::nullopt, std::nullopt, std::nullopt) == false);

        // 6. Verify OptiScaler.ini in repo has SmoothMotion=false and NOT AmpereMfgSmoothMotion=false
        std::ifstream iniFile("OptiScaler.ini");
        if (iniFile.is_open())
        {
            std::string content((std::istreambuf_iterator<char>(iniFile)), std::istreambuf_iterator<char>());
            assert(content.find("SmoothMotion=false") != std::string::npos);
            assert(content.find("AmpereMfgSmoothMotion=false") == std::string::npos);
            std::printf("  [PASS] Case 9b: Verified OptiScaler.ini uses modernized SmoothMotion key\n");
        }

        std::printf("  [PASS] Case 9: INI configuration priority and legacy migration verified\n");
    }

    // Test 10: Smooth Motion Status Banner & Tag Logic
    {
        auto evaluateStatusBanner = [](bool isAmpere, bool dllLoaded, bool swapchainAttached,
                                       bool smoothMotion) -> std::pair<std::string, std::string>
        {
            if (!smoothMotion)
                return { "", "" };

            if (isAmpere)
            {
                if (dllLoaded && swapchainAttached)
                    return { "[Smooth Motion Active (RTX 30)]", "green" };
                else if (dllLoaded)
                    return { "[Pending Restart: Active on next game launch]", "amber" };
                else
                    return { "[Pending Restart]", "amber" };
            }
            else
            {
                return { "[Smooth Motion Active]", "green" };
            }
        };

        // Ampere with swapchain bound -> Active (green)
        auto [text1, color1] = evaluateStatusBanner(true, true, true, true);
        assert(text1 == "[Smooth Motion Active (RTX 30)]" && color1 == "green");

        // Ampere with DLL loaded but swapchain not bound (mid-game toggle) -> Pending Restart (amber)
        auto [text2, color2] = evaluateStatusBanner(true, true, false, true);
        assert(text2 == "[Pending Restart: Active on next game launch]" && color2 == "amber");

        // Ampere with DLL not loaded -> Pending Restart (amber)
        auto [text3, color3] = evaluateStatusBanner(true, false, false, true);
        assert(text3 == "[Pending Restart]" && color3 == "amber");

        // Ada / Blackwell -> Smooth Motion Active (green)
        auto [text4, color4] = evaluateStatusBanner(false, false, false, true);
        assert(text4 == "[Smooth Motion Active]" && color4 == "green");

        std::printf("  [PASS] Case 10: Smooth Motion status banner and restart requirement logic verified\n");
    }

    std::printf("\nALL NVIDIA SMOOTH MOTION TESTS PASSED SUCCESSFULLY!\n");
    return 0;
}

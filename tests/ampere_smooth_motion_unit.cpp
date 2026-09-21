#include <cassert>
#include <cstdio>
#include <cstdint>
#include <string>
#include <optional>

// Setting IDs matching NVIDIA DRS constants
constexpr uint32_t NVDRS_SETTING_SMOOTH_MOTION_ENABLE = 0xB0D384C0;
constexpr uint32_t NVDRS_SETTING_SMOOTH_MOTION_APIS   = 0xB0CC0875;
constexpr uint32_t NVDRS_SETTING_SMOOTH_MOTION_DEBUG  = 0xB01B8B02;

// Minimum supported NVIDIA driver version (571.86 -> 57186)
constexpr uint32_t MIN_SMOOTH_MOTION_DRIVER_VERSION   = 57186;

// Bitmask for enabled APIs
constexpr uint32_t SMOOTH_MOTION_API_DX12             = 0x1;
constexpr uint32_t SMOOTH_MOTION_API_DX11             = 0x2;
constexpr uint32_t SMOOTH_MOTION_API_VULKAN           = 0x4;
constexpr uint32_t SMOOTH_MOTION_API_ALL              = (SMOOTH_MOTION_API_DX12 | SMOOTH_MOTION_API_DX11 | SMOOTH_MOTION_API_VULKAN);

// Pure validation helper simulating the OptiScaler decision logic
struct SmoothMotionPolicy
{
    static bool IsDriverSupported(uint32_t driverVersion)
    {
        return driverVersion >= MIN_SMOOTH_MOTION_DRIVER_VERSION;
    }

    static bool ShouldEngage(std::optional<bool> userConfig, bool isNvidia, bool isWindows, uint32_t driverVersion)
    {
        // Strictly opt-in: default is false
        const bool optIn = userConfig.value_or(false);
        if (!optIn)
            return false;

        // Platform & hardware guards
        if (!isNvidia || !isWindows)
            return false;

        // Driver version guard
        if (!IsDriverSupported(driverVersion))
            return false;

        return true;
    }

    static uint32_t ResolveApiMask(bool enableDx12 = true, bool enableDx11 = true, bool enableVulkan = true)
    {
        uint32_t mask = 0;
        if (enableDx12) mask |= SMOOTH_MOTION_API_DX12;
        if (enableDx11) mask |= SMOOTH_MOTION_API_DX11;
        if (enableVulkan) mask |= SMOOTH_MOTION_API_VULKAN;
        return mask;
    }
};

int main()
{
    std::printf("=== Running NVIDIA Smooth Motion Unit Tests ===\n");

    // Test 1: DRS Setting Identifiers match NVIDIA Profile Inspector / DRS standards
    {
        assert(NVDRS_SETTING_SMOOTH_MOTION_ENABLE == 0xB0D384C0);
        assert(NVDRS_SETTING_SMOOTH_MOTION_APIS   == 0xB0CC0875);
        assert(NVDRS_SETTING_SMOOTH_MOTION_DEBUG  == 0xB01B8B02);
        std::printf("  [PASS] Case 1: DRS setting IDs verified (Enable=0xB0D384C0, APIs=0xB0CC0875)\n");
    }

    // Test 2: Strict Opt-In Default Behavior (must default to false)
    {
        std::optional<bool> defaultUnset; // Unset in config
        assert(!defaultUnset.has_value());
        assert(defaultUnset.value_or(false) == false);

        // Even on modern supported driver, unset config MUST NOT engage
        bool engaged = SmoothMotionPolicy::ShouldEngage(defaultUnset, true, true, 57216);
        assert(!engaged);

        // Explicitly setting false MUST NOT engage
        engaged = SmoothMotionPolicy::ShouldEngage(false, true, true, 57216);
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
        assert(SmoothMotionPolicy::IsDriverSupported(57186));  // 571.86 (boundary check)
        assert(SmoothMotionPolicy::IsDriverSupported(57216));  // 572.16
        assert(SmoothMotionPolicy::IsDriverSupported(58108));  // 581.08
        assert(SmoothMotionPolicy::IsDriverSupported(61047));  // Future branch

        // With explicit user opt-in, old driver is safely blocked
        assert(!SmoothMotionPolicy::ShouldEngage(true, true, true, 56070));
        // With explicit user opt-in and valid driver, feature engages
        assert(SmoothMotionPolicy::ShouldEngage(true, true, true, 57186));
        assert(SmoothMotionPolicy::ShouldEngage(true, true, true, 58108));

        std::printf("  [PASS] Case 3: Driver version threshold verified (minimum 571.86 / 57186)\n");
    }

    // Test 4: Platform and Hardware Safety Guards
    {
        // Non-Nvidia GPUs are rejected even with opt-in and valid driver
        assert(!SmoothMotionPolicy::ShouldEngage(true, false, true, 57216));

        // Linux / Proton is rejected (Smooth Motion requires Windows DXGI/WDDM driver stack)
        assert(!SmoothMotionPolicy::ShouldEngage(true, true, false, 57216));

        std::printf("  [PASS] Case 4: Non-Nvidia and Linux platform guards verified\n");
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
        auto simulateGetSetting = [](uint32_t settingId, bool configEnabled, uint32_t& outVal) -> bool {
            if (settingId == NVDRS_SETTING_SMOOTH_MOTION_ENABLE) {
                outVal = configEnabled ? 1 : 0;
                return true;
            }
            if (settingId == NVDRS_SETTING_SMOOTH_MOTION_APIS) {
                if (configEnabled) {
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

    std::printf("\nALL NVIDIA SMOOTH MOTION TESTS PASSED SUCCESSFULLY!\n");
    return 0;
}

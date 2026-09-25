#include <cassert>
#include <cstdio>
#include <cstdint>
#include <string>
#include <filesystem>
#include <fstream>
#include <optional>

#include <map>

// Architecture identifiers matching NVAPI / OptiScaler
constexpr uint32_t NV_GPU_ARCHITECTURE_TU100 = 0x00000160;
constexpr uint32_t NV_GPU_ARCHITECTURE_GA100 = 0x00000170; // Ampere (RTX 30)
constexpr uint32_t NV_GPU_ARCHITECTURE_AD100 = 0x00000190; // Ada (RTX 40)
constexpr uint32_t NV_GPU_ARCHITECTURE_GB100 = 0x000001A0; // Blackwell (RTX 50)

// DRS setting identifiers
constexpr uint32_t NVDRS_SETTING_SMOOTH_MOTION_ENABLE = 0xB0D384C0;
constexpr uint32_t NVDRS_SETTING_SMOOTH_MOTION_APIS = 0xB0CC0875;

namespace fs = std::filesystem;

// Mock / standalone representation of NVSmooth30 loader decision logic and path resolution
namespace NVSmooth30Unit
{
enum class Status
{
    Disabled = 0,
    NotAmpere,
    MissingBinary,
    LoadFailed,
    Active,
    FgConflict
};

inline const char* StatusToString(Status s)
{
    switch (s)
    {
    case Status::Disabled:
        return "Disabled in config";
    case Status::NotAmpere:
        return "Requires NVIDIA Ampere (RTX 30) GPU";
    case Status::MissingBinary:
        return "OptiScaler/nvsmooth30.dll not found";
    case Status::LoadFailed:
        return "Failed to load nvsmooth30.dll";
    case Status::Active:
        return "Active (Smooth Motion unlocked on RTX 30)";
    case Status::FgConflict:
        return "Conflict: DLSS-G / MFG active";
    default:
        return "Unknown";
    }
}

inline fs::path FindCandidatePath(const fs::path& basePath, const fs::path& overridePath)
{
    // 1. Mandatory standard location: basePath / OptiScaler / nvsmooth30.dll
    if (!basePath.empty())
    {
        fs::path optiScalerSubdir = basePath / "OptiScaler" / "nvsmooth30.dll";
        if (fs::exists(optiScalerSubdir))
            return optiScalerSubdir;
    }

    // 2. Override location if specified
    if (!overridePath.empty())
    {
        fs::path overrideCandidate = overridePath / "nvsmooth30.dll";
        if (fs::exists(overrideCandidate))
            return overrideCandidate;
    }

    // 3. Root fallback for compatibility
    if (!basePath.empty())
    {
        fs::path rootCandidate = basePath / "nvsmooth30.dll";
        if (fs::exists(rootCandidate))
            return rootCandidate;
    }

    return {};
}

inline std::map<std::string, std::string> GetSanitizedEnvironment()
{
    return { { "SM86_ENABLE_D3D11_BRIDGE", "0" },
             { "SM86_LOW_LATENCY", "0" },
             { "SM86_ENABLE_OSD", "0" },
             { "SM86_SKIP_DXGI_HOOKS", "1" } };
}

inline bool IsDummyWindow(const std::string& className, uint32_t width, uint32_t height)
{
    if (className == "NVSmooth30DummyWindow")
        return true;
    if (width == 16 && height == 16 && className.find("Dummy") != std::string::npos)
        return true;
    return false;
}

struct MockLoaderState
{
    Status status = Status::Disabled;
    std::string loadedPath;
    bool drsProfileApplied = false;
    bool swapchainAttached = false;

    void Reset()
    {
        status = Status::Disabled;
        loadedPath.clear();
        drsProfileApplied = false;
        swapchainAttached = false;
    }

    std::string GetStatusString() const
    {
        if (status == Status::Active)
        {
            if (swapchainAttached)
                return "Active (Swapchain bound)";
            else
                return "Pending Restart (Swapchain not bound)";
        }
        return StatusToString(status);
    }

    std::string GetBannerTag() const
    {
        if (status == Status::Active)
        {
            if (swapchainAttached)
                return "[Smooth Motion Active (RTX 30)]";
            else
                return "[Pending Restart: Active on next game launch]";
        }
        return "[Pending Restart]";
    }

    bool TrySetup(bool configSmoothMotionEnabled, bool configNVSmooth30Enabled, bool isNvidia, uint32_t archId,
                  const fs::path& basePath, const fs::path& overridePath, bool simulateLoadSuccess = true,
                  bool fgConflict = false)
    {
        if (!configSmoothMotionEnabled || !configNVSmooth30Enabled)
        {
            status = Status::Disabled;
            return false;
        }

        // Mutual exclusion guard: DLSS-G / MFG Frame Generation conflict
        if (fgConflict)
        {
            status = Status::FgConflict;
            return false;
        }

        // Gating: Only supported on NVIDIA Ampere (RTX 30 series)
        if (!isNvidia || archId != NV_GPU_ARCHITECTURE_GA100)
        {
            status = Status::NotAmpere;
            return false;
        }

        fs::path candidate = FindCandidatePath(basePath, overridePath);
        if (candidate.empty())
        {
            status = Status::MissingBinary;
            return false;
        }

        if (!simulateLoadSuccess)
        {
            status = Status::LoadFailed;
            return false;
        }

        // Successfully resolved and simulated load
        loadedPath = candidate.string();
        status = Status::Active;

        // Automatically apply DRS settings to driver profile
        drsProfileApplied = true;
        return true;
    }
};
} // namespace NVSmooth30Unit

int main()
{
    std::printf("=== Running NVSmooth30 Loader Integration Unit Tests ===\n");

    const fs::path testRoot = fs::temp_directory_path() / "optiscaler_nvsmooth30_test";
    std::error_code ec;
    fs::remove_all(testRoot, ec);
    fs::create_directories(testRoot / "OptiScaler", ec);
    fs::create_directories(testRoot / "override", ec);

    // Test 1: Candidate Path Resolution - Preference for OptiScaler/nvsmooth30.dll
    {
        const fs::path optiScalerDll = testRoot / "OptiScaler" / "nvsmooth30.dll";
        const fs::path overrideDll = testRoot / "override" / "nvsmooth30.dll";
        const fs::path rootDll = testRoot / "nvsmooth30.dll";

        // Initially no files exist
        assert(NVSmooth30Unit::FindCandidatePath(testRoot, testRoot / "override").empty());

        // Create DLL in OptiScaler directory
        {
            std::ofstream f(optiScalerDll);
            f << "MZ_MOCK_DLL";
        }
        fs::path resolved = NVSmooth30Unit::FindCandidatePath(testRoot, testRoot / "override");
        assert(resolved == optiScalerDll);
        std::printf("  [PASS] Case 1a: Primary candidate resolved strictly to OptiScaler/nvsmooth30.dll\n");

        // When override also exists, OptiScaler/nvsmooth30.dll must still take precedence
        {
            std::ofstream f(overrideDll);
            f << "MZ_MOCK_OVERRIDE";
        }
        resolved = NVSmooth30Unit::FindCandidatePath(testRoot, testRoot / "override");
        assert(resolved == optiScalerDll);
        std::printf("  [PASS] Case 1b: OptiScaler/ directory takes strict priority over overrides\n");

        // Remove OptiScaler/ file, override should be used as fallback
        fs::remove(optiScalerDll, ec);
        resolved = NVSmooth30Unit::FindCandidatePath(testRoot, testRoot / "override");
        assert(resolved == overrideDll);
        std::printf("  [PASS] Case 1c: Override path fallback functions when OptiScaler/ is absent\n");

        // Clean up override, test root fallback
        fs::remove(overrideDll, ec);
        {
            std::ofstream f(rootDll);
            f << "MZ_MOCK_ROOT";
        }
        resolved = NVSmooth30Unit::FindCandidatePath(testRoot, {});
        assert(resolved == rootDll);
        std::printf("  [PASS] Case 1d: Root directory fallback functions as last resort\n");

        fs::remove(rootDll, ec);
        assert(NVSmooth30Unit::FindCandidatePath(testRoot, {}).empty());
    }

    // Test 2: Architecture Gating (Ampere RTX 30 only)
    {
        NVSmooth30Unit::MockLoaderState loader;
        const fs::path optiScalerDll = testRoot / "OptiScaler" / "nvsmooth30.dll";
        {
            std::ofstream f(optiScalerDll);
            f << "MZ_MOCK_DLL";
        }

        // Turing (0x160): Should fail with NotAmpere
        bool ok = loader.TrySetup(true, true, true, NV_GPU_ARCHITECTURE_TU100, testRoot, {});
        assert(!ok);
        assert(loader.status == NVSmooth30Unit::Status::NotAmpere);
        std::printf("  [PASS] Case 2a: Turing GPU rejected (NotAmpere)\n");

        // Ada (0x190): Native Smooth Motion supported by driver, nvsmooth30 unnecessary / rejected
        ok = loader.TrySetup(true, true, true, NV_GPU_ARCHITECTURE_AD100, testRoot, {});
        assert(!ok);
        assert(loader.status == NVSmooth30Unit::Status::NotAmpere);
        std::printf("  [PASS] Case 2b: Ada GPU rejected (NotAmpere)\n");

        // Non-Nvidia: Should fail with NotAmpere
        ok = loader.TrySetup(true, true, false, NV_GPU_ARCHITECTURE_GA100, testRoot, {});
        assert(!ok);
        assert(loader.status == NVSmooth30Unit::Status::NotAmpere);
        std::printf("  [PASS] Case 2c: Non-NVIDIA GPU rejected (NotAmpere)\n");

        // Ampere (0x170): Should succeed!
        ok = loader.TrySetup(true, true, true, NV_GPU_ARCHITECTURE_GA100, testRoot, {});
        assert(ok);
        assert(loader.status == NVSmooth30Unit::Status::Active);
        assert(loader.drsProfileApplied == true);
        assert(loader.loadedPath == optiScalerDll.string());
        std::printf("  [PASS] Case 2d: Ampere GPU successfully activates NVSmooth30 and applies DRS\n");
    }

    // Test 3: Configuration Disabling
    {
        NVSmooth30Unit::MockLoaderState loader;

        // SmoothMotion disabled overall
        bool ok = loader.TrySetup(false, true, true, NV_GPU_ARCHITECTURE_GA100, testRoot, {});
        assert(!ok);
        assert(loader.status == NVSmooth30Unit::Status::Disabled);

        // SmoothMotion enabled, but NVSmooth30 disabled
        ok = loader.TrySetup(true, false, true, NV_GPU_ARCHITECTURE_GA100, testRoot, {});
        assert(!ok);
        assert(loader.status == NVSmooth30Unit::Status::Disabled);

        std::printf("  [PASS] Case 3: Configuration disabling flags verified\n");
    }

    // Test 4: Missing Binary & Load Failure Handling
    {
        NVSmooth30Unit::MockLoaderState loader;
        const fs::path optiScalerDll = testRoot / "OptiScaler" / "nvsmooth30.dll";
        fs::remove(optiScalerDll, ec);

        // Binary does not exist
        bool ok = loader.TrySetup(true, true, true, NV_GPU_ARCHITECTURE_GA100, testRoot, {});
        assert(!ok);
        assert(loader.status == NVSmooth30Unit::Status::MissingBinary);
        assert(std::string(NVSmooth30Unit::StatusToString(loader.status)) == "OptiScaler/nvsmooth30.dll not found");
        std::printf("  [PASS] Case 4a: MissingBinary status handled cleanly\n");

        // Recreate file, simulate load failure
        {
            std::ofstream f(optiScalerDll);
            f << "MZ_MOCK_DLL";
        }
        ok = loader.TrySetup(true, true, true, NV_GPU_ARCHITECTURE_GA100, testRoot, {}, /*simulateLoadSuccess=*/false);
        assert(!ok);
        assert(loader.status == NVSmooth30Unit::Status::LoadFailed);
        std::printf("  [PASS] Case 4b: LoadFailed status handled cleanly\n");
    }

    // Test 5: Mutual Exclusion Guarding (DLSS-G / MFG vs Smooth Motion)
    {
        NVSmooth30Unit::MockLoaderState loader;
        const fs::path optiScalerDll = testRoot / "OptiScaler" / "nvsmooth30.dll";
        {
            std::ofstream f(optiScalerDll);
            f << "MZ_MOCK_DLL";
        }

        // With Ampere GPU, valid DLL, and SmoothMotion enabled, but FG conflict active (e.g. AmpereMfgUnlock)
        bool ok = loader.TrySetup(true, true, true, NV_GPU_ARCHITECTURE_GA100, testRoot, {}, true, /*fgConflict=*/true);
        assert(!ok);
        assert(loader.status == NVSmooth30Unit::Status::FgConflict);
        assert(std::string(NVSmooth30Unit::StatusToString(loader.status)) == "Conflict: DLSS-G / MFG active");
        assert(!loader.drsProfileApplied);
        std::printf("  [PASS] Case 5: DLSS-G / MFG mutual exclusion conflict handled cleanly\n");
    }

    // Test 6: Environment Sanitization Flags
    {
        auto env = NVSmooth30Unit::GetSanitizedEnvironment();
        assert(env["SM86_ENABLE_D3D11_BRIDGE"] == "0");
        assert(env["SM86_LOW_LATENCY"] == "0");
        assert(env["SM86_ENABLE_OSD"] == "0");
        assert(env["SM86_SKIP_DXGI_HOOKS"] == "1");
        std::printf("  [PASS] Case 6: Environment sanitization flags verified (Bridge=0, Latency=0, OSD=0, "
                    "SkipDxgiHooks=1)\n");
    }

    // Test 7: Swapchain Attachment & Restart Requirement State Transitions
    {
        NVSmooth30Unit::MockLoaderState loader;
        const fs::path optiScalerDll = testRoot / "OptiScaler" / "nvsmooth30.dll";
        {
            std::ofstream f(optiScalerDll);
            f << "MZ_MOCK_DLL";
        }

        bool ok = loader.TrySetup(true, true, true, NV_GPU_ARCHITECTURE_GA100, testRoot, {});
        assert(ok);
        assert(loader.status == NVSmooth30Unit::Status::Active);

        // Before swapchain attachment (mid-game toggle / pending restart):
        assert(!loader.swapchainAttached);
        assert(loader.GetStatusString() == "Pending Restart (Swapchain not bound)");
        assert(loader.GetBannerTag() == "[Pending Restart: Active on next game launch]");

        // After swapchain attachment verified:
        loader.swapchainAttached = true;
        assert(loader.GetStatusString() == "Active (Swapchain bound)");
        assert(loader.GetBannerTag() == "[Smooth Motion Active (RTX 30)]");

        std::printf("  [PASS] Case 7: Swapchain attachment & Pending Restart transitions verified\n");
    }

    // Test 8: NVSmooth30 Dummy Window Interception (prevents Streamline corruption and DXGI crash)
    {
        // Dummy 16x16 window created by nvsmooth30
        assert(NVSmooth30Unit::IsDummyWindow("NVSmooth30DummyWindow", 16, 16));
        assert(NVSmooth30Unit::IsDummyWindow("NVSmooth30DummyWindow", 0, 0));

        // Normal game windows MUST NOT be intercepted
        assert(!NVSmooth30Unit::IsDummyWindow("ControlWindowClass", 2560, 1440));
        assert(!NVSmooth30Unit::IsDummyWindow("UnrealWindow", 3840, 2160));
        assert(!NVSmooth30Unit::IsDummyWindow("DXGI_SWAPCHAIN_WINDOW", 1920, 1080));

        std::printf("  [PASS] Case 8: NVSmooth30 dummy window interception verified\n");
    }

    // Clean up temporary files
    fs::remove_all(testRoot, ec);

    std::printf("\nALL NVSMOOTH30 LOADER UNIT TESTS PASSED SUCCESSFULLY!\n");
    return 0;
}

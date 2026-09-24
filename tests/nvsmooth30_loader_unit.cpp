#include <cassert>
#include <cstdio>
#include <cstdint>
#include <string>
#include <filesystem>
#include <fstream>
#include <optional>

// Architecture identifiers matching NVAPI / OptiScaler
constexpr uint32_t NV_GPU_ARCHITECTURE_TU100 = 0x00000160;
constexpr uint32_t NV_GPU_ARCHITECTURE_GA100 = 0x00000170; // Ampere (RTX 30)
constexpr uint32_t NV_GPU_ARCHITECTURE_AD100 = 0x00000190; // Ada (RTX 40)
constexpr uint32_t NV_GPU_ARCHITECTURE_GB100 = 0x000001A0; // Blackwell (RTX 50)

// DRS setting identifiers
constexpr uint32_t NVDRS_SETTING_SMOOTH_MOTION_ENABLE = 0xB0D384C0;
constexpr uint32_t NVDRS_SETTING_SMOOTH_MOTION_APIS   = 0xB0CC0875;

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
        Active
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

    struct MockLoaderState
    {
        Status status = Status::Disabled;
        std::string loadedPath;
        bool drsProfileApplied = false;

        void Reset()
        {
            status = Status::Disabled;
            loadedPath.clear();
            drsProfileApplied = false;
        }

        bool TrySetup(bool configSmoothMotionEnabled,
                      bool configNVSmooth30Enabled,
                      bool isNvidia,
                      uint32_t archId,
                      const fs::path& basePath,
                      const fs::path& overridePath,
                      bool simulateLoadSuccess = true)
        {
            if (!configSmoothMotionEnabled || !configNVSmooth30Enabled)
            {
                status = Status::Disabled;
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
}

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
        const fs::path overrideDll   = testRoot / "override" / "nvsmooth30.dll";
        const fs::path rootDll       = testRoot / "nvsmooth30.dll";

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

    // Clean up temporary files
    fs::remove_all(testRoot, ec);

    std::printf("\nALL NVSMOOTH30 LOADER UNIT TESTS PASSED SUCCESSFULLY!\n");
    return 0;
}

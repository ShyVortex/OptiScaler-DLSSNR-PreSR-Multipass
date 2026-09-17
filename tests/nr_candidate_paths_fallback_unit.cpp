#include <cassert>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Mock NVSDK return codes
constexpr unsigned int NVSDK_NGX_Result_Success = 0x1;
constexpr unsigned int NVSDK_NGX_Result_Fail = 0xBAD00001; // FAIL_FeatureNotSupported
constexpr unsigned int NVSDK_NGX_Result_Fail_UnableToInit = 0xBAD0000B;

// Mock string utility
std::wstring ToLower(const std::wstring& str)
{
    std::wstring lower = str;
    for (auto& c : lower)
        c = (wchar_t) std::tolower(c);
    return lower;
}

// Mock CandidatePaths ordering logic from DlssNr_CompatibilityRuntimePaths.cpp
std::vector<fs::path> ResolveCandidatePaths(const std::optional<fs::path>& mainDllOverride,
                                           const fs::path& optiScalerDir,
                                           const fs::path& gameExeDir,
                                           const std::vector<fs::path>& featureInfoPaths)
{
    std::vector<fs::path> paths;
    // 1. Explicit user config override has highest priority
    if (mainDllOverride.has_value())
        paths.push_back(mainDllOverride.value());
    // 2. OptiScaler dedicated directory (where OptiScaler DLL and components reside)
    paths.push_back(optiScalerDir);
    // 3. Real game executable directory (where users place modded DLLs beside the exe)
    paths.push_back(gameExeDir);
    // 4. Game internal Streamline / feature directories as lowest-priority fallback
    for (const auto& path : featureInfoPaths)
        paths.push_back(path);

    std::vector<fs::path> candidates;
    std::set<std::wstring> visited;
    for (const auto& dir : paths)
    {
        if (dir.empty())
            continue;
        std::error_code ec;
        auto candidate = fs::absolute(dir / L"nvngx_dlssnr.dll", ec).lexically_normal();
        if (ec)
            continue;
        if (!visited.insert(ToLower(candidate.wstring())).second)
            continue;
        candidates.push_back(candidate);
    }
    return candidates;
}

// Mock CompatibilityRuntime Candidate Model
struct MockCompatibilityModule
{
    fs::path path;
    bool supportsGpuArch = false; // true if contains RTX 20/30/40 cubin kernels
    bool released = false;
    bool shutdown = false;
};

struct MockCompatibilityRuntime
{
    std::shared_ptr<MockCompatibilityModule> module;

    static std::shared_ptr<MockCompatibilityRuntime> TryOpen(const fs::path& path, bool supportsGpu)
    {
        auto rt = std::make_shared<MockCompatibilityRuntime>();
        rt->module = std::make_shared<MockCompatibilityModule>();
        rt->module->path = path;
        rt->module->supportsGpuArch = supportsGpu;
        return rt;
    }

    ~MockCompatibilityRuntime()
    {
        if (module)
            module->shutdown = true;
    }

    unsigned int Create(void** outHandle)
    {
        if (!module || !module->supportsGpuArch)
        {
            *outHandle = nullptr;
            return NVSDK_NGX_Result_Fail; // 0xBAD00001
        }
        *outHandle = reinterpret_cast<void*>(0x12345678);
        return NVSDK_NGX_Result_Success;
    }

    void Release(void* handle)
    {
        if (handle && module)
            module->released = true;
    }
};

// Simulation of DlssNr_Proxy Prepare logic with multi-candidate fallback
struct MockProxyContext
{
    std::shared_ptr<MockCompatibilityRuntime> compatibility;
    void* feature = nullptr;
    bool failed = false;
    std::vector<std::string> candidateAttempts;

    void Reset()
    {
        if (feature && compatibility)
            compatibility->Release(feature);
        feature = nullptr;
        compatibility.reset();
        failed = false;
        candidateAttempts.clear();
    }

    unsigned int PrepareWithCandidates(bool driverSuccess,
                                      const std::vector<std::pair<fs::path, bool>>& availableCandidates)
    {
        unsigned int created = driverSuccess ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail_UnableToInit;
        if (driverSuccess)
        {
            feature = reinterpret_cast<void*>(0xDEADBEEF);
            return NVSDK_NGX_Result_Success;
        }

        // Direct driver failed, enter multi-candidate compatibility loop
        for (const auto& [candidatePath, supportsGpu] : availableCandidates)
        {
            candidateAttempts.push_back(candidatePath.string());
            compatibility = MockCompatibilityRuntime::TryOpen(candidatePath, supportsGpu);
            if (!compatibility)
                continue;

            created = compatibility->Create(&feature);
            if (created == NVSDK_NGX_Result_Success && feature != nullptr)
            {
                // Successfully initialized with this candidate
                break;
            }

            // Failed on this candidate: clean release and reset before next attempt
            if (feature)
            {
                compatibility->Release(feature);
                feature = nullptr;
            }
            compatibility.reset();
        }

        if (created != NVSDK_NGX_Result_Success || feature == nullptr)
        {
            failed = true;
            return created;
        }

        return NVSDK_NGX_Result_Success;
    }
};

int main()
{
    std::cout << "Running DLSS-NR Candidate Priority & Multi-Runtime Fallback Unit Tests...\n";

    // Test 1: Priority ordering (User override > OptiScaler > Game Root > Streamline)
    {
        fs::path optiDir = "/games/NBA2K27/OptiScaler";
        fs::path exeDir = "/games/NBA2K27";
        std::vector<fs::path> featurePaths = {
            "/games/NBA2K27/data/Streamline",
            "/games/NBA2K27/data/streamline", // duplicate (different case)
            "/games/NBA2K27/Engine/Binaries"
        };

        // Without override
        auto candidates = ResolveCandidatePaths(std::nullopt, optiDir, exeDir, featurePaths);
        assert(candidates.size() == 4);
        assert(candidates[0].parent_path() == optiDir);
        assert(candidates[1].parent_path() == exeDir);
        assert(candidates[2].parent_path() == fs::path("/games/NBA2K27/data/Streamline"));
        assert(candidates[3].parent_path() == fs::path("/games/NBA2K27/Engine/Binaries"));

        // With explicit user override
        fs::path userOverride = "/custom/override_dir";
        auto candidatesWithOverride = ResolveCandidatePaths(userOverride, optiDir, exeDir, featurePaths);
        assert(candidatesWithOverride.size() == 5);
        assert(candidatesWithOverride[0].parent_path() == fs::path("/custom/override_dir"));
        assert(candidatesWithOverride[1].parent_path() == optiDir);
        assert(candidatesWithOverride[2].parent_path() == exeDir);

        std::cout << "  [PASS] Test 1: Candidate search priority order and case-insensitive deduplication\n";
    }

    // Test 2: Multi-candidate fallback when candidate 1 is Blackwell-only (NBA 2K27 bug scenario)
    {
        MockProxyContext ctx;

        // Candidate 1: Game Streamline DLL (official 310.8.0, supportsGpu = false on RTX 3070 Ti)
        // Candidate 2: Game root DLL (ShortFuse mod, supportsGpu = true on RTX 3070 Ti)
        std::vector<std::pair<fs::path, bool>> candidates = {
            { "/games/NBA2K27/data/Streamline/nvngx_dlssnr.dll", false },
            { "/games/NBA2K27/nvngx_dlssnr.dll", true }
        };

        unsigned int result = ctx.PrepareWithCandidates(false, candidates);
        assert(result == NVSDK_NGX_Result_Success);
        assert(ctx.feature != nullptr);
        assert(ctx.failed == false);
        assert(ctx.compatibility != nullptr);
        assert(ctx.compatibility->module->path == fs::path("/games/NBA2K27/nvngx_dlssnr.dll"));
        assert(ctx.candidateAttempts.size() == 2);
        assert(ctx.candidateAttempts[0] == "/games/NBA2K27/data/Streamline/nvngx_dlssnr.dll");
        assert(ctx.candidateAttempts[1] == "/games/NBA2K27/nvngx_dlssnr.dll");

        std::cout << "  [PASS] Test 2: Incompatible candidate 1 fails cleanly; candidate 2 succeeds seamlessly\n";
    }

    // Test 3: Direct driver success bypasses candidate iteration
    {
        MockProxyContext ctx;
        std::vector<std::pair<fs::path, bool>> candidates = {
            { "/games/NBA2K27/nvngx_dlssnr.dll", true }
        };

        unsigned int result = ctx.PrepareWithCandidates(true, candidates);
        assert(result == NVSDK_NGX_Result_Success);
        assert(ctx.feature != nullptr);
        assert(ctx.compatibility == nullptr);
        assert(ctx.candidateAttempts.empty());

        std::cout << "  [PASS] Test 3: Native driver success does not invoke compatibility candidates\n";
    }

    // Test 4: All candidates fail -> latches failure without leaks
    {
        MockProxyContext ctx;
        std::vector<std::pair<fs::path, bool>> candidates = {
            { "/games/NBA2K27/data/Streamline/nvngx_dlssnr.dll", false },
            { "/games/NBA2K27/invalid/nvngx_dlssnr.dll", false }
        };

        unsigned int result = ctx.PrepareWithCandidates(false, candidates);
        assert(result == NVSDK_NGX_Result_Fail);
        assert(ctx.feature == nullptr);
        assert(ctx.failed == true);
        assert(ctx.compatibility == nullptr);
        assert(ctx.candidateAttempts.size() == 2);

        std::cout << "  [PASS] Test 4: All candidates failing latches failure and cleans up runtime\n";
    }

    // Test 5: First candidate succeeds -> stops iteration immediately
    {
        MockProxyContext ctx;
        std::vector<std::pair<fs::path, bool>> candidates = {
            { "/games/NBA2K27/OptiScaler/nvngx_dlssnr.dll", true },
            { "/games/NBA2K27/nvngx_dlssnr.dll", true }
        };

        unsigned int result = ctx.PrepareWithCandidates(false, candidates);
        assert(result == NVSDK_NGX_Result_Success);
        assert(ctx.feature != nullptr);
        assert(ctx.compatibility != nullptr);
        assert(ctx.compatibility->module->path == fs::path("/games/NBA2K27/OptiScaler/nvngx_dlssnr.dll"));
        assert(ctx.candidateAttempts.size() == 1); // Second candidate not attempted

        std::cout << "  [PASS] Test 5: Successful first candidate stops further candidate attempts\n";
    }

    std::cout << "All DLSS-NR Candidate Priority & Multi-Runtime Fallback Unit Tests passed successfully!\n";
    return 0;
}

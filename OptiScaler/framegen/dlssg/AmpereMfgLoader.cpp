#include "pch.h"

#include "AmpereMfgLoader.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <misc/IdentifyGpu.h>
#include <proxies/Ntdll_Proxy.h>

#include <fstream>
#include <sstream>
#include <mutex>

#ifndef NV_GPU_ARCHITECTURE_GA100
#define NV_GPU_ARCHITECTURE_GA100 0x00000170
#endif

namespace AmpereMfgLoader
{
namespace
{
Status s_status;
bool s_setupAttempted = false;
std::recursive_mutex s_mutex;
} // namespace

Status LastStatus()
{
    std::lock_guard lock(s_mutex);
    return s_status;
}

std::string ResolveAutoKernelImage()
{
    const auto& gpu = IdentifyGpu::getPrimaryGpu();
    const bool onLinux = State::Instance().isRunningOnLinux || gpu.usesVkd3dProton;
    return ResolveAutoKernelImage(static_cast<uint32_t>(gpu.nvidiaArchInfo.architecture_id), gpu.name, onLinux);
}

std::string ResolveRouter()
{
    auto* cfg = Config::Instance();
    const auto& gpu = IdentifyGpu::getPrimaryGpu();
    const std::string configuredRouter = cfg->FGDLSSGAmpereMfgRouter.value_or("Auto");
    bool hasSm75Support = false;
    {
        std::lock_guard lock(s_mutex);
        hasSm75Support = s_status.HasSm75Support;
    }
    return ResolveRouter(static_cast<uint32_t>(gpu.nvidiaArchInfo.architecture_id), gpu.name, configuredRouter, hasSm75Support);
}

std::string GenerateIniContent(bool hasSm75Support, bool is3101Runtime)
{
    auto* cfg = Config::Instance();
    const auto& gpu = IdentifyGpu::getPrimaryGpu();
    const bool onLinux = State::Instance().isRunningOnLinux || gpu.usesVkd3dProton;
    const int configuredFrames = cfg->FGDLSSGAmpereMfgMaxFrames.value_or_default();
    const int maxCeiling = is3101Runtime ? 3 : 5;
    const int maxFrames = ResolveMaxGeneratedFrames(configuredFrames, onLinux, maxCeiling);

    if (onLinux && configuredFrames == 1)
    {
        LOG_INFO("AmpereMfgLoader: On Linux/Proton with 2X FG (configured max frames 1); SetFlipConfig is stubbed in NvApiHooks to enable clean native 2X FG");
    }

    std::string kernelImg = cfg->FGDLSSGAmpereMfgKernelImage.value_or("Auto");
    if (kernelImg != "PTX" && kernelImg != "Cubin")
        kernelImg = "Auto";

    if (kernelImg == "Auto")
    {
        std::string resolved = ResolveAutoKernelImage();
        if (resolved != "Auto")
        {
            LOG_INFO("AmpereMfgLoader: Auto kernel image resolved to {} for GPU: {}",
                     resolved, IdentifyGpu::getPrimaryGpu().name);
            kernelImg = resolved;
        }
    }

    int hwBilinear = cfg->FGDLSSGAmpereMfgHardwareBilinear.value_or_default() ? 1 : 0;
    const std::string configuredRouter = cfg->FGDLSSGAmpereMfgRouter.value_or("Auto");
    std::string router = ResolveRouter(static_cast<uint32_t>(gpu.nvidiaArchInfo.architecture_id), gpu.name, configuredRouter, hasSm75Support);
    const int optimized = cfg->FGDLSSGAmpereMfgOptimized.value_or(1);
    const std::string preset = cfg->FGDLSSGAmpereMfgPreset.value_or("Auto");
    const std::string spoofArch = cfg->FGDLSSGAmpereMfgSpoofArchToGame.value_or("Auto");
    int logLevel = cfg->FGDLSSGAmpereMfgLogLevel.value_or(1);
    if (logLevel < 0 || logLevel > 3)
        logLevel = 1;

    LOG_INFO("AmpereMfgLoader: 0.3.x INI: MaxFrames: {}, Optimized: {}, Preset: {}, Router: {}, SpoofArch: {}, LogLevel: {} (hasSm75Support: {}, is3101Runtime: {}) for GPU: {}",
             maxFrames, optimized, preset, router, spoofArch, logLevel, hasSm75Support, is3101Runtime, IdentifyGpu::getPrimaryGpu().name);

    return FormatIniContent030(maxFrames, optimized, preset, kernelImg, hwBilinear, router, logLevel, spoofArch);
}

std::string GenerateIniContent(bool hasSm75Support)
{
    return GenerateIniContent(hasSm75Support, false);
}

std::string GenerateIniContent()
{
    std::lock_guard lock(s_mutex);
    return GenerateIniContent(s_status.HasSm75Support, s_status.Is3101Runtime);
}

void TrySetup()
{
    std::lock_guard lock(s_mutex);
    if (s_setupAttempted)
        return;
    s_setupAttempted = true;

    auto* cfg = Config::Instance();
    if (!cfg->FGDLSSGAmpereMfgUnlock.value_or_default())
        return;

    const auto& gpu = IdentifyGpu::getPrimaryGpu();
    const bool onLinux = State::Instance().isRunningOnLinux || gpu.usesVkd3dProton;
    const int configuredFrames = cfg->FGDLSSGAmpereMfgMaxFrames.value_or_default();

    const std::string fallbackSetting = cfg->FGDLSSGAmpereMfgLinuxFsrFallback.value_or("auto");
    const std::string fallbackType = ResolveFallbackFgType(cfg->FGDLSSGAmpereMfgLinuxFallbackType.value_or("fsrfg"));
    if (ShouldFallbackToFsrFg(configuredFrames, onLinux, true, fallbackSetting))
    {
        s_status.Enabled = true;
        s_status.FsrFallbackActive = true;
        s_status.ErrorMessage.clear();
        LOG_INFO("AmpereMfgLoader: On Linux with FG fallback active (mode: {}), falling back to internal {} instead of sideloading dlssg_sm86",
                 fallbackSetting, (fallbackType == "xefg" ? "XeFG" : "FSR FG"));
        return;
    }

    s_status.Enabled = true;

    // Mutual exclusion: fail if Ada MFG unlock is also enabled
    if (cfg->FGDLSSGAdaMfgUnlock.value_or_default())
    {
        s_status.ErrorMessage = "Cannot enable SM86/SM75 MFG while Ada (RTX 40) MFG unlock is enabled.";
        LOG_ERROR("AmpereMfgLoader: {}", s_status.ErrorMessage);
        return;
    }

    // GPU guard: verify Nvidia Turing or Ampere architecture
    if (gpu.vendorId != VendorId::Nvidia)
    {
        s_status.ErrorMessage = "SM86/SM75 MFG requires an NVIDIA GPU.";
        LOG_ERROR("AmpereMfgLoader: {}", s_status.ErrorMessage);
        return;
    }

    const uint32_t archId = static_cast<uint32_t>(gpu.nvidiaArchInfo.architecture_id);
    const bool isAmpere = IsAmpereArch(archId) ||
                          (gpu.name.find("RTX 30") != std::string::npos ||
                           gpu.name.find("Ampere") != std::string::npos ||
                           gpu.name.find("GA10") != std::string::npos ||
                           gpu.name.find("RTX A") != std::string::npos);

    const bool isTuring = IsTuringArch(archId) ||
                          (gpu.name.find("RTX 20") != std::string::npos ||
                           gpu.name.find("GTX 16") != std::string::npos ||
                           gpu.name.find("TITAN RTX") != std::string::npos ||
                           gpu.name.find("Turing") != std::string::npos ||
                           gpu.name.find("TU10") != std::string::npos ||
                           gpu.name.find("TU11") != std::string::npos);

    if (!isAmpere && !isTuring)
    {
        s_status.ErrorMessage = std::format(
            "SM86/SM75 MFG requires an RTX 20 series (Turing) or RTX 30 series (Ampere) GPU. Detected arch 0x{:x} ({}).",
            archId, gpu.name);
        LOG_ERROR("AmpereMfgLoader: {}", s_status.ErrorMessage);
        return;
    }

    // Locate dlssg_sm86.dll
    auto basePath = Util::DllPath().parent_path();
    std::filesystem::path dllPath;
    std::error_code fileError;

    auto probePath = [&](const std::filesystem::path& candidate) -> bool {
        return !candidate.empty() && std::filesystem::exists(candidate, fileError);
    };

    auto mainOverride = cfg->MainDllPath.has_value() ? std::filesystem::path(cfg->MainDllPath.value()) : std::filesystem::path();

    // Standard candidates for root runtime (310.9)
    std::filesystem::path rootCandidates[] = {
        mainOverride.empty() ? std::filesystem::path() : mainOverride / L"dlssg_sm86" / L"dlssg_sm86.dll",
        basePath / L"OptiScaler" / L"dlssg_sm86" / L"dlssg_sm86.dll",
        basePath / L"dlssg_sm86" / L"dlssg_sm86.dll",
        basePath / L"dlssg_sm86.dll"
    };

    // Candidates for fallback 310.1 runtime (specifically providing SM75 kernels on legacy 0.3.0 builds)
    std::filesystem::path legacy3101Candidates[] = {
        mainOverride.empty() ? std::filesystem::path() : mainOverride / L"dlssg_sm86" / L"310.1" / L"dlssg_sm86.dll",
        mainOverride.empty() ? std::filesystem::path() : mainOverride / L"dlssg_sm86" / L"310.1" / L"version.dll",
        basePath / L"OptiScaler" / L"dlssg_sm86" / L"310.1" / L"dlssg_sm86.dll",
        basePath / L"OptiScaler" / L"dlssg_sm86" / L"310.1" / L"version.dll",
        basePath / L"dlssg_sm86" / L"310.1" / L"dlssg_sm86.dll",
        basePath / L"dlssg_sm86" / L"310.1" / L"version.dll",
        basePath / L"310.1" / L"dlssg_sm86.dll",
        basePath / L"310.1" / L"version.dll"
    };

    // Probe root runtime candidate first
    std::filesystem::path rootDll;
    for (const auto& candidate : rootCandidates)
    {
        if (probePath(candidate))
        {
            rootDll = candidate;
            break;
        }
    }

    if (isTuring)
    {
        // If root binary is found and already contains native SM75 support (0.3.1+ unified build),
        // use it directly so Turing benefits from the latest 310.9 runtime, 6X MFG, and optimizations.
        if (!rootDll.empty() && HasSm75KernelFamily(rootDll))
        {
            dllPath = rootDll;
            LOG_INFO("AmpereMfgLoader: Turing GPU detected; root runtime at {} contains native SM75 support (0.3.1+)",
                     wstring_to_string(dllPath.wstring()));
        }
        else
        {
            // Root binary lacks SM75 support (e.g. legacy 0.3.0 310.9 build); fallback to 310.1 runtime
            for (const auto& candidate : legacy3101Candidates)
            {
                if (probePath(candidate))
                {
                    dllPath = candidate;
                    LOG_INFO("AmpereMfgLoader: Turing GPU detected; root runtime lacks SM75 kernels, falling back to 310.1 runtime at {}",
                             wstring_to_string(dllPath.wstring()));
                    break;
                }
            }

            // If 310.1 was not found either, fall back to rootDll if available
            if (dllPath.empty() && !rootDll.empty())
            {
                dllPath = rootDll;
            }
        }
    }
    else
    {
        // Ampere (or other supported Nvidia arch): use root runtime, or fallback to 310.1 if root not present
        if (!rootDll.empty())
        {
            dllPath = rootDll;
        }
        else
        {
            for (const auto& candidate : legacy3101Candidates)
            {
                if (probePath(candidate))
                {
                    dllPath = candidate;
                    break;
                }
            }
        }
    }

    if (dllPath.empty())
    {
        s_status.DllFound = false;
        s_status.ErrorMessage = "dlssg_sm86.dll not found in OptiScaler/dlssg_sm86/ or dlssg_sm86/ subfolders.";
        LOG_ERROR("AmpereMfgLoader: {}", s_status.ErrorMessage);
        return;
    }

    s_status.DllFound = true;
    s_status.LoadedDllPath = dllPath.wstring();
    s_status.HasSm75Support = HasSm75KernelFamily(dllPath);
    s_status.Is3101Runtime = Is3101Runtime(dllPath);

    LOG_INFO("AmpereMfgLoader: Located binary at {}, HasSm75Support: {}, Is3101Runtime: {}",
             wstring_to_string(dllPath.wstring()), s_status.HasSm75Support, s_status.Is3101Runtime);

    // Generate and write companion dlssg_sm86.ini beside the DLL
    auto iniPath = dllPath.parent_path() / L"dlssg_sm86.ini";
    try
    {
        std::filesystem::create_directories(iniPath.parent_path());
        std::ofstream iniFile(iniPath, std::ios::out | std::ios::trunc);
        if (!iniFile.is_open())
        {
            s_status.IniWritten = false;
            s_status.ErrorMessage = "Failed to open dlssg_sm86.ini for writing.";
            LOG_ERROR("AmpereMfgLoader: Failed to open {} for writing", wstring_to_string(iniPath.wstring()));
            return;
        }
        iniFile << GenerateIniContent(s_status.HasSm75Support, s_status.Is3101Runtime);
        iniFile.close();
        if (!iniFile)
            throw std::runtime_error("Could not finish writing dlssg_sm86.ini");
        s_status.IniWritten = true;
    }
    catch (const std::exception& ex)
    {
        s_status.IniWritten = false;
        s_status.ErrorMessage = std::string("Error writing dlssg_sm86.ini: ") + ex.what();
        LOG_ERROR("AmpereMfgLoader: Exception writing INI: {}", ex.what());
        return;
    }

    // Load dlssg_sm86.dll
    NtdllProxy::Init();
    LOG_INFO("AmpereMfgLoader: Loading {}", wstring_to_string(dllPath.wstring()));
    HMODULE hMod = NtdllProxy::LoadLibraryExW_Ldr(dllPath.c_str(), NULL, 0);
    if (!hMod)
        hMod = LoadLibraryW(dllPath.c_str());

    if (!hMod)
    {
        DWORD err = GetLastError();
        s_status.DllLoaded = false;
        s_status.ErrorMessage = "Failed to load dlssg_sm86.dll (error code " + std::to_string(err) + ").";
        LOG_ERROR("AmpereMfgLoader: Failed to load dlssg_sm86.dll, error: {}", err);
        return;
    }

    s_status.DllLoaded = true;
    s_status.ErrorMessage.clear();
    LOG_INFO("AmpereMfgLoader: SM86/SM75 MFG loaded successfully from {}", wstring_to_string(dllPath.wstring()));
}

} // namespace AmpereMfgLoader

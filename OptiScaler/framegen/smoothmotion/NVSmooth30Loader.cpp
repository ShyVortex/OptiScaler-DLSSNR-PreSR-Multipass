#include "pch.h"

#include "NVSmooth30Loader.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <misc/IdentifyGpu.h>
#include <proxies/Ntdll_Proxy.h>
#include <nvapi/NvApiHooks.h>

#include <mutex>

namespace NVSmooth30Loader
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

void TrySetup()
{
    std::lock_guard lock(s_mutex);
    if (s_status.DllLoaded)
        return;

    auto* cfg = Config::Instance();

    // Check if Smooth Motion is requested and NVSmooth30 unlocker is enabled
    const bool smoothMotion = cfg->FGDLSSGSmoothMotion.value_or_default();
    const bool optedIn = cfg->SmoothMotionNVSmooth30.value_or_default();
    if (!smoothMotion || !optedIn)
    {
        s_status.Enabled = false;
        return;
    }

    s_setupAttempted = true;
    s_status.Enabled = true;

    // Platform guard: NVSmooth30 patches Windows NvPresent64.dll and does not run on Linux/Proton
    const auto& gpu = IdentifyGpu::getPrimaryGpu();
    const bool onLinux = State::Instance().isRunningOnLinux || gpu.usesVkd3dProton;
    if (onLinux)
    {
        s_status.ErrorMessage = "NVSmooth30 requires Windows (NvPresent64.dll path not supported on Linux/Proton).";
        LOG_INFO("NVSmooth30Loader: {}", s_status.ErrorMessage);
        return;
    }

    // GPU vendor guard: NVIDIA required
    if (gpu.vendorId != VendorId::Nvidia)
    {
        s_status.ErrorMessage = "NVSmooth30 requires an NVIDIA GPU.";
        LOG_ERROR("NVSmooth30Loader: {}", s_status.ErrorMessage);
        return;
    }

    // Architecture guard: NVIDIA Ampere (RTX 30 series / SM86) required
    const uint32_t archId = static_cast<uint32_t>(gpu.nvidiaArchInfo.architecture_id);
    const bool isAmpere =
        IsAmpereArch(archId) ||
        (gpu.name.find("RTX 30") != std::string::npos || gpu.name.find("Ampere") != std::string::npos ||
         gpu.name.find("GA10") != std::string::npos || gpu.name.find("RTX A") != std::string::npos);

    if (!isAmpere)
    {
        s_status.ErrorMessage = std::format(
            "NVSmooth30 requires an RTX 30 series (Ampere) GPU. Detected arch 0x{:x} ({}).", archId, gpu.name);
        LOG_WARN("NVSmooth30Loader: {}", s_status.ErrorMessage);
        return;
    }

    // Resolve DLL path (strictly inside OptiScaler directory)
    auto basePath = Util::DllPath().parent_path();
    auto mainOverride =
        cfg->MainDllPath.has_value() ? std::filesystem::path(cfg->MainDllPath.value()) : std::filesystem::path();
    auto dllPath = ResolveCandidatePath(basePath, mainOverride);

    if (dllPath.empty())
    {
        s_status.DllFound = false;
        s_status.ErrorMessage = "nvsmooth30.dll not found in OptiScaler directory (OptiScaler/nvsmooth30.dll).";
        LOG_WARN("NVSmooth30Loader: {}", s_status.ErrorMessage);
        return;
    }

    s_status.DllFound = true;
    s_status.LoadedDllPath = dllPath.wstring();
    LOG_INFO("NVSmooth30Loader: Located nvsmooth30.dll at {}", wstring_to_string(dllPath.wstring()));

    // Synchronize NVIDIA Smooth Motion DRS setting in driver profile
    s_status.SmoothMotionActive = NvApiHooks::ApplySmoothMotionDrs(true);
    if (s_status.SmoothMotionActive)
    {
        LOG_INFO("NVSmooth30Loader: NVIDIA Smooth Motion active in driver profile (via NVAPI DRS)");
    }
    else
    {
        LOG_WARN("NVSmooth30Loader: Failed to apply Smooth Motion DRS setting; driver profile may require manual "
                 "inspection");
    }

    // Load nvsmooth30.dll
    NtdllProxy::Init();
    LOG_INFO("NVSmooth30Loader: Loading {}", wstring_to_string(dllPath.wstring()));
    HMODULE hMod = NtdllProxy::LoadLibraryExW_Ldr(dllPath.c_str(), NULL, 0);
    if (!hMod)
        hMod = LoadLibraryW(dllPath.c_str());

    if (!hMod)
    {
        DWORD err = GetLastError();
        s_status.DllLoaded = false;
        s_status.ErrorMessage = "Failed to load nvsmooth30.dll (error code " + std::to_string(err) + ").";
        LOG_ERROR("NVSmooth30Loader: Failed to load nvsmooth30.dll, error: {}", err);
        return;
    }

    s_status.DllLoaded = true;
    s_status.ErrorMessage.clear();
    LOG_INFO("NVSmooth30Loader: NVSmooth30 loaded successfully from {}", wstring_to_string(dllPath.wstring()));
}

} // namespace NVSmooth30Loader

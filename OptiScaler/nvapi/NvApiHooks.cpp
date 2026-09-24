#include "pch.h"
#include "NvApiHooks.h"
#include <NvApiDriverSettings.h>
#include <framegen/dlssg/AmpereMfgLoader.h>

#include "State.h"
#include <Config.h>

#include <proxies/KernelBase_Proxy.h>

#include <detours/detours.h>
#include <misc/IdentifyGpu.h>
#include <low_latency/input/input_reflex.h>

// #define LOG_ALL_DRS_GET_CALLS

#ifdef LOG_ALL_DRS_GET_CALLS
#include <magic_enum.hpp>
#endif

#include <Util.h>
#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

#ifndef NV_GPU_ARCHITECTURE_GA100
#define NV_GPU_ARCHITECTURE_GA100 0x00000170
#endif
#ifndef NV_GPU_ARCHITECTURE_AD100
#define NV_GPU_ARCHITECTURE_AD100 0x00000190
#endif
#ifndef NV_GPU_ARCHITECTURE_GB200
#define NV_GPU_ARCHITECTURE_GB200 0x000001b0
#endif

NvAPI_Status __stdcall NvApiHooks::hkNvAPI_GPU_GetArchInfo(NvPhysicalGpuHandle hPhysicalGpu,
                                                           NV_GPU_ARCH_INFO* pGpuArchInfo)
{
    if (!o_NvAPI_GPU_GetArchInfo)
    {
        LOG_DEBUG("nullptr");
        return NVAPI_ERROR;
    }

    const auto status = o_NvAPI_GPU_GetArchInfo(hPhysicalGpu, pGpuArchInfo);

    if (status == NVAPI_OK && pGpuArchInfo)
    {
        if (pGpuArchInfo->architecture_id <= NV_GPU_ARCHITECTURE_GP100)
        {
            // Check if values were volatile, override them if so
            // if (!Config::Instance()->StreamlineSpoofing.value_for_config().has_value())
            //    Config::Instance()->StreamlineSpoofing.set_volatile_value(true);

            if (!Config::Instance()->DisableFlipMetering.value_for_config().has_value())
                Config::Instance()->DisableFlipMetering.set_volatile_value(true);
        }

        LOG_DEBUG("Original arch: {0:X} impl: {1:X} rev: {2:X}!", static_cast<uint32_t>(pGpuArchInfo->architecture),
                  static_cast<uint32_t>(pGpuArchInfo->implementation), static_cast<uint32_t>(pGpuArchInfo->revision));

        // When external mods (e.g. dlssg_sm86) or OptiScaler Ampere MFG unlock are used
        // to enable Streamline DLSS-G, FG callers (like sl.dlss_g, external mods, and _nvngx snippet loader)
        // must see Ada (0x190) architecture so NGX snippet validation and Streamline accept the GPU.
        // Conversely, non-FG callers (like nvngx_dlss for Super Resolution or nvngx_dlssd for Ray Reconstruction)
        // must NOT see the spoofed architecture. Otherwise, DLSS SR/RR loads Blackwell/Ada-only cubin shaders
        // (e.g. DLTSS NW E5M3_SKIP FP8 kernels) that execute illegal instructions on Ampere/Turing hardware,
        // causing DXGI_ERROR_DEVICE_HUNG (0x887A0006) crashes on startup (e.g. in The Last of Us Part II).
        const auto primaryGpu = IdentifyGpu::getPrimaryGpu();
        const auto realArch = primaryGpu.nvidiaArchInfo.architecture_id != 0 ? primaryGpu.nvidiaArchInfo.architecture_id
                                                                             : primaryGpu.nvidiaArchInfo.architecture;

        if (primaryGpu.vendorId == VendorId::Nvidia && realArch != 0 && realArch < NV_GPU_ARCHITECTURE_AD100)
        {
            const void* retAddr = _ReturnAddress();
            std::string caller = Util::WhoIsTheCaller(const_cast<void*>(retAddr));
            std::string callerLower = caller;
            std::transform(callerLower.begin(), callerLower.end(), callerLower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            const bool isExplicitNonFgCaller =
                (callerLower.find("nvngx_dlss.") != std::string::npos ||
                 callerLower.find("nvngx_dlssd") != std::string::npos || callerLower.ends_with(".exe"));

            const bool isFgCaller = !isExplicitNonFgCaller &&
                                    (callerLower.find("sl.common") != std::string::npos ||
                                     callerLower.find("sl.dlss_g") != std::string::npos ||
                                     callerLower.find("sl.interposer") != std::string::npos ||
                                     callerLower.find("dlssg") != std::string::npos ||
                                     callerLower.find("version") != std::string::npos || callerLower == "_nvngx.dll" ||
                                     callerLower == "nvngx.dll" || callerLower.find("_nvngx") != std::string::npos);

            const bool mfgUnlock = Config::Instance()->FGDLSSGAmpereMfgUnlock.value_or_default();

            if (mfgUnlock && isFgCaller)
            {
                if (pGpuArchInfo->architecture < NV_GPU_ARCHITECTURE_AD100)
                {
                    pGpuArchInfo->architecture =
                        static_cast<decltype(pGpuArchInfo->architecture)>(NV_GPU_ARCHITECTURE_AD100);
                    pGpuArchInfo->architecture_id =
                        static_cast<decltype(pGpuArchInfo->architecture_id)>(NV_GPU_ARCHITECTURE_AD100);
                    pGpuArchInfo->implementation = static_cast<decltype(pGpuArchInfo->implementation)>(0x102);
                    pGpuArchInfo->implementation_id = static_cast<decltype(pGpuArchInfo->implementation_id)>(0x102);

                    LOG_INFO("Spoofed GPU arch to Ada (0x190) for FG caller '{}' (real: {:X})", caller,
                             static_cast<uint32_t>(realArch));
                }
            }
            else if (!isFgCaller && pGpuArchInfo->architecture >= NV_GPU_ARCHITECTURE_AD100)
            {
                const auto spoofedArch = pGpuArchInfo->architecture;
                pGpuArchInfo->architecture = static_cast<decltype(pGpuArchInfo->architecture)>(realArch);
                pGpuArchInfo->architecture_id = static_cast<decltype(pGpuArchInfo->architecture_id)>(realArch);
                pGpuArchInfo->implementation = primaryGpu.nvidiaArchInfo.implementation;
                pGpuArchInfo->implementation_id = primaryGpu.nvidiaArchInfo.implementation_id;
                pGpuArchInfo->revision = primaryGpu.nvidiaArchInfo.revision;
                pGpuArchInfo->revision_id = primaryGpu.nvidiaArchInfo.revision_id;

                LOG_INFO("Restored physical GPU arch for non-FG caller '{}': arch: {:X} impl: {:X} rev: {:X} (was "
                         "spoofed: {:X})",
                         caller, static_cast<uint32_t>(pGpuArchInfo->architecture),
                         static_cast<uint32_t>(pGpuArchInfo->implementation),
                         static_cast<uint32_t>(pGpuArchInfo->revision), static_cast<uint32_t>(spoofedArch));
            }
        }

        // for DLSS on 16xx cards
        // Can't spoof ada for DLSSG here as that breaks DLSS/DLSSD
        if (pGpuArchInfo->architecture == NV_GPU_ARCHITECTURE_TU100 &&
            pGpuArchInfo->implementation > NV_GPU_ARCH_IMPLEMENTATION_TU106)
        {
            pGpuArchInfo->implementation = NV_GPU_ARCH_IMPLEMENTATION_TU106;
            LOG_INFO("Spoofed arch: {0:X} impl: {1:X} rev: {2:X}!", static_cast<uint32_t>(pGpuArchInfo->architecture),
                     static_cast<uint32_t>(pGpuArchInfo->implementation),
                     static_cast<uint32_t>(pGpuArchInfo->revision));
        }
    }

    return status;
}

NvAPI_Status __stdcall NvApiHooks::hkNvAPI_DRS_GetSetting(NvDRSSessionHandle hSession, NvDRSProfileHandle hProfile,
                                                          NvU32 settingId, NVDRS_SETTING* pSetting)
{
    // On Linux / Proton with Ampere SM86 MFG, Streamline queries DRS settings to determine
    // the static and dynamic multi-frame count limits:
    //   0x104D6667: Override DLSSG multi-frame count
    //   0x10562D0F: Override maximum DLSSG dynamic multi frame count
    // When dlssg_sm86.ini is elevated to 2 to bypass unimplemented SetFlipConfig in DXVK-NVAPI,
    // answering these DRS queries with the user's configured AmpereMfgMaxFrames (e.g. 1 for 2X FG)
    // forces Streamline to evaluate exactly 1 interpolated frame per present without toggling or flickering.
    const bool onLinux = State::Instance().isRunningOnLinux || IdentifyGpu::getPrimaryGpu().usesVkd3dProton;
    const bool mfgUnlock = Config::Instance()->FGDLSSGAmpereMfgUnlock.value_or_default();
    const int configuredFrames = Config::Instance()->FGDLSSGAmpereMfgMaxFrames.value_or_default();
    const int explicitOverride = Config::Instance()->FGDLSSGOverrideInterpolationCount.value_or(0);
    const bool dynamicMfg = Config::Instance()->FGDLSSGOverrideForceDMFG.value_or_default() ||
                            Config::Instance()->FGDLSSGForceDMFG.value_or_default();
    uint32_t targetFrames = 0;
    if (AmpereMfgLoader::TryResolveDrsMultiFrameSetting(settingId, configuredFrames, onLinux, mfgUnlock, targetFrames,
                                                        5, explicitOverride, dynamicMfg))
    {
        if (pSetting)
        {
            pSetting->settingId = settingId;
            pSetting->settingType = NVDRS_DWORD_TYPE;
            pSetting->settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
            pSetting->isCurrentPredefined = 0;
            pSetting->isPredefinedValid = 0;
            pSetting->u32CurrentValue = targetFrames;
            pSetting->u32PredefinedValue = targetFrames;
        }
        LOG_INFO("hkNvAPI_DRS_GetSetting: overriding setting 0x{:X} to {} for Ampere MFG on Linux", settingId,
                 targetFrames);
        return NVAPI_OK;
    }

    if (!o_NvAPI_DRS_GetSetting)
        return NVAPI_ERROR;

    auto result = o_NvAPI_DRS_GetSetting(hSession, hProfile, settingId, pSetting);
    if (pSetting && result == NVAPI_OK)
    {
        constexpr NvU32 streamlineOverrideId = 0x10E41E06;
        if (settingId == streamlineOverrideId && State::Instance().gameName == "KCD2" &&
            State::Instance().activeFgOutput == FGOutput::DLSSG && !State::Instance().externalFrameGeneration)
        {
            // Keep the tested local Streamline stack. This changes the query
            // result for this process only, not the saved NVIDIA driver profile.
            pSetting->u32CurrentValue = 0;
            LOG_INFO("KCD2: use installed Streamline instead of OTA override");
        }
#ifdef LOG_ALL_DRS_GET_CALLS
        LOG_TRACE("settingId: {:X}, settingLocation: {}, isCurrentPredefined: {}", settingId,
                  magic_enum::enum_name(pSetting->settingLocation), pSetting->isCurrentPredefined,
                  pSetting->isPredefinedValid);

        switch (pSetting->settingType)
        {
        case NVDRS_DWORD_TYPE:
            LOG_TRACE("    u32CurrentValue: {}, u32PredefinedValue: {}", pSetting->u32CurrentValue,
                      pSetting->u32PredefinedValue);
            break;
        case NVDRS_BINARY_TYPE:
            LOG_TRACE("    binary data");
            break;
        case NVDRS_STRING_TYPE:
            LOG_TRACE("    NVDRS_STRING_TYPE");
            break;
        case NVDRS_WSTRING_TYPE:
        {
            std::wstring wstrCurrentValue(reinterpret_cast<const wchar_t*>(pSetting->wszCurrentValue));
            std::wstring wstrPredefinedValue(reinterpret_cast<const wchar_t*>(pSetting->wszPredefinedValue));

            LOG_TRACE(L"    wszCurrentValue: {}, wszPredefinedValue: {}", wstrCurrentValue, wstrPredefinedValue);
            break;
        }
        }
#endif

        // TODO: maybe check those values and inform if they are being overridden externally

        // const auto dmfgFpsTarget = Config::Instance()->FGDLSSGFramerateTargetDMFG.value_or_default();
        // if (settingId == NGX_DLSSG_MODE_ID && dmfgFpsTarget != 0)
        //{
        //     pSetting->settingId = settingId;
        //     // constexpr auto name = L"NGX_DLSSG_MODE_ID";
        //     // memcpy_s(pSetting->settingName, sizeof(pSetting->settingName), name, sizeof(*name) * wcslen(name));
        //     pSetting->settingType = NVDRS_DWORD_TYPE;
        //     pSetting->isCurrentPredefined = 0;
        //     pSetting->u32CurrentValue = NGX_DLSSG_MODE_DEFAULT;

        //    LOG_DEBUG("Set NGX_DLSSG_MODE_ID to {}", pSetting->u32CurrentValue);
        //}

        // if (settingId == NGX_DLSSG_DYNAMIC_TARGET_FRAME_RATE_ID && dmfgFpsTarget != 0)
        //{
        //     pSetting->settingId = settingId;
        //     // constexpr auto name = L"NGX_DLSSG_DYNAMIC_TARGET_FRAME_RATE_ID";
        //     // memcpy_s(pSetting->settingName, sizeof(pSetting->settingName), name, sizeof(*name) * wcslen(name));
        //     pSetting->settingType = NVDRS_DWORD_TYPE;
        //     pSetting->isCurrentPredefined = 0;
        //     pSetting->u32CurrentValue = dmfgFpsTarget;

        //    LOG_DEBUG("Set NGX_DLSSG_DYNAMIC_TARGET_FRAME_RATE_ID to {}", pSetting->u32CurrentValue);
        //}

        // if (settingId == NGX_DLSSG_DYNAMIC_MULTI_FRAME_COUNT_MAX_ID && dmfgFpsTarget != 0)
        //{
        //     pSetting->settingId = settingId;
        //     // constexpr auto name = L"NGX_DLSSG_DYNAMIC_MULTI_FRAME_COUNT_MAX_ID";
        //     // memcpy_s(pSetting->settingName, sizeof(pSetting->settingName), name, sizeof(*name) * wcslen(name));
        //     pSetting->settingType = NVDRS_DWORD_TYPE;
        //     pSetting->isCurrentPredefined = 0;
        //     pSetting->u32CurrentValue = NGX_DLSSG_DYNAMIC_MULTI_FRAME_COUNT_MAX_DEFAULT;

        //    LOG_DEBUG("Set NGX_DLSSG_DYNAMIC_MULTI_FRAME_COUNT_MAX_ID to {}", pSetting->u32CurrentValue);
        //}

        // Making sure DLSSG is not set to force off
        if (settingId == NGX_DLSSG_MODE_ID)
        {
            if (State::Instance().activeFgOutput == FGOutput::DLSSG)
            {
                pSetting->u32CurrentValue = NGX_DLSSG_MODE_DISABLED;
            }
        }

        if (settingId == NGX_DLSS_SR_OVERRIDE_RENDER_PRESET_SELECTION_ID)
        {
            State::Instance().dlssRenderPresetExternal = pSetting->u32CurrentValue;

            State::Instance().dlssPresetsOverriddenExternally =
                pSetting->u32CurrentValue != NGX_DLSS_SR_OVERRIDE_RENDER_PRESET_SELECTION_OFF;

            // Report no override, we will handle presets from now on
            pSetting->u32CurrentValue = NGX_DLSS_SR_OVERRIDE_RENDER_PRESET_SELECTION_OFF;

            LOG_DEBUG("DLSS External override: {}", State::Instance().dlssRenderPresetExternal);
        }

        if (settingId == NGX_DLSS_RR_OVERRIDE_RENDER_PRESET_SELECTION_ID)
        {
            State::Instance().dlssdRenderPresetExternal = pSetting->u32CurrentValue;

            State::Instance().dlssdPresetsOverriddenExternally =
                pSetting->u32CurrentValue != NGX_DLSS_RR_OVERRIDE_RENDER_PRESET_SELECTION_OFF;

            // Report no override, we will handle presets from now on
            pSetting->u32CurrentValue = NGX_DLSS_RR_OVERRIDE_RENDER_PRESET_SELECTION_OFF;

            LOG_DEBUG("DLSSD External override: {}", State::Instance().dlssdRenderPresetExternal);
        }

        if (settingId == NGX_DLSS_RR_OVERRIDE_SCALING_RATIO_ID || settingId == NGX_DLSS_SR_OVERRIDE_SCALING_RATIO_ID)
        {
            if (Config::Instance()->UpscaleRatioOverrideEnabled.value_or_default())
            {
                auto ratio = Config::Instance()->UpscaleRatioOverrideValue.value_or_default();
                auto ratioPercentage = (uint32_t) std::round(100.f / ratio);

                // Uses the clamp from SR for RR but it should be fine
                ratioPercentage = std::clamp(ratioPercentage, (uint32_t) NGX_DLSS_SR_OVERRIDE_SCALING_RATIO_MIN,
                                             (uint32_t) NGX_DLSS_SR_OVERRIDE_SCALING_RATIO_MAX);

                pSetting->u32CurrentValue = ratioPercentage;
            }
        }
        if (settingId == NVDRS_SETTING_SMOOTH_MOTION_ENABLE)
        {
            const bool smoothMotion = Config::Instance()->FGDLSSGSmoothMotion.value_or(false);
            pSetting->settingType = NVDRS_DWORD_TYPE;
            pSetting->u32CurrentValue = smoothMotion ? 1 : 0;
            LOG_DEBUG("NvAPI_DRS_GetSetting: Intercepted Smooth Motion Enable -> {}", pSetting->u32CurrentValue);
        }

        if (settingId == NVDRS_SETTING_SMOOTH_MOTION_APIS)
        {
            const bool smoothMotion = Config::Instance()->FGDLSSGSmoothMotion.value_or(false);
            if (smoothMotion)
            {
                pSetting->settingType = NVDRS_DWORD_TYPE;
                pSetting->u32CurrentValue = 7; // DX12 (1) | DX11 (2) | Vulkan (4)
                LOG_DEBUG("NvAPI_DRS_GetSetting: Intercepted Smooth Motion Enabled APIs -> 7");
            }
        }
    }

    return result;
}

bool NvApiHooks::ApplySmoothMotionDrs(bool enable)
{
    const auto& gpu = IdentifyGpu::getPrimaryGpu();
    if (State::Instance().isRunningOnLinux || gpu.usesVkd3dProton || gpu.vendorId != VendorId::Nvidia)
    {
        LOG_INFO("NvApiHooks::ApplySmoothMotionDrs: skipped on non-Windows or non-Nvidia GPU");
        return false;
    }

    HMODULE nvapiDll = GetModuleHandleW(L"nvapi64.dll");
    bool loadedLocally = false;
    if (!nvapiDll)
    {
        nvapiDll = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (nvapiDll)
            loadedLocally = true;
    }

    if (!nvapiDll)
    {
        LOG_WARN("NvApiHooks::ApplySmoothMotionDrs: nvapi64.dll not found");
        return false;
    }

    auto qi = reinterpret_cast<PFN_NvApi_QueryInterface>(
        KernelBaseProxy::GetProcAddress_()(nvapiDll, "nvapi_QueryInterface"));
    if (!qi)
    {
        LOG_WARN("NvApiHooks::ApplySmoothMotionDrs: nvapi_QueryInterface not found in nvapi64.dll");
        if (loadedLocally)
            FreeLibrary(nvapiDll);
        return false;
    }

    // Check driver version: 0x2926aaad = NvAPI_SYS_GetDriverAndBranchVersion
    using PFN_GetDriverVersion = NvAPI_Status(__cdecl*)(NvU32*, NvAPI_ShortString);
    auto pfnGetDriverVersion = reinterpret_cast<PFN_GetDriverVersion>(qi(0x2926aaad));
    if (pfnGetDriverVersion)
    {
        NvU32 driverVersion = 0;
        NvAPI_ShortString branchString {};
        if (pfnGetDriverVersion(&driverVersion, branchString) == NVAPI_OK)
        {
            LOG_INFO("NvApiHooks::ApplySmoothMotionDrs: detected NVIDIA driver version {}", driverVersion);
            if (driverVersion < MIN_SMOOTH_MOTION_DRIVER_VERSION)
            {
                LOG_WARN("NvApiHooks::ApplySmoothMotionDrs: Smooth Motion requires NVIDIA driver 571.86 or newer "
                         "(detected: {})",
                         driverVersion);
                if (loadedLocally)
                    FreeLibrary(nvapiDll);
                return false;
            }
        }
    }

    // Initialize NVAPI if needed
    using PFN_Initialize = NvAPI_Status(__cdecl*)();
    auto pfnInit = reinterpret_cast<PFN_Initialize>(qi(0x0150e828));
    if (pfnInit)
        pfnInit();

    // DRS entry points
    using PFN_CreateSession = NvAPI_Status(__cdecl*)(NvDRSSessionHandle*);
    using PFN_DestroySession = NvAPI_Status(__cdecl*)(NvDRSSessionHandle);
    using PFN_LoadSettings = NvAPI_Status(__cdecl*)(NvDRSSessionHandle);
    using PFN_SaveSettings = NvAPI_Status(__cdecl*)(NvDRSSessionHandle);
    using PFN_FindApplicationByName =
        NvAPI_Status(__cdecl*)(NvDRSSessionHandle, NvAPI_UnicodeString, NvDRSProfileHandle*, NVDRS_APPLICATION*);
    using PFN_CreateProfile = NvAPI_Status(__cdecl*)(NvDRSSessionHandle, NVDRS_PROFILE*, NvDRSProfileHandle*);
    using PFN_CreateApplication = NvAPI_Status(__cdecl*)(NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_APPLICATION*);
    using PFN_SetSetting = NvAPI_Status(__cdecl*)(NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_SETTING*);

    auto pfnCreateSession = reinterpret_cast<PFN_CreateSession>(qi(0x0694d52e));
    auto pfnDestroySession = reinterpret_cast<PFN_DestroySession>(qi(0xdad9cff8));
    auto pfnLoadSettings = reinterpret_cast<PFN_LoadSettings>(qi(0x375dbd6b));
    auto pfnSaveSettings = reinterpret_cast<PFN_SaveSettings>(qi(0xfcbc7e14));
    auto pfnFindApp = reinterpret_cast<PFN_FindApplicationByName>(qi(0xeee566b2));
    auto pfnCreateProfile = reinterpret_cast<PFN_CreateProfile>(qi(0xcc176068));
    auto pfnCreateApp = reinterpret_cast<PFN_CreateApplication>(qi(0x4347a9de));
    auto pfnSetSetting = reinterpret_cast<PFN_SetSetting>(qi(0x577dd202));

    if (!pfnCreateSession || !pfnDestroySession || !pfnLoadSettings || !pfnSaveSettings || !pfnFindApp ||
        !pfnSetSetting)
    {
        LOG_WARN("NvApiHooks::ApplySmoothMotionDrs: Required DRS APIs are missing from driver");
        if (loadedLocally)
            FreeLibrary(nvapiDll);
        return false;
    }

    NvDRSSessionHandle session = nullptr;
    if (pfnCreateSession(&session) != NVAPI_OK || !session)
    {
        LOG_WARN("NvApiHooks::ApplySmoothMotionDrs: Failed to create DRS session");
        if (loadedLocally)
            FreeLibrary(nvapiDll);
        return false;
    }

    bool success = false;
    if (pfnLoadSettings(session) == NVAPI_OK)
    {
        wchar_t exePath[MAX_PATH] {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);

        NvAPI_UnicodeString appName {};
        std::wcsncpy(reinterpret_cast<wchar_t*>(appName), exePath, NVAPI_UNICODE_STRING_MAX - 1);

        NVDRS_APPLICATION appInfo {};
        appInfo.version = NVDRS_APPLICATION_VER;
        NvDRSProfileHandle profile = nullptr;

        auto findStatus = pfnFindApp(session, appName, &profile, &appInfo);
        if (findStatus != NVAPI_OK || !profile)
        {
            if (pfnCreateProfile && pfnCreateApp)
            {
                NVDRS_PROFILE newProfile {};
                newProfile.version = NVDRS_PROFILE_VER;
                const std::wstring profName = L"OptiScaler " + std::filesystem::path(exePath).filename().wstring();
                std::wcsncpy(reinterpret_cast<wchar_t*>(newProfile.profileName), profName.c_str(),
                             NVAPI_UNICODE_STRING_MAX - 1);

                if (pfnCreateProfile(session, &newProfile, &profile) == NVAPI_OK && profile)
                {
                    NVDRS_APPLICATION newApp {};
                    newApp.version = NVDRS_APPLICATION_VER;
                    std::wcsncpy(reinterpret_cast<wchar_t*>(newApp.appName), exePath, NVAPI_UNICODE_STRING_MAX - 1);
                    pfnCreateApp(session, profile, &newApp);
                }
            }
        }

        if (profile)
        {
            NVDRS_SETTING enableSetting {};
            enableSetting.version = NVDRS_SETTING_VER;
            enableSetting.settingId = NVDRS_SETTING_SMOOTH_MOTION_ENABLE;
            enableSetting.settingType = NVDRS_DWORD_TYPE;
            enableSetting.u32CurrentValue = enable ? 1 : 0;
            pfnSetSetting(session, profile, &enableSetting);

            if (enable)
            {
                NVDRS_SETTING apisSetting {};
                apisSetting.version = NVDRS_SETTING_VER;
                apisSetting.settingId = NVDRS_SETTING_SMOOTH_MOTION_APIS;
                apisSetting.settingType = NVDRS_DWORD_TYPE;
                apisSetting.u32CurrentValue = 7; // All APIs
                pfnSetSetting(session, profile, &apisSetting);
            }

            if (pfnSaveSettings(session) == NVAPI_OK)
            {
                LOG_INFO("NvApiHooks::ApplySmoothMotionDrs: Successfully applied Smooth Motion (enable={}) to driver "
                         "profile",
                         enable);
                success = true;
            }
            else
            {
                LOG_WARN("NvApiHooks::ApplySmoothMotionDrs: Failed to save DRS settings");
            }
        }
    }

    pfnDestroySession(session);
    if (loadedLocally)
        FreeLibrary(nvapiDll);

    return success;
}

NvAPI_Status __stdcall NvApiHooks::hkNvAPI_D3D12_SetFlipConfig(void* pCommandQueue, NvU32 dwFlags, void* pParams)
{
    LOG_TRACE("hkNvAPI_D3D12_SetFlipConfig: pCommandQueue={:p}, dwFlags=0x{:X}, pParams={:p}", pCommandQueue, dwFlags,
              pParams);
    return NVAPI_OK;
}

void* __stdcall NvApiHooks::hkNvAPI_QueryInterface(unsigned int InterfaceId)
{
    // Native Reflex, flip metering, architecture/capability queries and driver
    // presets belong to the external FG owner in this mode. Returning null for
    // a Reflex query would disable it, so forward to the real function table.
    // However, NvAPI_DRS_GetSetting is intercepted to permit multi-frame count overrides,
    // and NvAPI_D3D12_SetFlipConfig is stubbed if the driver does not implement it (e.g. DXVK-NVAPI on Linux).
    if (State::Instance().externalFrameGeneration)
    {
        if (InterfaceId == GET_ID(NvAPI_DRS_GetSetting))
        {
            if (o_NvAPI_QueryInterface && !o_NvAPI_DRS_GetSetting)
                o_NvAPI_DRS_GetSetting =
                    reinterpret_cast<decltype(&NvAPI_DRS_GetSetting)>(o_NvAPI_QueryInterface(InterfaceId));
            return &hkNvAPI_DRS_GetSetting;
        }

        if (InterfaceId == GET_ID(NvAPI_D3D12_SetFlipConfig) || InterfaceId == 0xf3148c42)
        {
            void* realFunc = o_NvAPI_QueryInterface ? o_NvAPI_QueryInterface(InterfaceId) : nullptr;
            if (realFunc)
                return realFunc;

            LOG_INFO("hkNvAPI_QueryInterface: NvAPI_D3D12_SetFlipConfig is unimplemented by driver; providing stub "
                     "returning NVAPI_OK");
            return reinterpret_cast<void*>(&hkNvAPI_D3D12_SetFlipConfig);
        }

        return o_NvAPI_QueryInterface ? o_NvAPI_QueryInterface(InterfaceId) : nullptr;
    }

    if (!o_NvAPI_QueryInterface)
        if (Config::Instance()->UseFakenvapi.value_or_default())
            o_NvAPI_QueryInterface = (PFN_NvApi_QueryInterface) fakenvapi::queryInterface;
        else
            return nullptr;

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();

    // Disable flip metering
    if ((InterfaceId == GET_ID(NvAPI_D3D12_SetFlipConfig) || InterfaceId == 0xf3148c42) &&
        Config::Instance()->DisableFlipMetering.value_or(primaryGpu.vendorId != VendorId::Nvidia))
    {
        LOG_INFO("FlipMetering is disabled (returning NVAPI_OK stub)");
        return reinterpret_cast<void*>(&hkNvAPI_D3D12_SetFlipConfig);
    }

    if ((InterfaceId == GET_ID(NvAPI_D3D12_SetFlipConfig) || InterfaceId == 0xf3148c42) &&
        (primaryGpu.usesVkd3dProton || State::Instance().isRunningOnLinux))
    {
        const auto functionPointer = o_NvAPI_QueryInterface ? o_NvAPI_QueryInterface(InterfaceId) : nullptr;
        if (functionPointer)
            return functionPointer;

        LOG_INFO("hkNvAPI_QueryInterface: NvAPI_D3D12_SetFlipConfig unimplemented on Linux; providing stub returning "
                 "NVAPI_OK");
        return reinterpret_cast<void*>(&hkNvAPI_D3D12_SetFlipConfig);
    }

    if (InterfaceId == GET_ID(NvAPI_D3D_SetSleepMode) || InterfaceId == GET_ID(NvAPI_D3D_Sleep) ||
        InterfaceId == GET_ID(NvAPI_D3D_GetLatency) || InterfaceId == GET_ID(NvAPI_D3D_SetLatencyMarker) ||
        InterfaceId == GET_ID(NvAPI_D3D12_SetAsyncFrameMarker) || InterfaceId == GET_ID(NvAPI_Vulkan_GetLatency) ||
        InterfaceId == GET_ID(NvAPI_Vulkan_SetLatencyMarker) || InterfaceId == GET_ID(NvAPI_Vulkan_SetSleepMode)
#ifdef LOW_LATENCY_INPUTS
        || InterfaceId == GET_ID(NvAPI_D3D_GetSleepStatus)
#endif
    )
    {
#ifdef LOW_LATENCY_INPUTS
        if (InterfaceId == GET_ID(NvAPI_D3D_SetSleepMode))
            return InputReflex::D3D_SetSleepMode;
        if (InterfaceId == GET_ID(NvAPI_D3D_GetSleepStatus))
            return InputReflex::D3D_GetSleepStatus;
        else if (InterfaceId == GET_ID(NvAPI_D3D_Sleep))
            return InputReflex::D3D_Sleep;
        else if (InterfaceId == GET_ID(NvAPI_D3D_GetLatency))
            return InputReflex::D3D_GetLatency;
        else if (InterfaceId == GET_ID(NvAPI_D3D_SetLatencyMarker))
            return InputReflex::D3D_SetLatencyMarker;
        else if (InterfaceId == GET_ID(NvAPI_D3D12_SetAsyncFrameMarker))
            return InputReflex::D3D12_SetAsyncFrameMarker;
#endif

        // LOG_DEBUG("counter: {}, hookReflex()", qiCounter);
        ReflexHooks::hookReflex(o_NvAPI_QueryInterface);
        return ReflexHooks::getHookedReflex(InterfaceId);
    }

    ReflexHooks::hookReflex(o_NvAPI_QueryInterface);

    const auto functionPointer = o_NvAPI_QueryInterface(InterfaceId);

    if (functionPointer)
    {
        if (InterfaceId == GET_ID(NvAPI_GPU_GetArchInfo))
        {
            o_NvAPI_GPU_GetArchInfo = reinterpret_cast<decltype(&NvAPI_GPU_GetArchInfo)>(functionPointer);
            return &hkNvAPI_GPU_GetArchInfo;
        }
        if (InterfaceId == GET_ID(NvAPI_DRS_GetSetting))
        {
            o_NvAPI_DRS_GetSetting = reinterpret_cast<decltype(&NvAPI_DRS_GetSetting)>(functionPointer);
            return &hkNvAPI_DRS_GetSetting;
        }
    }

    // LOG_DEBUG("counter: {} functionPointer: {:X}", qiCounter, (size_t)functionPointer);

    return functionPointer;
}

// Requires HMODULE to make sure nvapi is loaded before calling this function
void NvApiHooks::Hook(HMODULE nvapiModule)
{
    if (o_NvAPI_QueryInterface != nullptr)
        return;

    if (nvapiModule == nullptr)
    {
        LOG_ERROR("Hook called with a nullptr nvapi module");
        return;
    }

    LOG_DEBUG("Trying to hook NvApi");

    o_NvAPI_QueryInterface =
        (PFN_NvApi_QueryInterface) KernelBaseProxy::GetProcAddress_()(nvapiModule, "nvapi_QueryInterface");

    LOG_DEBUG("OriginalNvAPI_QueryInterface = {0:X}", (unsigned long long) o_NvAPI_QueryInterface);

    if (o_NvAPI_QueryInterface != nullptr)
    {
        LOG_INFO("NvAPI_QueryInterface found, hooking!");

        constexpr bool leanMode = true;
        if (fakenvapi::isUsingAsMainNvapi())
            fakenvapi::init(!leanMode);
        else if (State::Instance().activeFgOutput == FGOutput::XeFG)
            fakenvapi::init(leanMode);

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&) o_NvAPI_QueryInterface, hkNvAPI_QueryInterface);
        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook NvAPI_QueryInterface: {:X}", detourResult);
            o_NvAPI_QueryInterface = nullptr;
        }
    }
}

void NvApiHooks::Unhook()
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_NvAPI_QueryInterface != nullptr)
        DetourDetach(&(PVOID&) o_NvAPI_QueryInterface, hkNvAPI_QueryInterface);

    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to unhook NvAPI_QueryInterface: {:X}", detourResult);
    }
    else
    {
        o_NvAPI_QueryInterface = nullptr;
    }
}

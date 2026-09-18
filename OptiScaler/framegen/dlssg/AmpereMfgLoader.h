#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace AmpereMfgLoader
{
struct Status
{
    bool Enabled = false;     // Config says to use it
    bool DllFound = false;    // dlssg_sm86.dll found in OptiScaler/dlssg_sm86/
    bool IniWritten = false;  // dlssg_sm86.ini generated and written
    bool DllLoaded = false;   // LoadLibrary succeeded
    bool FsrFallbackActive = false; // 2X FG on Linux: internal FSR FG active
    bool HasSm75Support = false;    // Loaded runtime binary contains dedicated SM75 kernel family (310.1 or unified 310.9 0.3.1+)
    bool Is3101Runtime = false;     // True if 310.1 runtime (max ceiling 3 / 4X), false if 310.9 runtime (max ceiling 5 / 6X)
    std::wstring LoadedDllPath;     // Absolute path of loaded DLL
    std::string ErrorMessage; // Human-readable error if anything failed
};

Status LastStatus();

/// Called after DLL initialization, once GPU/environment information is available.
void TrySetup();

/// Resolves the companion INI MaxGeneratedFrames setting.
/// MaxFrames is clamped to [1, maxCeiling] (preserving 1 for 2X FG, up to maxCeiling).
/// With NvAPI_D3D12_SetFlipConfig stubbed on Linux, both Windows and Linux cleanly
/// support single-frame 2X FG (MaxGeneratedFrames = 1) without artificial elevation.
inline int ResolveMaxGeneratedFrames(int configuredMaxFrames, bool /*onLinux*/ = false, int maxCeiling = 3)
{
    if (configuredMaxFrames <= 0 || configuredMaxFrames > maxCeiling)
        configuredMaxFrames = maxCeiling;

    return configuredMaxFrames;
}

/// Returns true if Linux FG should fall back to OptiScaler's internal FG pipeline (DLSSG input -> FSRFG/XeFG output)
/// instead of sideloading dlssg_sm86.
/// Mode: "auto" (default, falls back when configuredMaxFrames <= 1), "true"/"on"/"1" (force fallback), "false"/"off"/"0" (force external dlssg_sm86).
inline bool ShouldFallbackToFsrFg(int configuredMaxFrames, bool onLinux, bool mfgUnlockEnabled, const std::string& fallbackSetting = "auto")
{
    if (!onLinux || !mfgUnlockEnabled)
        return false;

    if (fallbackSetting == "true" || fallbackSetting == "1" || fallbackSetting == "on" || fallbackSetting == "True")
        return true;

    if (fallbackSetting == "false" || fallbackSetting == "0" || fallbackSetting == "off" || fallbackSetting == "False")
        return false;

    // "auto": fall back to internal FG on Linux when configured for single-frame (2X FG)
    return (configuredMaxFrames <= 1);
}

/// Resolves the fallback pipeline type string ("fsrfg" or "xefg")
inline std::string ResolveFallbackFgType(const std::string& fallbackType = "fsrfg")
{
    if (fallbackType == "xefg" || fallbackType == "XeFG" || fallbackType == "XEFG")
        return "xefg";
    return "fsrfg";
}

constexpr uint32_t DRS_OVERRIDE_DLSSG_MULTI_FRAME_COUNT_ID = 0x104D6667;
constexpr uint32_t DRS_OVERRIDE_MAX_DLSSG_DYNAMIC_MULTI_FRAME_COUNT_ID = 0x10562D0F;

/// Evaluates whether a DRS query matches a DLSSG multi-frame setting and resolves
/// the overridden value when running on Linux with Ampere MFG unlock enabled.
inline bool TryResolveDrsMultiFrameSetting(uint32_t settingId, int configuredMaxFrames, bool onLinux,
                                          bool mfgUnlockEnabled, uint32_t& outValue, int maxCeiling = 3,
                                          int explicitOverrideCount = 0)
{
    if (!onLinux || !mfgUnlockEnabled)
        return false;

    // Static multi-frame multiplier override:
    // 0x104D6667 forcibly overrides Streamline's frame generation multiplier.
    // We only override it when:
    // 1. An explicit user override is configured (FGDLSSGOverrideInterpolationCount > 0), OR
    // 2. The user configured single-frame generation (configuredMaxFrames == 1), where the Linux 2X
    //    elevation workaround in dlssg_sm86.ini requires setting DRS to 1 to force single-frame 2X FG.
    // If configuredMaxFrames > 1 (e.g. 3 or 5) and no explicit override is set, we do NOT intercept 0x104D6667,
    // allowing the game's in-engine FG setting (e.g. Cyberpunk 2077 2X FG) to control the multiplier without
    // causing extreme swapchain pacing judder and micro-stutter.
    if (settingId == DRS_OVERRIDE_DLSSG_MULTI_FRAME_COUNT_ID)
    {
        if (explicitOverrideCount > 0)
        {
            int clamped = explicitOverrideCount > maxCeiling ? maxCeiling : explicitOverrideCount;
            outValue = static_cast<uint32_t>(clamped);
            return true;
        }
        if (configuredMaxFrames == 1)
        {
            outValue = 1;
            return true;
        }
        return false;
    }

    // Dynamic multi-frame ceiling override:
    // 0x10562D0F sets the maximum generated frame ceiling that Streamline can dynamically select.
    // Do not override when configured for single-frame (<= 1), because Streamline requires dynamic max > 1.
    if (settingId == DRS_OVERRIDE_MAX_DLSSG_DYNAMIC_MULTI_FRAME_COUNT_ID)
    {
        if (configuredMaxFrames <= 1)
            return false;

        int clamped = configuredMaxFrames;
        if (clamped <= 0 || clamped > maxCeiling)
            clamped = maxCeiling;
        outValue = static_cast<uint32_t>(clamped);
        return true;
    }

    return false;
}

/// Formats dlssg_sm86.ini content with Native 0.2.4 specification and strict clamping.
inline std::string FormatIniContent(int maxFrames, const std::string& kernelImg, int hwBilinear = 0, const std::string& router = "SM86", int logLevel = 1)
{
    // Native 0.2.4 strictly requires: MaxGeneratedFrames must be 1, 2 or 3
    if (maxFrames <= 0 || maxFrames > 3)
        maxFrames = 3;

    std::string validKernel = kernelImg;
    if (validKernel != "PTX" && validKernel != "Cubin")
        validKernel = "Auto";

    std::string validRouter = router;
    if (validRouter != "SM75" && validRouter != "SM86" && validRouter != "Auto")
        validRouter = "SM86";

    int validHwBilinear = (hwBilinear == 1) ? 1 : 0;
    int validLogLevel = (logLevel >= 0 && logLevel <= 3) ? logLevel : 1;

    std::ostringstream ss;
    ss << "; Native 0.2.4. Restart the game after changing this file.\n";
    ss << "[Compatibility]\n";
    ss << "Router=" << validRouter << "\n";
    ss << "KernelImage=" << validKernel << "\n";
    ss << "HardwareBilinear=" << validHwBilinear << "\n\n";
    ss << "[FrameGeneration]\n";
    ss << "MaxGeneratedFrames=" << maxFrames << "\n\n";
    ss << "[Logging]\n";
    ss << "Level=" << validLogLevel << "\n";

    return ss.str();
}

/// Formats dlssg_sm86.ini content with 0.3.x specification ([General], [FrameGeneration] Optimized 0-3, MaxGeneratedFrames up to 5, [Compatibility] Preset, SpoofArchToGame).
inline std::string FormatIniContent030(int maxFrames, int optimized = 1, const std::string& preset = "Auto",
                                       const std::string& kernelImg = "Auto", int hwBilinear = 0,
                                       const std::string& router = "Auto", int logLevel = 1,
                                       const std::string& spoofArch = "Auto")
{
    // Clamping of MaxGeneratedFrames for 0.3.x: 1 to 5 (5 = 6X)
    if (maxFrames <= 0 || maxFrames > 5)
        maxFrames = 5;

    std::string validKernel = kernelImg;
    if (validKernel != "PTX" && validKernel != "Cubin")
        validKernel = "Auto";

    std::string validRouter = router;
    if (validRouter != "SM75" && validRouter != "SM86" && validRouter != "Auto")
        validRouter = "Auto";

    std::string validPreset = preset;
    if (validPreset != "A" && validPreset != "B" && validPreset != "a" && validPreset != "b")
        validPreset = "Auto";
    else if (validPreset == "a")
        validPreset = "A";
    else if (validPreset == "b")
        validPreset = "B";

    int validOptimized = (optimized >= 0 && optimized <= 3) ? optimized : 1;
    int validHwBilinear = (hwBilinear == 1) ? 1 : 0;
    int validLogLevel = (logLevel >= 0 && logLevel <= 3) ? logLevel : 1;

    std::ostringstream ss;
    ss << "; DLSSG SM86 0.3.x configuration. Restart the game after changing this file.\n";
    ss << "[General]\n";
    ss << "Enabled=1\n\n";
    ss << "[FrameGeneration]\n";
    ss << "Optimized=" << validOptimized << "\n";
    ss << "MaxGeneratedFrames=" << maxFrames << "\n\n";
    ss << "[Compatibility]\n";
    ss << "Preset=" << validPreset << "\n";
    ss << "Router=" << validRouter << "\n";
    ss << "KernelImage=" << validKernel << "\n";
    ss << "HardwareBilinear=" << validHwBilinear << "\n";
    if (spoofArch == "1" || spoofArch == "true")
        ss << "SpoofArchToGame=1\n";
    else if (spoofArch == "0" || spoofArch == "false")
        ss << "SpoofArchToGame=0\n";
    ss << "\n";
    ss << "[Logging]\n";
    ss << "Level=" << validLogLevel << "\n";
    ss << "Directory=dlssg_sm86\\logs\n\n";
    ss << "[Runtime]\n";
    ss << "Mode=Bundled\n";

    return ss.str();
}

/// Checks if an architecture ID represents Turing (SM75).
inline bool IsTuringArch(uint32_t archId)
{
    return (archId == 0x00000160) || ((archId & 0xFFF0) == 0x0160);
}

/// Checks if an architecture ID represents Ampere (SM86).
inline bool IsAmpereArch(uint32_t archId)
{
    return (archId == 0x00000170) || ((archId & 0xFFF0) == 0x0170);
}

/// Detects if a dlssg_sm86 binary contains the SM75 kernel family (310.1 build or 0.3.1+ unified 310.9 build).
inline bool HasSm75KernelFamily(const std::filesystem::path& dllPath)
{
    if (dllPath.empty())
        return false;

    if (dllPath.wstring().find(L"310.1") != std::wstring::npos)
        return true;

    std::ifstream file(dllPath, std::ios::binary);
    if (!file.is_open())
        return false;

    constexpr size_t bufferSize = 65536;
    std::string buffer(bufferSize, '\0');
    const std::string needleSm75Slots = "DLSSG_SM75_SLOTS";
    const std::string needleSm75Family = "sm75_family";
    const std::string needleCubinSm75 = "cubin_sm75";
    const std::string needleSm75Hw = "executed_on_sm75_hardware";
    const std::string needle3109NoSm75 = "The 310.9 backend has no SM75";

    bool foundSm75 = false;
    std::string overlap;
    while (file.read(buffer.data(), bufferSize) || file.gcount() > 0)
    {
        size_t bytesRead = file.gcount();
        std::string chunk = overlap + std::string(buffer.data(), bytesRead);
        if (chunk.find(needle3109NoSm75) != std::string::npos)
            return false;
        if (!foundSm75 && (chunk.find(needleSm75Slots) != std::string::npos ||
                           chunk.find(needleSm75Family) != std::string::npos ||
                           chunk.find(needleCubinSm75) != std::string::npos ||
                           chunk.find(needleSm75Hw) != std::string::npos))
        {
            foundSm75 = true;
        }
        constexpr size_t maxNeedle = 64;
        if (chunk.size() >= maxNeedle)
            overlap = chunk.substr(chunk.size() - maxNeedle + 1);
        else
            overlap = chunk;
    }

    return foundSm75;
}

/// Detects if a dlssg_sm86 binary represents the 310.1 runtime (4X / MaxGeneratedFrames=3 ceiling)
/// rather than the 310.9+ runtime (6X / MaxGeneratedFrames=5 ceiling).
inline bool Is3101Runtime(const std::filesystem::path& dllPath)
{
    if (dllPath.empty())
        return false;

    if (dllPath.wstring().find(L"310.1") != std::wstring::npos)
        return true;

    std::ifstream file(dllPath, std::ios::binary);
    if (!file.is_open())
        return false;

    constexpr size_t bufferSize = 65536;
    std::string buffer(bufferSize, '\0');
    const std::string needle3101 = "dlssg-310.1";
    const std::string needle3109 = "dlssg-310.9";

    std::string overlap;
    while (file.read(buffer.data(), bufferSize) || file.gcount() > 0)
    {
        size_t bytesRead = file.gcount();
        std::string chunk = overlap + std::string(buffer.data(), bytesRead);
        if (chunk.find(needle3109) != std::string::npos)
            return false;
        if (chunk.find(needle3101) != std::string::npos)
            return true;
        constexpr size_t maxNeedle = 32;
        if (chunk.size() >= maxNeedle)
            overlap = chunk.substr(chunk.size() - maxNeedle + 1);
        else
            overlap = chunk;
    }

    return false;
}

/// Resolves router string ("Auto", "SM75" or "SM86") based on architecture ID, GPU name, configured preference,
/// and whether the selected runtime binary supports dedicated SM75 kernels.
inline std::string ResolveRouter(uint32_t archId, const std::string& gpuName = "", const std::string& configuredRouter = "Auto", bool hasSm75Support = true)
{
    if (configuredRouter == "SM75" || configuredRouter == "sm75")
    {
        // Coerce to Auto if loaded runtime has no SM75 kernel family (e.g. 310.9) to prevent runtime abort
        return hasSm75Support ? "SM75" : "Auto";
    }
    if (configuredRouter == "SM86" || configuredRouter == "sm86")
        return "SM86";

    // Auto router resolution:
    // If running on Turing, only select SM75 if the runtime actually supports it (310.1).
    // On 310.9, use "Auto" which is safe and prevents the abort message:
    // "The 310.9 backend has no SM75 kernel family; use Router=Auto or SM86"
    if (IsTuringArch(archId))
        return hasSm75Support ? "SM75" : "Auto";
    if (IsAmpereArch(archId))
        return "SM86";

    // Fallback: name matching
    if (!gpuName.empty())
    {
        if (gpuName.find("RTX 20") != std::string::npos ||
            gpuName.find("GTX 16") != std::string::npos ||
            gpuName.find("TITAN RTX") != std::string::npos ||
            gpuName.find("Turing") != std::string::npos ||
            gpuName.find("TU10") != std::string::npos ||
            gpuName.find("TU11") != std::string::npos)
            return hasSm75Support ? "SM75" : "Auto";

        if (gpuName.find("RTX 30") != std::string::npos ||
            gpuName.find("Ampere") != std::string::npos ||
            gpuName.find("GA10") != std::string::npos ||
            gpuName.find("RTX A") != std::string::npos)
            return "SM86";
    }

    return "SM86";
}

/// Resolves router string for current hardware.
std::string ResolveRouter();

/// Generates dlssg_sm86.ini content from OptiScaler config values.
std::string GenerateIniContent();
std::string GenerateIniContent(bool hasSm75Support);
std::string GenerateIniContent(bool hasSm75Support, bool is3101Runtime);

/// Resolves optimal kernel image format for current hardware/environment when Auto is requested.
std::string ResolveAutoKernelImage();

inline std::string ResolveAutoKernelImage(uint32_t archId, const std::string& name, bool onLinux)
{
    return onLinux || IsTuringArch(archId) || name.find("RTX 20") != std::string::npos ||
                   name.find("GTX 16") != std::string::npos || name.find("TITAN RTX") != std::string::npos ||
                   name.find("Turing") != std::string::npos || name.find("TU10") != std::string::npos ||
                   name.find("TU11") != std::string::npos || name.find("3080 Ti") != std::string::npos ||
                   name.find("3080Ti") != std::string::npos || name.find("Laptop") != std::string::npos ||
                   name.find("Mobile") != std::string::npos
               ? "PTX" : "Auto";
}
} // namespace AmpereMfgLoader


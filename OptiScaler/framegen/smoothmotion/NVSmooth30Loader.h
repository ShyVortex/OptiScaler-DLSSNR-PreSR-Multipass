#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace NVSmooth30Loader
{
struct Status
{
    bool Enabled = false;            // Config says to use it
    bool DllFound = false;           // nvsmooth30.dll found in OptiScaler directory
    bool DllLoaded = false;          // LoadLibrary succeeded
    bool SmoothMotionActive = false; // NVIDIA Smooth Motion DRS setting applied
    std::wstring LoadedDllPath;      // Absolute path of loaded DLL
    std::string ErrorMessage;        // Human-readable error if anything failed
};

Status LastStatus();

/// Called after DLL initialization, once GPU/environment information is available.
void TrySetup();

/// Evaluates whether an architecture ID represents Nvidia Ampere (SM86).
inline bool IsAmpereArch(uint32_t archId) { return (archId == 0x00000170) || ((archId & 0xFFF0) == 0x0170); }

/// Evaluates candidate paths for nvsmooth30.dll, strictly prioritizing the OptiScaler directory.
inline std::filesystem::path ResolveCandidatePath(const std::filesystem::path& basePath,
                                                  const std::filesystem::path& mainOverride = {})
{
    // 1. mainOverride / "nvsmooth30.dll"
    if (!mainOverride.empty())
    {
        auto candidate = mainOverride / L"nvsmooth30.dll";
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec))
            return candidate;
    }

    // 2. basePath / "OptiScaler" / "nvsmooth30.dll" (Mandatory placement inside OptiScaler directory)
    {
        auto candidate = basePath / L"OptiScaler" / L"nvsmooth30.dll";
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec))
            return candidate;
    }

    // 3. basePath / "nvsmooth30.dll" (fallback if game root has OptiScaler files directly)
    {
        auto candidate = basePath / L"nvsmooth30.dll";
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec))
            return candidate;
    }

    return {};
}

} // namespace NVSmooth30Loader

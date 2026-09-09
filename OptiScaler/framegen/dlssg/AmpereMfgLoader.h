#pragma once

#include <cstdint>
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
    std::string ErrorMessage; // Human-readable error if anything failed
};

const Status& LastStatus();

/// Called once at startup. Writes the INI and loads the DLL if enabled + Ampere GPU.
void TrySetup();

/// Formats dlssg_sm86.ini content with Native 0.2.3 specification and strict clamping.
inline std::string FormatIniContent(int maxFrames, const std::string& kernelImg, int hwBilinear = 0, const std::string& router = "SM86", int logLevel = 1)
{
    // Native 0.2.3 strictly requires: MaxGeneratedFrames must be 1, 2 or 3
    if (maxFrames <= 0 || maxFrames > 3)
        maxFrames = 3;

    std::string validKernel = kernelImg;
    if (validKernel != "PTX" && validKernel != "Cubin")
        validKernel = "Auto";

    std::string validRouter = router;
    if (validRouter != "SM75" && validRouter != "SM86")
        validRouter = "SM86";

    int validHwBilinear = (hwBilinear == 1) ? 1 : 0;
    int validLogLevel = (logLevel >= 0 && logLevel <= 3) ? logLevel : 1;

    std::ostringstream ss;
    ss << "; Native 0.2.3. Restart the game after changing this file.\n";
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

/// Resolves router string ("SM75" or "SM86") based on architecture ID and GPU name.
inline std::string ResolveRouter(uint32_t archId, const std::string& gpuName = "")
{
    if (IsTuringArch(archId))
        return "SM75";
    if (IsAmpereArch(archId))
        return "SM86";

    // Fallback: name matching
    if (!gpuName.empty())
    {
        if (gpuName.find("RTX 20") != std::string::npos ||
            gpuName.find("GTX 16") != std::string::npos ||
            gpuName.find("Turing") != std::string::npos)
            return "SM75";

        if (gpuName.find("RTX 30") != std::string::npos ||
            gpuName.find("Ampere") != std::string::npos)
            return "SM86";
    }

    return "SM86";
}

/// Resolves router string ("SM75" or "SM86") for current hardware.
std::string ResolveRouter();

/// Generates dlssg_sm86.ini content from OptiScaler config values.
std::string GenerateIniContent();

/// Resolves optimal kernel image format for current hardware/environment when Auto is requested.
std::string ResolveAutoKernelImage();
} // namespace AmpereMfgLoader

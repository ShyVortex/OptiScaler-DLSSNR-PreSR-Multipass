#pragma once

#include "AmpereMfgLoader.h"

#include <string>

// Plain-language status for the RTX 20/30 MFG unlocker section. Users read the old "Live: n/a" as the unlocker not
// running; live control is only the SilyNoMeta command API (see FG-AMPERE-MFG-SDLI1995-040-DETECTION-FIX.md), so
// the headline says whether the unlocker runs and the second line says where the multiplier is changed.
namespace AmpereMfgStatusText
{
enum class Headline
{
    Running,
    NotRunning,
    Starting
};

inline Headline ResolveHeadline(const AmpereMfgLoader::Status& status)
{
    if (!status.ErrorMessage.empty())
        return Headline::NotRunning;
    return status.DllLoaded ? Headline::Running : Headline::Starting;
}

inline std::string ModName(const AmpereMfgLoader::Status& status)
{
    return status.ModName.empty() ? "sdli1995" : status.ModName;
}

inline std::string HeadlineText(const AmpereMfgLoader::Status& status)
{
    switch (ResolveHeadline(status))
    {
    case Headline::Running:
        return "RTX 20/30 frame generation unlock: running (" + ModName(status) + ")";
    case Headline::NotRunning:
        return "RTX 20/30 frame generation unlock: not running: " + status.ErrorMessage;
    default:
        return "RTX 20/30 frame generation unlock: starting...";
    }
}

inline const char* MultiplierText(const AmpereMfgLoader::Status& status)
{
    return status.LiveControlSupported
               ? "Multiplier: set in the game's menu or live from OptiScaler (Dynamic MFG available)."
               : "Multiplier: set in the game's graphics menu. Ceiling changes need Save Settings + restart.";
}

inline const char* LiveControlText(const AmpereMfgLoader::Status& status)
{
    if (status.LiveControlActive)
        return "active";
    return status.LiveControlSupported ? "ready" : "not supported by this mod";
}

// The mod needs an R580 driver (hard since sdli1995 0.4.1: below it frame generation is refused, no PTX fallback).
// Windows only: under Wine/Proton dxvk-nvapi reports Linux driver numbers; an unknown version (0) says nothing.
inline constexpr uint32_t MinimumDriver = 58000; // NVAPI encoding: 580.00

inline bool DriverBelowFloor(bool onWine, uint32_t driverVersion)
{
    return !onWine && driverVersion != 0 && driverVersion < MinimumDriver;
}

inline std::string DriverFloorText(uint32_t driverVersion)
{
    const uint32_t minor = driverVersion % 100;
    const std::string version = std::to_string(driverVersion / 100) + (minor < 10 ? ".0" : ".") + std::to_string(minor);
    return "NVIDIA driver " + version +
           " is too old: this unlocker needs driver 580 or newer and refuses frame generation below it.";
}

inline constexpr const char* HardwareBilinearLabel = "Hardware Bilinear (0.3.x / 0.4.0 only)##sm86";

// Numeric dotted comparison of versions the mod states ("0.4.1", "0.3.5-5"); a -suffix does not affect ordering.
// Returns <0, 0 or >0.
inline int CompareModVersion(const std::string& a, const std::string& b)
{
    size_t i = 0;
    size_t j = 0;
    while (true)
    {
        const auto part = [](const std::string& s, size_t& pos) -> long
        {
            if (pos >= s.size() || s[pos] == '-')
                return 0;
            long value = 0;
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
                value = value * 10 + (s[pos++] - '0');
            if (pos < s.size() && s[pos] == '.')
                ++pos;
            return value;
        };
        const bool aDone = i >= a.size() || a[i] == '-';
        const bool bDone = j >= b.size() || b[j] == '-';
        if (aDone && bDone)
            return 0;
        const long x = part(a, i);
        const long y = part(b, j);
        if (x != y)
            return x < y ? -1 : 1;
    }
}

// sdli1995 0.4.1 removed [Compatibility] HardwareBilinear (and runs [DLSSG] Preset=Auto as A). Only a build that
// states 0.4.1 or newer is known to ignore it; 0.4.0 and older state no version and still read it.
inline bool IsSdli1995AtLeast041(const AmpereMfgLoader::Status& status)
{
    return status.Variant == AmpereMfgLoader::ModVariant::Sdli1995 && !status.ModVersion.empty() &&
           CompareModVersion(status.ModVersion, "0.4.1") >= 0;
}

inline bool HardwareBilinearHonoured(const AmpereMfgLoader::Status& status) { return !IsSdli1995AtLeast041(status); }

// 0.4.1 removed [DLSSG] Preset=Auto and runs it as A; the stored value stays "Auto" for builds that still honour it.
inline bool PresetAutoRunsAsA(const AmpereMfgLoader::Status& status) { return IsSdli1995AtLeast041(status); }

inline const char* PresetAutoLabel(const AmpereMfgLoader::Status& status)
{
    return PresetAutoRunsAsA(status) ? "Auto (runs as A in this build)" : "Auto (Game / Profile default)";
}

inline constexpr const char* HardwareBilinearIgnoredText =
    "sdli1995 0.4.1 and newer no longer read this setting, so it is locked for this build.\n"
    "Your saved value is kept and applies again with builds that still read it.";

// Technical fields for bug reports, shown in the headline's help marker.
inline std::string DetailsText(const AmpereMfgLoader::Status& status, const std::string& router)
{
    return "Mod: " + ModName(status) + "\nDLL: " + (status.DllFound ? "found" : "missing") + "\nRouter: " + router +
           "\nINI: " + (status.IniWritten ? "written" : "not written") +
           "\nLoaded: " + (status.DllLoaded ? "yes" : "no") + "\nLive control: " + LiveControlText(status);
}
} // namespace AmpereMfgStatusText

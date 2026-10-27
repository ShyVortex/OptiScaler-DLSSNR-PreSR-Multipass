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

// Technical fields for bug reports, shown in the headline's help marker.
inline std::string DetailsText(const AmpereMfgLoader::Status& status, const std::string& router)
{
    return "Mod: " + ModName(status) + "\nDLL: " + (status.DllFound ? "found" : "missing") + "\nRouter: " + router +
           "\nINI: " + (status.IniWritten ? "written" : "not written") +
           "\nLoaded: " + (status.DllLoaded ? "yes" : "no") + "\nLive control: " + LiveControlText(status);
}
} // namespace AmpereMfgStatusText

#pragma once

#include <SysUtils.h>
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

/// Generates dlssg_sm86.ini content from OptiScaler config values.
std::string GenerateIniContent();
} // namespace AmpereMfgLoader

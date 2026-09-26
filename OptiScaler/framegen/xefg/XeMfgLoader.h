#pragma once

#include <SysUtils.h>
#include <string>

namespace XeMfgLoader
{
struct Status
{
    bool ModuleFound = false;           // libxess_fg.dll or igxess_fg.dll was located
    bool Patched = false;               // all required unlock patches applied
    bool Applied = false;               // alias for Patched
    unsigned int PatchesApplied = 0;    // count of applied patches (0..5)
    unsigned int PacingDetours = 0;     // count of applied pacing detours (0..3)
    bool PacingInstalled = false;       // extra presentation pacing installed
    bool VerifiedPacing = false;        // alias for PacingInstalled
    bool PatchFailed = false;           // memory protection or write operation failed
    bool RollbackFailed = false;        // at least one original byte state could not be restored
    unsigned int ConfiguredCeiling = 3; // configured max frames (1..5)
    std::string ModuleVersion;          // file version of libxess_fg.dll
    std::string ErrorMessage;           // optional failure reason
};

enum class Failure
{
    None,
    PatchFailed,
    RollbackFailed
};

Status LastStatus();
bool EnabledForSession();
inline bool IsEnabled() { return EnabledForSession(); }
bool Pending();

// The generated frame ceiling the patches unlocked, or 1 when unpatched/stock
unsigned int UnlockedMax();

// Clamps or reports effective maximum given native capability
unsigned int EffectiveMax(unsigned int nativeMaximum = 1);

// Applies the 5 patches and optional pacing hooks to libxess_fg.dll / igxess_fg.dll
void TryApply(HMODULE module = nullptr);

// Dynamically sets the max generated frames ceiling
void SetMaxGeneratedFrames(unsigned int maxFrames);

Failure LastFailure();

// Transactional memory patch test helper for automated unit tests
bool ApplyToMemory(uint8_t* baseAddress, size_t imageSize, unsigned int maxFrames, bool enablePacing,
                   Status& outStatus);
void RollbackMemory(uint8_t* baseAddress, Status& outStatus);

} // namespace XeMfgLoader

#pragma once

#if defined(OPTISCALER_RTX40_MFG)

#include <SysUtils.h>

#include <string>

// Multi Frame Generation on Ada.
//
// nvngx_dlssg.dll gates MFG on the architecture id reported by the driver: 0x1b0 is Blackwell, Ada
// is below it. Two sites decide what a card is allowed to do, and both compare against that constant.
//
//   Advertise, the function that publishes DLSSG.MultiFrameCountMax:
//       mov   ebx, 0x1
//       mov   r8d, 0x3            the Blackwell count
//       cmp   edi, 0x1b0
//       cmovl r8d, ebx            below Blackwell the count becomes 1
//
//   Validate, the function that accepts or rejects a requested count:
//       cmp   eax, 0x1b0
//       jl    ada                 Ada takes this branch and accepts only 1
//       cmp   ebx, 0x3
//       jbe   accept
//
// Patched: the count immediates become 5, the cmovl becomes a nop, and the jl becomes two nops. The
// result is a maximum of five generated frames -- 6X -- on supported Ada GPUs.
//
// Memory only. The file on disk carries an Authenticode signature and is left alone.
//
// A Streamline wrapper between the game and the snippet can carry a lower ceiling of its own. That
// one is raised where the count crosses slDLSSGGetState. Advertise and validate have no such
// boundary: nothing stands between sl.dlss_g.dll and nvngx_dlssg.dll to intercept.
//
// Ada also runs a different interpolation kernel: Kernel_EstimateIntermMvecsScatter reads three f32
// fields of its parameter block on sm_120 and one on sm_89, so every generated frame lands at the
// same point between the two real ones. Optional experimental Blackwell-image retargeting is separate
// from the gate unlock and is disabled by default; opening the gates does not fix interpolation timing.
namespace MfgUnlock
{
// What the last attempt found. The signatures are version specific by construction -- they carry the
// shape of the code they patch -- so a module this does not recognise is the expected outcome on a
// version nobody has looked at yet, not a fault. The menu reports this so a report comes back with a
// version number attached rather than "it does not work".
struct Status
{
    bool ModuleFound = false; // nvngx_dlssg.dll was loaded
    bool AdvertiseMatched = false;
    bool ValidateMatched = false;
    bool ArchGatesPatched = false;
    unsigned int KernelsRewritten = 0;
    bool PatchFailed = false;    // an intended write/protection/cache operation failed
    bool RollbackFailed = false; // at least one original byte/protection/cache state could not be restored
    std::string SnippetVersion;  // file version of nvngx_dlssg.dll, empty if it could not be read
};

enum class Failure
{
    None,
    PatchFailed,
    RollbackFailed
};

Status LastStatus();
bool EnabledForSession();

// Applies the patches once per process. Silent and harmless when the config option is off, when
// nvngx_dlssg.dll is not loaded, or when a signature does not match exactly once.
void TryApply(HMODULE module = nullptr);
bool Pending();

// The generated frame ceiling the patches opened, or 0 when they did not land.
unsigned int UnlockedMax();

// Applies the verified ceiling to a native report. Any failed patch attempt is fail-closed because an
// incompletely restored advertise site could otherwise make the native runtime report a false maximum.
unsigned int EffectiveMax(unsigned int nativeMaximum);

// Cheap failure severity for per-frame consumers that must clamp or disable behavior without copying
// the full Status (including its version string).
Failure LastFailure();

// Applies only the complete, unambiguous legacy or 310.9 gate set. Unknown comparisons are rejected.
bool PatchArchGates(HMODULE module);
} // namespace MfgUnlock

#endif

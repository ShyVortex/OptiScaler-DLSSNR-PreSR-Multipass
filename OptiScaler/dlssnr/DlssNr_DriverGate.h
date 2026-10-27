#pragma once

#include <atomic>
#include <cstdint>
#include <string>

// Neural Rendering needs NGX support for feature 18. Drivers without it reject the feature with FAIL_OutOfDate,
// and forcing the model through the direct runtime then hangs the GPU (DLSS Unlocked #60, 576.88). 616.64 is
// the first officially supported driver; 610.88 is the oldest verified to work. Kept free of Windows headers so
// the unit test runs the same rule.
namespace DlssNr::DriverGate
{
inline constexpr uint32_t MinimumWindowsDriver = 61088;     // NVAPI encoding: 610.88, oldest verified
inline constexpr uint32_t RecommendedWindowsDriver = 61664; // 616.64, first with official NR support

enum class Verdict
{
    Supported,
    TooOld,
    Unknown,      // NVAPI did not report a version; allowed, the OutOfDate check still applies
    NotApplicable // Wine/Proton: dxvk-nvapi reports Linux driver numbers, not comparable
};

constexpr Verdict Resolve(bool onWine, uint32_t driverVersion)
{
    if (onWine)
        return Verdict::NotApplicable;
    if (driverVersion == 0)
        return Verdict::Unknown;
    return driverVersion < MinimumWindowsDriver ? Verdict::TooOld : Verdict::Supported;
}

inline std::string FormatVersion(uint32_t driverVersion)
{
    const uint32_t minor = driverVersion % 100;
    return std::to_string(driverVersion / 100) + (minor < 10 ? ".0" : ".") + std::to_string(minor);
}

inline std::string BlockedMessage(uint32_t driverVersion)
{
    return "Neural Rendering requires NVIDIA driver " + FormatVersion(MinimumWindowsDriver) + " or newer (" +
           FormatVersion(RecommendedWindowsDriver) + "+ recommended; detected " + FormatVersion(driverVersion) +
           "). Update your driver to enable it.";
}

// Status reason when NGX itself reports FAIL_OutOfDate (Wine, unknown versions, or the proxy's own refusal).
// A literal because the status keeps a const char*; the unit test checks it matches the constants above.
inline constexpr const char* UnsupportedDriverReason =
    "the NVIDIA driver does not support Neural Rendering; update to 610.88 or newer (616.64+ recommended)";

// NGX's "driver too old for this feature": the direct runtime must never be forced past it.
inline constexpr unsigned int NgxResultOutOfDate = 0xBAD0000C;

// Set once at startup from the GPU worker; read by the menu and the NR proxy.
inline std::atomic<uint32_t> detectedVersion { 0 };
inline std::atomic<bool> blocked { false };

inline bool Blocked() { return blocked.load(std::memory_order_acquire); }
inline uint32_t DetectedVersion() { return detectedVersion.load(std::memory_order_acquire); }

inline void Publish(Verdict verdict, uint32_t driverVersion)
{
    detectedVersion.store(driverVersion, std::memory_order_release);
    blocked.store(verdict == Verdict::TooOld, std::memory_order_release);
}
} // namespace DlssNr::DriverGate

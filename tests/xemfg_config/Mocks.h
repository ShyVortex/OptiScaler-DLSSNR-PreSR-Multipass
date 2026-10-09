#pragma once
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#define LOG_DEBUG(...) ((void) 0)
#define LOG_INFO(...) ((void) 0)
#define LOG_WARN(...) ((void) 0)
#define LOG_ERROR(...) ((void) 0)

#include "production-config.inc"
struct Config
{
    CustomOptional<bool> XeMfgUnlock { false };
    CustomOptional<bool> XeMfgExtraPacing { false };
    CustomOptional<int> XeMfgMaxFrames { 3 };
    CustomOptional<bool> ExternalFrameGeneration { false };
    CustomOptional<bool> FGDLSSGAdaMfgUnlock { false };
    CustomOptional<bool> FGDLSSGAmpereMfgUnlock { false };
    static Config* Instance()
    {
        static Config config;
        return &config;
    }
};

struct version_t
{
    unsigned major = 1, minor = 3, patch = 1, reserved = 78;
};
inline DWORD WINAPI XeTestFileName(HMODULE, LPWSTR output, DWORD size)
{
    constexpr wchar_t path[] = L"C:\\fixture\\libxess_fg.dll";
    if (size <= std::size(path))
        return 0;
    std::copy(std::begin(path), std::end(path), output);
    return static_cast<DWORD>(std::size(path) - 1);
}
namespace Util
{
inline bool GetFileVersion(const std::wstring&, version_t* version)
{
    *version = version_t {};
    return true;
}
} // namespace Util
#define GetModuleFileNameW XeTestFileName

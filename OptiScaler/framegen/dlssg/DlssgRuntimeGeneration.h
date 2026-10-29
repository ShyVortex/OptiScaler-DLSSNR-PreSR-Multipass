#pragma once

#include <cstdint>
#include <cwctype>
#include <functional>
#include <string>
#include <vector>

// The DLSS-G runtime a game ships decides whether the RTX 20/30 unlocker can take it over. Since sdli1995 0.4.1 a
// pre-310 runtime ("generation A", 1.0.1 - 3.8.x) is forwarded unchanged and NVIDIA's runtime refuses RTX 20/30, so
// the game asks for DLSS-G every frame and gets nothing (DLSS Unlocked #61, FG-REMNANT2-GENERATION-A-DLSSG.md).
// NGX loads the first nvngx_dlssg.dll on the feature path list, so that is the file to classify.
namespace DlssgRuntimeGeneration
{
enum class Generation
{
    A,
    B,
    Unknown
};

inline constexpr const wchar_t* RuntimeFileName = L"nvngx_dlssg.dll";

// 310.x introduced the DLSSG.MultiFrameCount contract; an unread version (0) says nothing.
inline Generation Classify(uint32_t major)
{
    if (major == 0)
        return Generation::Unknown;

    return major < 310 ? Generation::A : Generation::B;
}

inline std::string FormatVersion(uint32_t major, uint32_t minor, uint32_t patch)
{
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

// Lower case, backslashes, no trailing separator: NGX gets paths with both separators and with or without one.
inline std::wstring NormalizePath(const std::wstring& path)
{
    std::wstring result;
    result.reserve(path.size());

    for (auto c : path)
        result.push_back(c == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(c)));

    while (!result.empty() && result.back() == L'\\')
        result.pop_back();

    return result;
}

inline bool IsUnder(const std::wstring& path, const std::wstring& root)
{
    const auto p = NormalizePath(path);
    const auto r = NormalizePath(root);

    if (r.empty() || p.size() < r.size() || p.compare(0, r.size(), r) != 0)
        return false;

    return p.size() == r.size() || p[r.size()] == L'\\';
}

inline std::wstring JoinRuntimePath(const std::wstring& directory)
{
    if (!directory.empty() && (directory.back() == L'\\' || directory.back() == L'/'))
        return directory + RuntimeFileName;

    return directory + L"\\" + RuntimeFileName;
}

// First nvngx_dlssg.dll in search order outside the excluded roots (the unlocker's own runtime); empty when none.
inline std::wstring SelectGameRuntime(const std::vector<std::wstring>& searchPaths,
                                      const std::vector<std::wstring>& excludedRoots,
                                      const std::function<bool(const std::wstring&)>& fileExists)
{
    for (const auto& directory : searchPaths)
    {
        if (directory.empty())
            continue;

        bool excluded = false;
        for (const auto& root : excludedRoots)
        {
            if (IsUnder(directory, root))
            {
                excluded = true;
                break;
            }
        }

        if (excluded)
            continue;

        const auto candidate = JoinRuntimePath(directory);
        if (fileExists(candidate))
            return candidate;
    }

    return {};
}
} // namespace DlssgRuntimeGeneration

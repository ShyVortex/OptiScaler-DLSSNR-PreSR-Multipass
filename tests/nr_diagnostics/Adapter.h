// Dependency adapters for the production NGX diagnostics regression.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <nvsdk_ngx.h>
#include <filesystem>
#include <optional>
#include <vector>
#include <string>
#include <format>
#include <mutex>
#include <cstdio>
#include <algorithm>
struct version_t { unsigned major=0,minor=0,patch=0; };
inline std::mutex logMutex;
inline std::vector<std::string> diagnosticLines;
template<class... T> void TestLog(std::format_string<T...> fmt,T&&... args) {
 auto line=std::format(fmt,std::forward<T>(args)...);
 std::lock_guard lock(logMutex); diagnosticLines.push_back(line);
}
#define LOG_INFO(...) TestLog(__VA_ARGS__)
inline std::string wstring_to_string(const std::wstring& s) { return {s.begin(),s.end()}; }
struct Config { std::optional<std::wstring> MainDllPath; static Config* Instance(){static Config c;return &c;} };
struct State { std::vector<std::wstring> NVNGX_FeatureInfo_Paths; static State& Instance(){static State s;return s;} };
namespace Util {
inline std::filesystem::path ExePath(){wchar_t p[32768]{};GetModuleFileNameW(nullptr,p,32768);return p;}
inline std::filesystem::path DllPath(){return ExePath();}
inline std::wstring ToLower(std::wstring s){std::transform(s.begin(),s.end(),s.begin(),::towlower);return s;}
inline bool GetFileVersion(const std::wstring&,version_t*){return false;}
}
struct NVNGXProxy { inline static std::wstring driver; static std::wstring NVNGXModule_Path(){return driver;} };

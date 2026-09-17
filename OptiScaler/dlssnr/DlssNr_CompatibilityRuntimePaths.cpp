#include "pch.h"
#include "DlssNr_CompatibilityRuntime.h"
#include <Config.h>
#include <State.h>
#include <Util.h>
#include <proxies/NVNGX_Proxy.h>
#include <set>

namespace DlssNr
{
std::vector<std::filesystem::path> CompatibilityRuntime::CandidatePaths()
{
    std::vector<std::filesystem::path> paths;
    // 1. Explicit user config override has highest priority
    if (Config::Instance()->MainDllPath.has_value())
        paths.emplace_back(Config::Instance()->MainDllPath.value());
    // 2. OptiScaler dedicated directory (where OptiScaler DLL and components reside)
    paths.push_back(Util::DllPath().parent_path());
    // 3. Real game executable directory (where users place modded DLLs beside the exe)
    paths.push_back(Util::ExePath().parent_path());
    // 4. Game internal Streamline / feature directories as lowest-priority fallback
    for (const auto& path : State::Instance().NVNGX_FeatureInfo_Paths)
        paths.emplace_back(path);

    std::vector<std::filesystem::path> candidates;
    std::set<std::wstring> visited;
    for (const auto& dir : paths)
    {
        if (dir.empty())
            continue;
        std::error_code ec;
        auto candidate = std::filesystem::absolute(dir / L"nvngx_dlssnr.dll", ec).lexically_normal();
        if (ec)
            continue;
        if (!visited.insert(Util::ToLower(candidate.wstring())).second)
            continue;
        if (std::filesystem::exists(candidate, ec) && !std::filesystem::is_directory(candidate, ec))
            candidates.push_back(candidate);
    }
    return candidates;
}

std::shared_ptr<CompatibilityRuntime> CompatibilityRuntime::TryOpen(ID3D12Device* device)
{
    for (const auto& candidate : CandidatePaths())
    {
        if (auto runtime = Open(candidate, device,
                                NVNGXProxy::D3D12_GetCapabilityParameters(), NVNGXProxy::D3D12_DestroyParameters(),
                                State::Instance().NVNGX_ApplicationDataPath))
            return runtime;
    }
    LOG_INFO("NR compatibility: no supported direct runtime available; preserving driver failure");
    return {};
}
}

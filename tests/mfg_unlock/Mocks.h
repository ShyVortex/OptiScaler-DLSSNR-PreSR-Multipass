#pragma once
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
struct TestOption
{
    bool enabled = false;
    bool value_or_default() const { return enabled; }
};
struct Config
{
    TestOption FGDLSSGAdaMfgUnlock;
    static Config* Instance() { static Config config; return &config; }
};
enum class VendorId { Nvidia, Other };
constexpr unsigned NV_GPU_ARCHITECTURE_AD100 = 0x190;
struct TestGpu
{
    VendorId vendorId = VendorId::Nvidia;
    struct { unsigned architecture_id = NV_GPU_ARCHITECTURE_AD100; } nvidiaArchInfo;
};
namespace IdentifyGpu
{
inline TestGpu gpu;
inline const TestGpu& getPrimaryGpu() { return gpu; }
}
struct version_t { unsigned major = 0, minor = 0, patch = 0; };
namespace Util
{
inline bool GetFileVersion(const wchar_t*, version_t*, version_t*) { return false; }
}

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
#include <unordered_map>
#include <vector>

#define LOG_INFO(...) ((void) 0)
#define LOG_WARN(...) ((void) 0)

struct TestOption
{
    bool enabled = false;
    bool value_or_default() const { return enabled; }
};

struct Config
{
    TestOption FGDLSSGAdaMfgUnlock;
    TestOption FGDLSSGAdaBlackwellKernels;
    TestOption FGDLSSGAmpereMfgUnlock;

    static Config* Instance()
    {
        static Config config;
        return &config;
    }
};

struct State
{
    bool externalFrameGeneration = false;

    static State& Instance()
    {
        static State state;
        return state;
    }
};

enum class VendorId
{
    Nvidia,
    Other
};

constexpr unsigned NV_GPU_ARCHITECTURE_AD100 = 0x190;

struct TestGpu
{
    VendorId vendorId = VendorId::Nvidia;
    struct
    {
        unsigned architecture_id = NV_GPU_ARCHITECTURE_AD100;
    } nvidiaArchInfo;
};

namespace IdentifyGpu
{
inline TestGpu gpu;
inline const TestGpu& getPrimaryGpu() { return gpu; }
} // namespace IdentifyGpu

struct version_t
{
    unsigned major = 0;
    unsigned minor = 0;
    unsigned patch = 0;
};

namespace Util
{
inline bool GetFileVersion(const wchar_t*, version_t*, version_t*) { return false; }
} // namespace Util

namespace MfgTestSeams
{
inline void* failProtectAt = nullptr;
inline unsigned failProtectInvocation = 0;
inline void* failFlushAt = nullptr;
inline unsigned failFlushInvocation = 0;
inline unsigned protectInvocationsAtAddress = 0;
inline unsigned flushInvocationsAtAddress = 0;
inline std::unordered_map<HMODULE, unsigned int> moduleReferences;
inline bool failModuleReference = false;

inline void FailProtect(void* address, unsigned invocation)
{
    failProtectAt = address;
    failProtectInvocation = invocation;
    protectInvocationsAtAddress = 0;
}

inline void FailFlush(void* address, unsigned invocation = 1)
{
    failFlushAt = address;
    failFlushInvocation = invocation;
    flushInvocationsAtAddress = 0;
}

inline void RegisterModule(HMODULE module) { moduleReferences[module] = 1; }
inline void ForgetModule(HMODULE module) { moduleReferences.erase(module); }
inline bool IsModuleLoaded(HMODULE module)
{
    const auto found = moduleReferences.find(module);
    return found != moduleReferences.end() && found->second > 0;
}
} // namespace MfgTestSeams

inline BOOL WINAPI MfgTestVirtualProtect(LPVOID address, SIZE_T size, DWORD newProtect, PDWORD oldProtect)
{
    if (address == MfgTestSeams::failProtectAt)
    {
        ++MfgTestSeams::protectInvocationsAtAddress;
        if (MfgTestSeams::protectInvocationsAtAddress == MfgTestSeams::failProtectInvocation)
        {
            MfgTestSeams::failProtectAt = nullptr;
            return FALSE;
        }
    }

    return ::VirtualProtect(address, size, newProtect, oldProtect);
}

inline BOOL WINAPI MfgTestFlushInstructionCache(HANDLE process, LPCVOID address, SIZE_T size)
{
    if (address == MfgTestSeams::failFlushAt)
    {
        ++MfgTestSeams::flushInvocationsAtAddress;
        if (MfgTestSeams::flushInvocationsAtAddress == MfgTestSeams::failFlushInvocation)
        {
            MfgTestSeams::failFlushAt = nullptr;
            return FALSE;
        }
    }

    return ::FlushInstructionCache(process, address, size);
}

inline BOOL WINAPI MfgTestGetModuleHandleExW(DWORD flags, LPCWSTR address, HMODULE* module)
{
    if (MfgTestSeams::failModuleReference || module == nullptr || (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) == 0)
        return FALSE;

    const auto candidate = reinterpret_cast<HMODULE>(const_cast<wchar_t*>(address));
    const auto found = MfgTestSeams::moduleReferences.find(candidate);
    if (found == MfgTestSeams::moduleReferences.end() || found->second == 0)
        return FALSE;

    ++found->second;
    *module = candidate;
    return TRUE;
}

inline BOOL WINAPI MfgTestFreeLibrary(HMODULE module)
{
    const auto found = MfgTestSeams::moduleReferences.find(module);
    if (found == MfgTestSeams::moduleReferences.end() || found->second == 0)
        return FALSE;

    --found->second;
    return TRUE;
}

// Replacements are visible only in the CPU-test build because this file is force-included.
#define VirtualProtect MfgTestVirtualProtect
#define FlushInstructionCache MfgTestFlushInstructionCache
#define GetModuleHandleExW MfgTestGetModuleHandleExW
#define FreeLibrary MfgTestFreeLibrary

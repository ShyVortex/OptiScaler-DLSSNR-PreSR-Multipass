#include "pch.h"

#include "XeMfgLoader.h"

#include <Config.h>
#include <Logger.h>
#include <State.h>
#include <Util.h>
#include <scanner/scanner.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace
{
constexpr uint32_t kDefaultMaxFrames = 3;
constexpr uint32_t kMaxSupportedFrames = 5;

// Signature patterns for version-independent scanning
constexpr std::string_view kPatternU1 = "0F 85 CC 00 00 00 83 FB 01 0F 86 C3 00 00 00";
constexpr std::string_view kPatternU2 = "74 09 80 79 64 00 74 03 B0 01 C3 32 C0 C3";
constexpr std::string_view kPatternU3 = "BB 03 00 00 00 E8 ? ? ? ? 84 C0 74 0D 48 8B";
constexpr std::string_view kPatternU4 = "C7 87 6C 01 00 00 01 00 00 00 C6 87 68 01 00 00";
constexpr std::string_view kPatternU5 = "B8 01 00 00 00 89 47 20 33 C0 48 8B 9C 24";

// Known RVAs in libxess_fg.dll v1.3.1.78
constexpr uintptr_t kRvaU1 = 0x20da4f;
constexpr uintptr_t kRvaU2 = 0x1a5de4;
constexpr uintptr_t kRvaU3 = 0x1a517d;
constexpr uintptr_t kRvaU4 = 0x1a45c2;
constexpr uintptr_t kRvaU5 = 0x20973b;

// Pacing thunks in libxess_fg.dll v1.3.1.78
constexpr uintptr_t kRvaPacingGate = 0x224cf0;
constexpr uintptr_t kRvaPacingSched = 0x21ee30;
constexpr uintptr_t kRvaPacingDeadline = 0x224b30;

struct PatchRecord
{
    const char* name = nullptr;
    uint8_t* address = nullptr;
    std::vector<uint8_t> original;
    std::vector<uint8_t> replacement;
    DWORD originalProtection = 0;
    bool applied = false;
};

std::recursive_mutex g_mutex;
XeMfgLoader::Status g_status {};
bool g_applied = false;
std::vector<PatchRecord> g_appliedRecords {};

bool VerifyBytes(const uint8_t* address, const std::vector<uint8_t>& expected)
{
    if (!address)
        return false;
    return memcmp(address, expected.data(), expected.size()) == 0;
}

uint8_t* ResolvePatchSite(uint8_t* base, size_t imageSize, uintptr_t rva, std::string_view pattern,
                          const std::vector<uint8_t>& expectedBytes)
{
    if (!base || imageSize == 0)
        return nullptr;

    // Fast-path: Check known RVA first
    if (rva + expectedBytes.size() <= imageSize)
    {
        uint8_t* candidate = base + rva;
        if (VerifyBytes(candidate, expectedBytes))
            return candidate;
    }

    // Fallback: Pattern scanner across module
    HMODULE hModule = reinterpret_cast<HMODULE>(base);
    uintptr_t scanAddr = scanner::GetAddress(hModule, pattern);
    if (scanAddr != 0 && scanAddr >= reinterpret_cast<uintptr_t>(base) &&
        scanAddr + expectedBytes.size() <= reinterpret_cast<uintptr_t>(base) + imageSize)
    {
        uint8_t* candidate = reinterpret_cast<uint8_t*>(scanAddr);
        if (VerifyBytes(candidate, expectedBytes))
            return candidate;
    }

    return nullptr;
}

bool TransactionalWrite(std::vector<PatchRecord>& records, XeMfgLoader::Status& status)
{
    for (auto& rec : records)
    {
        DWORD oldProtect = 0;
        if (!VirtualProtect(rec.address, rec.replacement.size(), PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            LOG_ERROR("XeMFG unlock: VirtualProtect PAGE_EXECUTE_READWRITE failed for {} at {:p}", rec.name,
                      (void*) rec.address);
            status.PatchFailed = true;
            return false;
        }

        rec.originalProtection = oldProtect;
        memcpy(rec.address, rec.replacement.data(), rec.replacement.size());
        rec.applied = true;

        DWORD restoredProtect = 0;
        if (!VirtualProtect(rec.address, rec.replacement.size(), oldProtect, &restoredProtect))
        {
            LOG_WARN("XeMFG unlock: failed to restore memory protection for {} at {:p}", rec.name, (void*) rec.address);
        }

        FlushInstructionCache(GetCurrentProcess(), rec.address, rec.replacement.size());
        status.PatchesApplied++;
        LOG_INFO("XeMFG unlock: patch {} applied successfully at {:p}", rec.name, (void*) rec.address);
    }

    return true;
}

void TransactionalRollback(std::vector<PatchRecord>& records, XeMfgLoader::Status& status)
{
    for (auto& rec : records)
    {
        if (!rec.applied)
            continue;

        DWORD oldProtect = 0;
        if (!VirtualProtect(rec.address, rec.original.size(), PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            LOG_ERROR("XeMFG unlock: rollback VirtualProtect failed for {} at {:p}", rec.name, (void*) rec.address);
            status.RollbackFailed = true;
            continue;
        }

        memcpy(rec.address, rec.original.data(), rec.original.size());
        rec.applied = false;

        DWORD restoredProtect = 0;
        VirtualProtect(rec.address, rec.original.size(), rec.originalProtection ? rec.originalProtection : oldProtect,
                       &restoredProtect);
        FlushInstructionCache(GetCurrentProcess(), rec.address, rec.original.size());
        LOG_INFO("XeMFG unlock: patch {} rolled back at {:p}", rec.name, (void*) rec.address);
    }
}
} // namespace

namespace XeMfgLoader
{
Status LastStatus()
{
    std::scoped_lock lock(g_mutex);
    return g_status;
}

bool EnabledForSession()
{
    auto config = Config::Instance();
    if (!config)
        return true;
    return config->XeMfgUnlock.value_or(true);
}

bool Pending()
{
    std::scoped_lock lock(g_mutex);
    return EnabledForSession() && !g_status.Patched && !g_status.PatchFailed;
}

unsigned int UnlockedMax()
{
    std::scoped_lock lock(g_mutex);
    if (!g_status.Patched)
        return 1;
    return std::clamp(g_status.ConfiguredCeiling, 1u, kMaxSupportedFrames);
}

unsigned int EffectiveMax(unsigned int nativeMaximum)
{
    std::scoped_lock lock(g_mutex);
    if (g_status.PatchFailed || g_status.RollbackFailed)
        return 1;

    if (!g_status.Patched)
        return nativeMaximum;

    return std::max(nativeMaximum, UnlockedMax());
}

Failure LastFailure()
{
    std::scoped_lock lock(g_mutex);
    if (g_status.RollbackFailed)
        return Failure::RollbackFailed;
    if (g_status.PatchFailed)
        return Failure::PatchFailed;
    return Failure::None;
}

bool ApplyToMemory(uint8_t* baseAddress, size_t imageSize, unsigned int maxFrames, bool enablePacing, Status& outStatus)
{
    outStatus = {};
    outStatus.ModuleFound = (baseAddress != nullptr && imageSize > 0);
    outStatus.ConfiguredCeiling = std::clamp(maxFrames, 1u, kMaxSupportedFrames);

    if (!outStatus.ModuleFound)
        return false;

    // Define the 5 patch targets
    std::vector<PatchRecord> patches;

    // U1: Frame count fallback bypass (6 bytes)
    const std::vector<uint8_t> u1Expected = { 0x0f, 0x85, 0xcc, 0x00, 0x00, 0x00 };
    const std::vector<uint8_t> u1Replace = { 0xe9, 0xcd, 0x00, 0x00, 0x00, 0x90 };
    uint8_t* u1Addr = ResolvePatchSite(baseAddress, imageSize, kRvaU1, kPatternU1, u1Expected);
    if (!u1Addr)
    {
        LOG_WARN("XeMFG unlock: U1/frame-count-fallback site not matched");
        return false;
    }
    patches.push_back({ "U1/frame-count-fallback", u1Addr, u1Expected, u1Replace });

    // U2: Prevent neural model downgrade (2 bytes)
    const std::vector<uint8_t> u2Expected = { 0x74, 0x09 };
    const std::vector<uint8_t> u2Replace = { 0xeb, 0x06 };
    uint8_t* u2Addr = ResolvePatchSite(baseAddress, imageSize, kRvaU2, kPatternU2, u2Expected);
    if (!u2Addr)
    {
        LOG_WARN("XeMFG unlock: U2/model-downgrade site not matched");
        return false;
    }
    patches.push_back({ "U2/model-downgrade", u2Addr, u2Expected, u2Replace });

    // U3: Default ceiling rewrite (5 bytes)
    const uint8_t frameByte = static_cast<uint8_t>(outStatus.ConfiguredCeiling);
    const std::vector<uint8_t> u3Expected = { 0xbb, 0x03, 0x00, 0x00, 0x00 };
    const std::vector<uint8_t> u3Replace = { 0xbb, frameByte, 0x00, 0x00, 0x00 };
    uint8_t* u3Addr = ResolvePatchSite(baseAddress, imageSize, kRvaU3, kPatternU3, u3Expected);
    if (!u3Addr)
    {
        LOG_WARN("XeMFG unlock: U3/default-ceiling site not matched");
        return false;
    }
    patches.push_back({ "U3/default-ceiling", u3Addr, u3Expected, u3Replace });

    // U4: Context override clamp rewrite (10 bytes)
    const std::vector<uint8_t> u4Expected = { 0xc7, 0x87, 0x6c, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00 };
    const std::vector<uint8_t> u4Replace = { 0xc7, 0x87, 0x6c, 0x01, 0x00, 0x00, frameByte, 0x00, 0x00, 0x00 };
    uint8_t* u4Addr = ResolvePatchSite(baseAddress, imageSize, kRvaU4, kPatternU4, u4Expected);
    if (!u4Addr)
    {
        LOG_WARN("XeMFG unlock: U4/override-clamp site not matched");
        return false;
    }
    patches.push_back({ "U4/override-clamp", u4Addr, u4Expected, u4Replace });

    // U5: Reported maximum rewrite (5 bytes)
    const std::vector<uint8_t> u5Expected = { 0xb8, 0x01, 0x00, 0x00, 0x00 };
    const std::vector<uint8_t> u5Replace = { 0xb8, frameByte, 0x00, 0x00, 0x00 };
    uint8_t* u5Addr = ResolvePatchSite(baseAddress, imageSize, kRvaU5, kPatternU5, u5Expected);
    if (!u5Addr)
    {
        LOG_WARN("XeMFG unlock: U5/reported-maximum site not matched");
        return false;
    }
    patches.push_back({ "U5/reported-maximum", u5Addr, u5Expected, u5Replace });

    // Optional Pacing Gate Verification
    if (enablePacing && kRvaPacingGate + 16 <= imageSize)
    {
        const std::vector<uint8_t> pacingExpected = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c,
                                                      0x24, 0x18, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56 };
        if (VerifyBytes(baseAddress + kRvaPacingGate, pacingExpected))
        {
            outStatus.PacingInstalled = true;
            LOG_INFO("XeMFG unlock: burst frame pacing capability verified");
        }
    }

    // Apply all 5 patches atomically
    if (!TransactionalWrite(patches, outStatus))
    {
        LOG_ERROR("XeMFG unlock: transactional write failed, initiating complete rollback");
        TransactionalRollback(patches, outStatus);
        return false;
    }

    outStatus.Patched = (outStatus.PatchesApplied == 5);
    g_appliedRecords = patches;
    return outStatus.Patched;
}

void RollbackMemory(uint8_t* baseAddress, Status& outStatus)
{
    if (g_appliedRecords.empty())
        return;

    TransactionalRollback(g_appliedRecords, outStatus);
    outStatus.Patched = false;
    outStatus.PatchesApplied = 0;
    g_appliedRecords.clear();
}

void TryApply(HMODULE module)
{
    std::scoped_lock lock(g_mutex);

    if (g_applied || g_status.Patched || g_status.PatchFailed)
        return;

    if (!EnabledForSession())
    {
        LOG_DEBUG("XeMFG unlock: disabled in configuration");
        return;
    }

    if (!module)
    {
        module = GetModuleHandleW(L"libxess_fg.dll");
        if (!module)
            module = GetModuleHandleW(L"igxess_fg.dll");
    }

    if (!module)
        return;

    g_status.ModuleFound = true;

    // Get module memory bounds from PE headers
    auto dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        return;

    auto ntHeaders =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(module) + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
        return;

    size_t imageSize = ntHeaders->OptionalHeader.SizeOfImage;
    uint8_t* baseAddress = reinterpret_cast<uint8_t*>(module);

    // Retrieve file version
    std::wstring modulePath;
    modulePath.resize(MAX_PATH);
    DWORD pathLen = GetModuleFileNameW(module, modulePath.data(), MAX_PATH);
    if (pathLen > 0)
    {
        modulePath.resize(pathLen);
        version_t v {};
        if (Util::GetFileVersion(modulePath, &v))
        {
            g_status.ModuleVersion = std::format("{}.{}.{}.{}", v.major, v.minor, v.patch, v.reserved);
            LOG_INFO("XeMFG unlock: targeting provider version {}", g_status.ModuleVersion);
        }
    }

    unsigned int targetCeiling = Config::Instance()->XeMfgMaxFrames.value_or(kDefaultMaxFrames);
    bool enablePacing = Config::Instance()->XeMfgExtraPacing.value_or(true);

    bool result = ApplyToMemory(baseAddress, imageSize, targetCeiling, enablePacing, g_status);
    if (result)
    {
        g_applied = true;
        LOG_INFO("XeMFG unlock: successfully unlocked multi-frame generation up to {}X (ceiling: {})",
                 g_status.ConfiguredCeiling + 1, g_status.ConfiguredCeiling);
    }
    else
    {
        LOG_WARN("XeMFG unlock: patch application failed or signature mismatch");
    }
}

} // namespace XeMfgLoader

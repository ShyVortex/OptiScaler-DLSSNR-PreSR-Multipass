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

#include <immintrin.h>

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
constexpr uintptr_t kRvaThunkGate = 0x25c0;     // Thunk 1 -> Target 0x21f730
constexpr uintptr_t kRvaThunkSched = 0x3100;    // Thunk 2 -> Target 0x21ee30
constexpr uintptr_t kRvaThunkDeadline = 0x3430; // Thunk 3 -> Target 0x224b30

constexpr uintptr_t kRvaTargetGate = 0x21f730;
constexpr uintptr_t kRvaTargetSched = 0x21ee30;
constexpr uintptr_t kRvaTargetDeadline = 0x224b30;

// Original 16-byte thunks in libxess_fg.dll
const std::vector<uint8_t> kThunkGateExpected = { 0xe9, 0x6b, 0xd1, 0x21, 0x00, 0xcc, 0xcc, 0xcc,
                                                  0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc };
const std::vector<uint8_t> kThunkSchedExpected = { 0xe9, 0x2b, 0xbd, 0x21, 0x00, 0xcc, 0xcc, 0xcc,
                                                   0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc };
const std::vector<uint8_t> kThunkDeadlineExpected = { 0xe9, 0xfb, 0x16, 0x22, 0x00, 0xcc, 0xcc, 0xcc,
                                                      0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc };

// Target function signatures
typedef uint64_t (*PacingGateFn)(void* rcx, uint32_t edx, uint32_t r8d, void* r9, void* arg5, void* arg6, uint8_t arg7);
typedef uint64_t (*PacingSchedFn)(void* rcx, void* rdx, uint8_t r8b, void* r9, void* arg5);
typedef void* (*PacingDeadlineFn)(void* rcx, int64_t* pDeadline, void* r8, void* cycleInfo, uint32_t frameIndex,
                                  uint32_t totalFrames);

PacingGateFn g_originalGateFn = nullptr;
PacingSchedFn g_originalSchedFn = nullptr;
PacingDeadlineFn g_originalDeadlineFn = nullptr;

constexpr size_t kPacingRingSize = 15;
int64_t g_deltaRingBuffer[kPacingRingSize] = {};
size_t g_ringIndex = 0;
size_t g_ringCount = 0;
int64_t g_lastRealFrameQpc = 0;
int64_t g_qpcFrequency = 0;
int64_t g_medianDeltaNs = 0;
void* g_pacingContext = nullptr;

int64_t CalculateMedianDelta(int64_t newDeltaNs)
{
    g_deltaRingBuffer[g_ringIndex] = newDeltaNs;
    g_ringIndex = (g_ringIndex + 1) % kPacingRingSize;
    if (g_ringCount < kPacingRingSize)
        g_ringCount++;

    int64_t sorted[kPacingRingSize];
    memcpy(sorted, g_deltaRingBuffer, g_ringCount * sizeof(int64_t));
    std::sort(sorted, sorted + g_ringCount);
    return sorted[g_ringCount / 2];
}

void WaitForDeadline(int64_t targetDeadlineQpc)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    while (now.QuadPart < targetDeadlineQpc)
    {
        int64_t remaining = targetDeadlineQpc - now.QuadPart;
        int64_t threshold = (g_qpcFrequency > 0) ? (g_qpcFrequency / 500) : 200000;
        if (remaining > threshold)
            Sleep(0);
        else
            _mm_pause();
        QueryPerformanceCounter(&now);
    }
}

uint64_t PacingHookGate(void* rcx, uint32_t edx, uint32_t r8d, void* r9, void* arg5, void* arg6, uint8_t arg7)
{
    if (g_qpcFrequency == 0)
    {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        g_qpcFrequency = freq.QuadPart;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    if (rcx != nullptr)
        g_pacingContext = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(rcx) + 0x168);

    if (g_lastRealFrameQpc > 0 && g_qpcFrequency > 0)
    {
        int64_t deltaNs = (now.QuadPart - g_lastRealFrameQpc) * 1000000000LL / g_qpcFrequency;
        if (deltaNs > 0 && deltaNs < 500000000LL)
        {
            g_medianDeltaNs = CalculateMedianDelta(deltaNs);
        }
    }
    g_lastRealFrameQpc = now.QuadPart;

    if (g_originalGateFn)
        return g_originalGateFn(rcx, edx, r8d, r9, arg5, arg6, arg7);
    return 0;
}

uint64_t PacingHookSched(void* rcx, void* rdx, uint8_t r8b, void* r9, void* arg5)
{
    if (g_originalSchedFn)
        return g_originalSchedFn(rcx, rdx, r8b, r9, arg5);
    return 0;
}

void* PacingHookDeadline(void* rcx, int64_t* pDeadline, void* r8, void* cycleInfo, uint32_t frameIndex,
                         uint32_t totalFrames)
{
    void* res = nullptr;
    if (g_originalDeadlineFn)
        res = g_originalDeadlineFn(rcx, pDeadline, r8, cycleInfo, frameIndex, totalFrames);

    if (!pDeadline || !cycleInfo)
        return res;

    if (frameIndex < 1 || frameIndex > 5 || totalFrames < 2)
        return res;

    int64_t totalInterval = *reinterpret_cast<int64_t*>(reinterpret_cast<uintptr_t>(cycleInfo) + 8);
    if (totalInterval <= 0)
        return res;

    int64_t intervalPerFrame = totalInterval / totalFrames;

    int64_t adjust = 0;
    if (g_pacingContext != nullptr)
    {
        float renderEst = *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(g_pacingContext) + 0x1b8);
        renderEst = std::clamp(renderEst, 0.1f, 100.0f);
        int64_t renderTicks = static_cast<int64_t>(renderEst * 10000.0f) / totalFrames;
        if (renderTicks < intervalPerFrame)
            adjust = renderTicks;
    }

    *pDeadline = (*pDeadline) + frameIndex * (intervalPerFrame - adjust);

    if (g_lastRealFrameQpc > 0 && g_qpcFrequency > 0 && g_medianDeltaNs > 0)
    {
        int64_t targetIntervalTicks = (g_medianDeltaNs * g_qpcFrequency) / (1000000000LL * totalFrames);
        int64_t targetDeadlineQpc = g_lastRealFrameQpc + frameIndex * targetIntervalTicks;
        WaitForDeadline(targetDeadlineQpc);
    }

    return res;
}

std::vector<uint8_t> CreateJmpIndirect64(void* target)
{
    std::vector<uint8_t> bytes(16, 0xcc);
    bytes[0] = 0xff;
    bytes[1] = 0x25;
    bytes[2] = 0x00;
    bytes[3] = 0x00;
    bytes[4] = 0x00;
    bytes[5] = 0x00;
    uint64_t addr = reinterpret_cast<uint64_t>(target);
    memcpy(&bytes[6], &addr, sizeof(uint64_t));
    return bytes;
}

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
        if (strncmp(rec.name, "Pacing/", 7) == 0)
            status.PacingDetours++;
        else
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
    g_originalGateFn = nullptr;
    g_originalSchedFn = nullptr;
    g_originalDeadlineFn = nullptr;
    status.PacingDetours = 0;
    status.PacingInstalled = false;
    status.VerifiedPacing = false;
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
        return false;
    return config->XeMfgUnlock.value_or_default();
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

    // Optional Presentation Pacing Detours (Thunk 1, 2, 3)
    if (enablePacing && kRvaThunkDeadline + 16 <= imageSize)
    {
        uint8_t* thunkGateAddr = baseAddress + kRvaThunkGate;
        uint8_t* thunkSchedAddr = baseAddress + kRvaThunkSched;
        uint8_t* thunkDeadlineAddr = baseAddress + kRvaThunkDeadline;

        if (VerifyBytes(thunkGateAddr, kThunkGateExpected) && VerifyBytes(thunkSchedAddr, kThunkSchedExpected) &&
            VerifyBytes(thunkDeadlineAddr, kThunkDeadlineExpected))
        {
            g_originalGateFn = reinterpret_cast<PacingGateFn>(baseAddress + kRvaTargetGate);
            g_originalSchedFn = reinterpret_cast<PacingSchedFn>(baseAddress + kRvaTargetSched);
            g_originalDeadlineFn = reinterpret_cast<PacingDeadlineFn>(baseAddress + kRvaTargetDeadline);

            patches.push_back({ "Pacing/thunk-gate", thunkGateAddr, kThunkGateExpected,
                                CreateJmpIndirect64(reinterpret_cast<void*>(&PacingHookGate)) });
            patches.push_back({ "Pacing/thunk-sched", thunkSchedAddr, kThunkSchedExpected,
                                CreateJmpIndirect64(reinterpret_cast<void*>(&PacingHookSched)) });
            patches.push_back({ "Pacing/thunk-deadline", thunkDeadlineAddr, kThunkDeadlineExpected,
                                CreateJmpIndirect64(reinterpret_cast<void*>(&PacingHookDeadline)) });

            LOG_INFO("XeMFG unlock: presentation pacing detours prepared (Thunks 1, 2, 3)");
        }
        else
        {
            LOG_WARN("XeMFG unlock: pacing thunks byte verification mismatch, skipping pacing detours");
        }
    }

    // Apply all patches atomically
    if (!TransactionalWrite(patches, outStatus))
    {
        LOG_ERROR("XeMFG unlock: transactional write failed, initiating complete rollback");
        outStatus.ErrorMessage = "Memory protection or write failed";
        TransactionalRollback(patches, outStatus);
        if (outStatus.RollbackFailed)
            outStatus.ErrorMessage = "Rollback failed";
        return false;
    }

    outStatus.Patched = (outStatus.PatchesApplied == 5);
    outStatus.Applied = outStatus.Patched;
    if (outStatus.PacingDetours == 3)
    {
        outStatus.PacingInstalled = true;
        outStatus.VerifiedPacing = true;
    }
    g_appliedRecords = patches;
    return outStatus.Patched;
}

void RollbackMemory(uint8_t* baseAddress, Status& outStatus)
{
    if (g_appliedRecords.empty())
        return;

    TransactionalRollback(g_appliedRecords, outStatus);
    outStatus.Patched = false;
    outStatus.Applied = false;
    outStatus.PatchesApplied = 0;
    g_appliedRecords.clear();
}

void SetMaxGeneratedFrames(unsigned int maxFrames)
{
    std::scoped_lock lock(g_mutex);
    uint32_t clamped = std::clamp(maxFrames, 1u, kMaxSupportedFrames);
    g_status.ConfiguredCeiling = clamped;

    if (g_applied && !g_appliedRecords.empty())
    {
        uint8_t frameByte = static_cast<uint8_t>(clamped);
        for (auto& rec : g_appliedRecords)
        {
            if (!rec.address || !rec.applied)
                continue;

            size_t byteOffset = 0;
            if (strcmp(rec.name, "U3/default-ceiling") == 0)
                byteOffset = 1;
            else if (strcmp(rec.name, "U4/override-clamp") == 0)
                byteOffset = 6;
            else if (strcmp(rec.name, "U5/reported-maximum") == 0)
                byteOffset = 1;
            else
                continue;

            DWORD oldProtect = 0;
            if (VirtualProtect(rec.address, rec.replacement.size(), PAGE_EXECUTE_READWRITE, &oldProtect))
            {
                rec.address[byteOffset] = frameByte;
                rec.replacement[byteOffset] = frameByte;
                DWORD restoredProtect = 0;
                VirtualProtect(rec.address, rec.replacement.size(), oldProtect, &restoredProtect);
                FlushInstructionCache(GetCurrentProcess(), rec.address, rec.replacement.size());
            }
        }
    }
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

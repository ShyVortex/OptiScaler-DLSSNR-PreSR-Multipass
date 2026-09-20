// Adapted from y4my4my4m/OptiScaler_DLSSNR_Multipass_MFG, tag v4 (7b7220bb), GPL-3.0.
#include "pch.h"

#if defined(OPTISCALER_RTX40_MFG)

#include "MfgUnlock.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <misc/IdentifyGpu.h>
#include <scanner/scanner.h>

#include <mutex>

namespace
{
// mov ebx,1 / mov r8d,3 / cmp edi,0x1b0 / cmovl r8d,ebx.
constexpr std::string_view kAdvertisePattern =
    "BB 01 00 00 00 41 B8 03 00 00 00 81 FF B0 01 00 00 44 0F 4C C3";

// cmp eax,0x1b0 / jl / cmp ebx,3 / jbe.
constexpr std::string_view kValidatePattern = "3D B0 01 00 00 7C ? 83 FB 03 76";

constexpr uint8_t kMaxGeneratedFrames = 5;

// 310.9 restructured the advertise gate to a rel32 jl and the capability gate to setae.
constexpr std::string_view kAdvertisePattern309 = "81 FD B0 01 00 00 0F 8C ? ? ? ? BF 05 00 00 00";
constexpr std::string_view kValidatePattern309 = "3D B0 01 00 00 0F 93 C0";

constexpr uint32_t kArchAda = 89;
constexpr uint32_t kArchBlackwell = 120;
constexpr uint32_t kArchParked = 122;
constexpr size_t kImagePayloadSize = 8;
constexpr size_t kImageArch = 28;

struct PatternMatches
{
    uintptr_t address = 0;
    unsigned int count = 0;
};

struct Patch
{
    uint8_t* address = nullptr;
    std::vector<uint8_t> original;
    std::vector<uint8_t> replacement;
    DWORD originalProtection = 0;
    bool changed = false;
};

enum class TransactionResult
{
    Succeeded,
    FailedRolledBack,
    FailedRollbackIncomplete
};

enum class AttemptOutcome
{
    WaitingForModule,
    Succeeded,
    Unsupported,
    FailedBeforeMutation,
    FailedRolledBack,
    FailedRollbackIncomplete
};

MfgUnlock::Status g_status {};
std::recursive_mutex g_mutex;
AttemptOutcome g_attemptOutcome = AttemptOutcome::WaitingForModule;
HMODULE g_attemptedModule = nullptr;
HMODULE g_retainedModule = nullptr;

bool AcquireModuleReference(HMODULE module, HMODULE& acquired)
{
    acquired = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(module), &acquired))
        return false;

    if (acquired == module)
        return true;

    if (acquired != nullptr)
        FreeLibrary(acquired);
    acquired = nullptr;
    return false;
}

void ReleaseModuleReference(HMODULE& module)
{
    if (module == nullptr)
        return;

    if (!FreeLibrary(module))
        LOG_WARN("MFG unlock: failed to release the temporary DLSSG module reference");
    module = nullptr;
}

PatternMatches FindMatches(HMODULE module, std::string_view pattern)
{
    PatternMatches matches;
    uintptr_t start = 0;

    while (const auto address = scanner::GetAddress(module, pattern, 0, start))
    {
        if (matches.count == 0)
            matches.address = address;
        ++matches.count;
        if (matches.count > 1)
            break;
        start = address + 1;
    }

    return matches;
}

std::string ModuleVersion(HMODULE module)
{
    wchar_t path[MAX_PATH] {};

    if (GetModuleFileNameW(module, path, MAX_PATH) == 0)
        return {};

    version_t file {};
    version_t product {};

    if (!Util::GetFileVersion(path, &file, &product))
        return {};

    return std::format("{}.{}.{}", file.major, file.minor, file.patch);
}

bool AddPatch(std::vector<Patch>& plan, uint8_t* address, const uint8_t* replacement, size_t size)
{
    if (address == nullptr || replacement == nullptr || size == 0)
        return false;

    const auto begin = reinterpret_cast<uintptr_t>(address);
    const auto end = begin + size;
    if (end < begin)
        return false;

    for (const auto& patch : plan)
    {
        const auto otherBegin = reinterpret_cast<uintptr_t>(patch.address);
        const auto otherEnd = otherBegin + patch.original.size();
        if (begin < otherEnd && otherBegin < end)
            return false;
    }

    Patch patch;
    patch.address = address;
    patch.original.assign(address, address + size);
    patch.replacement.assign(replacement, replacement + size);
    plan.push_back(std::move(patch));
    return true;
}

template <size_t N> bool AddPatch(std::vector<Patch>& plan, uintptr_t address, const uint8_t (&replacement)[N])
{
    return AddPatch(plan, reinterpret_cast<uint8_t*>(address), replacement, N);
}

bool Rollback(std::vector<Patch>& plan)
{
    bool complete = true;

    for (auto it = plan.rbegin(); it != plan.rend(); ++it)
    {
        auto& patch = *it;
        if (!patch.changed)
            continue;

        DWORD currentProtection = 0;
        if (!VirtualProtect(patch.address, patch.original.size(), PAGE_EXECUTE_READWRITE, &currentProtection))
        {
            LOG_WARN("MFG unlock: rollback VirtualProtect failed at {:X}",
                     reinterpret_cast<uintptr_t>(patch.address));
            complete = false;
            continue;
        }

        std::memcpy(patch.address, patch.original.data(), patch.original.size());

        DWORD ignored = 0;
        if (!VirtualProtect(patch.address, patch.original.size(), patch.originalProtection, &ignored))
        {
            LOG_WARN("MFG unlock: rollback protection restore failed at {:X}",
                     reinterpret_cast<uintptr_t>(patch.address));
            complete = false;
        }

        if (!FlushInstructionCache(GetCurrentProcess(), patch.address, patch.original.size()))
        {
            LOG_WARN("MFG unlock: rollback cache flush failed at {:X}",
                     reinterpret_cast<uintptr_t>(patch.address));
            complete = false;
        }
    }

    return complete;
}

TransactionResult ApplyTransaction(std::vector<Patch>& plan)
{
    // Revalidate every captured byte immediately before the first write. This makes the transaction
    // reject a concurrently changed or incorrectly planned module without touching it.
    for (const auto& patch : plan)
    {
        if (std::memcmp(patch.address, patch.original.data(), patch.original.size()) != 0)
        {
            LOG_WARN("MFG unlock: planned bytes changed before apply at {:X}",
                     reinterpret_cast<uintptr_t>(patch.address));
            return TransactionResult::FailedRolledBack;
        }
    }

    for (auto& patch : plan)
    {
        if (!VirtualProtect(patch.address, patch.replacement.size(), PAGE_EXECUTE_READWRITE,
                            &patch.originalProtection))
        {
            LOG_WARN("MFG unlock: VirtualProtect failed at {:X}", reinterpret_cast<uintptr_t>(patch.address));
            return Rollback(plan) ? TransactionResult::FailedRolledBack
                                  : TransactionResult::FailedRollbackIncomplete;
        }

        std::memcpy(patch.address, patch.replacement.data(), patch.replacement.size());
        patch.changed = true;

        DWORD ignored = 0;
        if (!VirtualProtect(patch.address, patch.replacement.size(), patch.originalProtection, &ignored))
        {
            LOG_WARN("MFG unlock: protection restore failed at {:X}",
                     reinterpret_cast<uintptr_t>(patch.address));
            return Rollback(plan) ? TransactionResult::FailedRolledBack
                                  : TransactionResult::FailedRollbackIncomplete;
        }

        if (!FlushInstructionCache(GetCurrentProcess(), patch.address, patch.replacement.size()))
        {
            LOG_WARN("MFG unlock: cache flush failed at {:X}", reinterpret_cast<uintptr_t>(patch.address));
            return Rollback(plan) ? TransactionResult::FailedRolledBack
                                  : TransactionResult::FailedRollbackIncomplete;
        }
    }

    return TransactionResult::Succeeded;
}

bool BuildGatePlan(HMODULE module, std::vector<Patch>& plan)
{
    const auto advertise309 = FindMatches(module, kAdvertisePattern309);
    const auto validate309 = FindMatches(module, kValidatePattern309);
    const auto advertiseLegacy = FindMatches(module, kAdvertisePattern);
    const auto validateLegacy = FindMatches(module, kValidatePattern);

    const bool exact309 = advertise309.count == 1 && validate309.count == 1 && advertiseLegacy.count == 0 &&
                          validateLegacy.count == 0;
    const bool exactLegacy = advertiseLegacy.count == 1 && validateLegacy.count == 1 && advertise309.count == 0 &&
                             validate309.count == 0;

    if (exact309)
    {
        const uint8_t advertiseNop[] = { 0x0F, 0x1F, 0x44, 0x00, 0x00, 0x90 };
        const uint8_t validateAlways[] = { 0xB0, 0x01, 0x90 };
        return AddPatch(plan, advertise309.address + 6, advertiseNop) &&
               AddPatch(plan, validate309.address + 5, validateAlways);
    }

    if (exactLegacy)
    {
        const uint8_t count[] = { kMaxGeneratedFrames };
        const uint8_t advertiseNop[] = { 0x0F, 0x1F, 0x40, 0x00 };
        const uint8_t validateNop[] = { 0x90, 0x90 };
        return AddPatch(plan, advertiseLegacy.address + 7, count) &&
               AddPatch(plan, advertiseLegacy.address + 17, advertiseNop) &&
               AddPatch(plan, validateLegacy.address + 5, validateNop) &&
               AddPatch(plan, validateLegacy.address + 9, count);
    }

    return false;
}

// Plans every compatible fatbin rewrite without changing the module. A malformed candidate makes the
// optional kernel mode unsupported; mixing a partial rewrite with unlocked frame-count gates is unsafe.
bool BuildKernelPlan(HMODULE module, std::vector<Patch>& plan, unsigned int& containers)
{
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto* sections = IMAGE_FIRST_SECTION(nt);
    const uint8_t magic[] = { 0x50, 0xED, 0x55, 0xBA };
    containers = 0;

    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        const auto& section = sections[i];
        if (section.Characteristics & IMAGE_SCN_MEM_EXECUTE)
            continue;

        uint8_t* start = base + section.VirtualAddress;
        uint8_t* end = start + section.Misc.VirtualSize;
        for (uint8_t* cursor = start; cursor < end;)
        {
            uint8_t* container = std::search(cursor, end, magic, magic + sizeof(magic));
            if (container == end)
                break;
            cursor = container + 1;

            if (end - container < 16)
                return false;

            const auto headerSize = *reinterpret_cast<const uint16_t*>(container + 6);
            const auto fatSize = *reinterpret_cast<const uint64_t*>(container + 8);
            if (headerSize != 0x10 || fatSize == 0 || fatSize > static_cast<uint64_t>(end - container - 16))
                return false;

            uint8_t* const containerEnd = container + 16 + fatSize;
            uint8_t* blackwell = nullptr;
            size_t blackwellHeader = 0;
            size_t blackwellPayload = 0;
            std::vector<uint8_t*> adaImages;

            for (uint8_t* image = container + 16; image < containerEnd;)
            {
                const auto remaining = static_cast<uint64_t>(containerEnd - image);
                if (remaining < kImageArch + sizeof(uint32_t))
                    return false;

                const auto kind = *reinterpret_cast<const uint16_t*>(image);
                const auto imageHeader = *reinterpret_cast<const uint32_t*>(image + 4);
                const auto payload = *reinterpret_cast<const uint64_t*>(image + kImagePayloadSize);
                const auto arch = *reinterpret_cast<const uint32_t*>(image + kImageArch);
                if (imageHeader < kImageArch + sizeof(uint32_t) || imageHeader > remaining || payload == 0 ||
                    payload > remaining - imageHeader)
                    return false;

                if (kind == 1 && arch == kArchBlackwell)
                {
                    if (blackwell != nullptr)
                        return false;
                    blackwell = image;
                    blackwellHeader = imageHeader;
                    blackwellPayload = static_cast<size_t>(payload);
                }
                else if (arch == kArchAda)
                    adaImages.push_back(image);

                image += imageHeader + payload;
            }

            if (blackwell == nullptr || adaImages.empty())
                continue;

            const char from[] = ".target sm_120";
            const char to[] = ".target sm_89 ";
            static_assert(sizeof(from) == sizeof(to), "the directive rewrite must not change length");

            uint8_t* body = blackwell + blackwellHeader;
            uint8_t* bodyEnd = body + blackwellPayload;
            auto target = std::search(body, bodyEnd, from, from + sizeof(from) - 1);
            if (target == bodyEnd || std::search(target + 1, bodyEnd, from, from + sizeof(from) - 1) != bodyEnd)
                return false;

            std::vector<uint8_t> patched(container, containerEnd);
            std::memcpy(patched.data() + (target - container), to, sizeof(to) - 1);
            std::memcpy(patched.data() + (blackwell + kImageArch - container), &kArchAda, sizeof(kArchAda));
            for (auto* image : adaImages)
                std::memcpy(patched.data() + (image + kImageArch - container), &kArchParked, sizeof(kArchParked));

            if (!AddPatch(plan, container, patched.data(), patched.size()))
                return false;
            ++containers;
            cursor = containerEnd;
        }
    }

    return containers > 0;
}
} // namespace

bool MfgUnlock::PatchArchGates(HMODULE module)
{
    if (module == nullptr)
        return false;

    std::lock_guard lock(g_mutex);
    if (g_retainedModule != nullptr)
        return false;

    HMODULE acquired = nullptr;
    if (!AcquireModuleReference(module, acquired))
        return false;

    std::vector<Patch> plan;
    if (!BuildGatePlan(module, plan))
    {
        ReleaseModuleReference(acquired);
        return false;
    }

    const auto result = ApplyTransaction(plan);
    if (result == TransactionResult::Succeeded || result == TransactionResult::FailedRollbackIncomplete)
        g_retainedModule = acquired;
    else
        ReleaseModuleReference(acquired);
    return result == TransactionResult::Succeeded;
}

void MfgUnlock::TryApply(HMODULE requestedModule)
{
    if (!EnabledForSession())
        return;

    const auto& gpu = IdentifyGpu::getPrimaryGpu();
    if (gpu.vendorId != VendorId::Nvidia || gpu.nvidiaArchInfo.architecture_id != NV_GPU_ARCHITECTURE_AD100)
        return;

    std::lock_guard lock(g_mutex);
    if (g_attemptOutcome != AttemptOutcome::WaitingForModule)
    {
        if (requestedModule != nullptr && requestedModule != g_attemptedModule)
            LOG_WARN("MFG unlock: ignoring a different DLSSG module after the terminal first attempt");
        return;
    }

    auto module = requestedModule ? requestedModule : GetModuleHandleW(L"nvngx_dlssg.dll");
    if (module == nullptr)
        return;

    g_attemptedModule = module;
    g_status.ModuleFound = true;
    HMODULE acquired = nullptr;
    if (!AcquireModuleReference(module, acquired))
    {
        g_status.PatchFailed = true;
        g_attemptOutcome = AttemptOutcome::FailedBeforeMutation;
        LOG_WARN("MFG unlock: could not retain the DLSSG module; patch attempt rejected");
        return;
    }

    g_status.SnippetVersion = ModuleVersion(module);

    std::vector<Patch> gatePlan;
    if (!BuildGatePlan(module, gatePlan))
    {
        ReleaseModuleReference(acquired);
        g_attemptOutcome = AttemptOutcome::Unsupported;
        LOG_WARN("MFG unlock: unsupported or ambiguous DLSSG {} signatures; left unchanged",
                 g_status.SnippetVersion);
        return;
    }

    std::vector<Patch> plan;
    unsigned int kernelContainers = 0;
    if (Config::Instance()->FGDLSSGAdaBlackwellKernels.value_or_default() &&
        !BuildKernelPlan(module, plan, kernelContainers))
    {
        ReleaseModuleReference(acquired);
        g_attemptOutcome = AttemptOutcome::Unsupported;
        LOG_WARN("MFG unlock: no complete, unambiguous interpolation kernel set; module left unchanged");
        return;
    }

    // Kernel data goes first so a later gate failure proves that the complete optional rewrite is
    // rolled back with the gates. The entire plan was validated before this point.
    plan.insert(plan.end(), std::make_move_iterator(gatePlan.begin()), std::make_move_iterator(gatePlan.end()));
    const auto result = ApplyTransaction(plan);
    if (result != TransactionResult::Succeeded)
    {
        g_status.PatchFailed = true;
        g_status.RollbackFailed = result == TransactionResult::FailedRollbackIncomplete;
        g_attemptOutcome = g_status.RollbackFailed ? AttemptOutcome::FailedRollbackIncomplete
                                                   : AttemptOutcome::FailedRolledBack;
        if (g_status.RollbackFailed)
            g_retainedModule = acquired;
        else
            ReleaseModuleReference(acquired);
        LOG_WARN("MFG unlock: patch transaction failed; rollback {}",
                 g_status.RollbackFailed ? "was incomplete" : "completed");
        return;
    }

    g_status.AdvertiseMatched = true;
    g_status.ValidateMatched = true;
    g_status.ArchGatesPatched = true;
    g_status.KernelsRewritten = kernelContainers;
    g_retainedModule = acquired;
    g_attemptOutcome = AttemptOutcome::Succeeded;
    LOG_INFO("MFG unlock: nvngx_dlssg.dll patched for {} generated frames (kernels rewritten: {})",
             kMaxGeneratedFrames, kernelContainers);
}

unsigned int MfgUnlock::UnlockedMax()
{
    const auto status = LastStatus();
    return status.AdvertiseMatched && status.ValidateMatched && !status.PatchFailed ? kMaxGeneratedFrames : 0;
}

unsigned int MfgUnlock::EffectiveMax(unsigned int nativeMaximum)
{
    std::lock_guard lock(g_mutex);
    if (g_status.PatchFailed)
        return 1;

    const unsigned int verified = g_status.AdvertiseMatched && g_status.ValidateMatched
                                      ? kMaxGeneratedFrames
                                      : 0;
    return std::max(nativeMaximum, verified);
}

MfgUnlock::Failure MfgUnlock::LastFailure()
{
    std::lock_guard lock(g_mutex);
    if (g_status.RollbackFailed)
        return Failure::RollbackFailed;
    if (g_status.PatchFailed)
        return Failure::PatchFailed;
    return Failure::None;
}

bool MfgUnlock::Pending()
{
    if (!EnabledForSession())
        return false;

    const auto& gpu = IdentifyGpu::getPrimaryGpu();
    if (gpu.vendorId != VendorId::Nvidia || gpu.nvidiaArchInfo.architecture_id != NV_GPU_ARCHITECTURE_AD100)
        return false;

    std::lock_guard lock(g_mutex);
    return g_attemptOutcome == AttemptOutcome::WaitingForModule;
}

MfgUnlock::Status MfgUnlock::LastStatus()
{
    std::lock_guard lock(g_mutex);
    return g_status;
}

bool MfgUnlock::EnabledForSession()
{
    // Latch before the first FG load/query. UI changes take effect on the next launch.
    // Mutual exclusion: Ada unlock is disabled if Ampere unlock or external FG is enabled.
    static const bool enabled = Config::Instance()->FGDLSSGAdaMfgUnlock.value_or_default() &&
                                !Config::Instance()->FGDLSSGAmpereMfgUnlock.value_or_default() &&
                                !State::Instance().externalFrameGeneration;
    return enabled;
}

#endif

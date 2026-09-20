#include "Mocks.h"
#include "../../OptiScaler/framegen/dlssg/MfgUnlock.h"

#include <stdexcept>

namespace
{
constexpr size_t kImageSize = 0x6000;
constexpr size_t kAdvertise = 0x1100;
constexpr size_t kValidate = 0x1200;
constexpr size_t kDuplicate = 0x1300;
constexpr size_t kContainer = 0x3000;

constexpr std::string_view kLegacyAdvertise = "BB 01 00 00 00 41 B8 03 00 00 00 81 FF B0 01 00 00 44 0F 4C C3";
constexpr std::string_view kLegacyValidate = "3D B0 01 00 00 7C 08 83 FB 03 76";
constexpr std::string_view k309Advertise = "81 FD B0 01 00 00 0F 8C 20 00 00 00 BF 05 00 00 00";
constexpr std::string_view k309Validate = "3D B0 01 00 00 0F 93 C0";

void Expect(bool value, const char* why)
{
    if (!value)
        throw std::runtime_error(why);
}

template <class T> void Put(uint8_t* at, T value) { std::memcpy(at, &value, sizeof(value)); }
template <class T> T Read(const uint8_t* at)
{
    T value;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

void PutPattern(uint8_t* to, std::string_view pattern)
{
    size_t offset = 0;
    for (size_t i = 0; i < pattern.size();)
    {
        if (pattern[i] == ' ')
        {
            ++i;
            continue;
        }

        to[offset++] = static_cast<uint8_t>(std::stoul(std::string(pattern.substr(i, 2)), nullptr, 16));
        i += 2;
    }
}

struct PeImage
{
    uint8_t* bytes =
        static_cast<uint8_t*>(::VirtualAlloc(nullptr, kImageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));

    PeImage()
    {
        Expect(bytes != nullptr, "VirtualAlloc failed");
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(bytes);
        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = 0x100;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(bytes + 0x100);
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.NumberOfSections = 2;
        nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        nt->OptionalHeader.SizeOfImage = kImageSize;
        auto* sections = IMAGE_FIRST_SECTION(nt);
        sections[0].VirtualAddress = 0x1000;
        sections[0].Misc.VirtualSize = 0x1000;
        sections[0].Characteristics = IMAGE_SCN_MEM_EXECUTE;
        sections[1].VirtualAddress = 0x3000;
        sections[1].Misc.VirtualSize = 0x1000;
        MfgTestSeams::RegisterModule(Module());
    }

    ~PeImage()
    {
        MfgTestSeams::ForgetModule(Module());
        ::VirtualFree(bytes, 0, MEM_RELEASE);
    }
    HMODULE Module() const { return reinterpret_cast<HMODULE>(bytes); }
    std::vector<uint8_t> Snapshot() const { return { bytes, bytes + kImageSize }; }

    void AddLegacy()
    {
        PutPattern(bytes + kAdvertise, kLegacyAdvertise);
        PutPattern(bytes + kValidate, kLegacyValidate);
    }

    void Add309()
    {
        PutPattern(bytes + kAdvertise, k309Advertise);
        PutPattern(bytes + kValidate, k309Validate);
    }

    void AddKernel(bool malformed = false)
    {
        auto* container = bytes + kContainer;
        Put<uint32_t>(container, 0xba55ed50);
        Put<uint16_t>(container + 6, 16);
        Put<uint64_t>(container + 8, 128);
        auto* ada = container + 16;
        auto* blackwell = ada + 64;
        Put<uint16_t>(ada, 2);
        Put<uint32_t>(ada + 4, 32);
        Put<uint64_t>(ada + 8, 32);
        Put<uint32_t>(ada + 28, 89);
        Put<uint16_t>(blackwell, 1);
        Put<uint32_t>(blackwell + 4, 32);
        Put<uint64_t>(blackwell + 8, malformed ? UINT64_MAX : 32);
        Put<uint32_t>(blackwell + 28, 120);
        std::memcpy(blackwell + 32, ".target sm_120", 14);
    }
};

void ExpectUnchanged(const PeImage& image, const std::vector<uint8_t>& before, const char* why)
{
    Expect(std::memcmp(image.bytes, before.data(), before.size()) == 0, why);
    Expect(MfgUnlock::UnlockedMax() == 0, "failed/unsupported patch advertised an unlocked maximum");
}

void ConfigureEnabled()
{
    Config::Instance()->FGDLSSGAdaMfgUnlock.enabled = true;
    IdentifyGpu::gpu.vendorId = VendorId::Nvidia;
    IdentifyGpu::gpu.nvidiaArchInfo.architecture_id = NV_GPU_ARCHITECTURE_AD100;
}

void VerifyLegacyGates(const PeImage& image)
{
    Expect(image.bytes[kAdvertise + 7] == 5, "legacy advertise count was not raised");
    const uint8_t advertiseNop[] = { 0x0f, 0x1f, 0x40, 0x00 };
    Expect(std::memcmp(image.bytes + kAdvertise + 17, advertiseNop, sizeof(advertiseNop)) == 0,
           "legacy advertise clamp was not neutralised");
    const uint8_t validateNop[] = { 0x90, 0x90 };
    Expect(std::memcmp(image.bytes + kValidate + 5, validateNop, sizeof(validateNop)) == 0,
           "legacy validation branch was not neutralised");
    Expect(image.bytes[kValidate + 9] == 5, "legacy validation count was not raised");
}

void VerifyKernelRetarget(const PeImage& image)
{
    const auto* ada = image.bytes + kContainer + 16;
    const auto* blackwell = ada + 64;
    Expect(Read<uint32_t>(ada + 28) == 122, "Ada kernel image was not parked");
    Expect(Read<uint32_t>(blackwell + 28) == 89, "Blackwell PTX did not answer for Ada");
    Expect(std::memcmp(blackwell + 32, ".target sm_89 ", 14) == 0, "PTX target was not retargeted");
}
} // namespace

int main(int argc, char** argv) try
{
    Expect(argc == 2, "pass exactly one CPU test case");
    const std::string mode = argv[1];
    ConfigureEnabled();

    if (mode == "disabled" || mode == "blackwell" || mode == "ampere" || mode == "other-vendor" ||
        mode == "ampere-option" || mode == "external")
    {
        if (mode == "disabled")
            Config::Instance()->FGDLSSGAdaMfgUnlock.enabled = false;
        else if (mode == "blackwell")
            IdentifyGpu::gpu.nvidiaArchInfo.architecture_id = 0x1b0;
        else if (mode == "ampere")
            IdentifyGpu::gpu.nvidiaArchInfo.architecture_id = 0x170;
        else if (mode == "other-vendor")
            IdentifyGpu::gpu.vendorId = VendorId::Other;
        else if (mode == "ampere-option")
            Config::Instance()->FGDLSSGAmpereMfgUnlock.enabled = true;
        else
            State::Instance().externalFrameGeneration = true;

        PeImage image;
        image.AddLegacy();
        const auto before = image.Snapshot();
        MfgUnlock::TryApply(image.Module());
        ExpectUnchanged(image, before, "excluded session modified the module");
        Expect(!MfgUnlock::Pending(), "excluded session reported a pending patch");
        Expect(MfgUnlock::EffectiveMax(8) == 8, "disabled/excluded session changed a native maximum");
        Expect(MfgUnlock::LastFailure() == MfgUnlock::Failure::None, "excluded session fabricated a failure");
    }
    else if (mode == "missing-gate" || mode == "duplicate-gate" || mode == "mixed-families")
    {
        PeImage image;
        PutPattern(image.bytes + kAdvertise, kLegacyAdvertise);
        PutPattern(image.bytes + kValidate, kLegacyValidate);
        if (mode == "missing-gate")
            std::memset(image.bytes + kValidate, 0, 32);
        else if (mode == "duplicate-gate")
            PutPattern(image.bytes + kDuplicate, kLegacyAdvertise);
        else
        {
            PutPattern(image.bytes + kDuplicate, k309Advertise);
            PutPattern(image.bytes + 0x1400, k309Validate);
        }

        const auto before = image.Snapshot();
        MfgUnlock::TryApply(image.Module());
        ExpectUnchanged(image, before, "ambiguous/incomplete signatures modified the module");
        Expect(MfgUnlock::LastStatus().ModuleFound, "present unsupported module was not recorded");
        Expect(!MfgUnlock::Pending(), "terminal unsupported result remained pending");
        Expect(MfgTestFreeLibrary(image.Module()), "owner could not release unsupported module");
        Expect(!MfgTestSeams::IsModuleLoaded(image.Module()), "unsupported module reference leaked");
    }
    else if (mode == "unknown" || mode == "direct-unknown")
    {
        PeImage image;
        const uint8_t cmpEax[] = { 0x3d, 0xb0, 0x01, 0x00, 0x00, 0x75, 0x04 };
        const uint8_t cmpEdi[] = { 0x81, 0xff, 0xb0, 0x01, 0x00, 0x00, 0x90 };
        std::memcpy(image.bytes + kAdvertise, cmpEax, sizeof(cmpEax));
        std::memcpy(image.bytes + kValidate, cmpEdi, sizeof(cmpEdi));
        const auto before = image.Snapshot();
        if (mode == "direct-unknown")
            Expect(!MfgUnlock::PatchArchGates(image.Module()), "broad public patcher accepted unknown code");
        else
            MfgUnlock::TryApply(image.Module());
        ExpectUnchanged(image, before, "unknown architecture comparisons were patched");
    }
    else if (mode == "legacy" || mode == "legacy-kernel-default-off")
    {
        PeImage image;
        image.AddLegacy();
        image.AddKernel();
        const auto kernelBefore = std::vector<uint8_t>(image.bytes + kContainer, image.bytes + kContainer + 144);
        MfgUnlock::TryApply(image.Module());
        VerifyLegacyGates(image);
        Expect(MfgUnlock::UnlockedMax() == 5, "legacy patch did not publish the unlocked maximum");
        Expect(MfgUnlock::EffectiveMax(1) == 5, "verified patch did not raise the effective maximum");
        Expect(MfgUnlock::EffectiveMax(8) == 8, "verified patch lowered a larger native maximum");
        Expect(MfgUnlock::LastFailure() == MfgUnlock::Failure::None, "successful patch reported a failure");
        Expect(MfgUnlock::LastStatus().KernelsRewritten == 0, "default unexpectedly retargeted kernels");
        Expect(std::memcmp(image.bytes + kContainer, kernelBefore.data(), kernelBefore.size()) == 0,
               "default changed Blackwell kernel routing");
    }
    else if (mode == "3109")
    {
        PeImage image;
        image.Add309();
        MfgUnlock::TryApply(image.Module());
        const uint8_t advertiseNop[] = { 0x0f, 0x1f, 0x44, 0x00, 0x00, 0x90 };
        Expect(std::memcmp(image.bytes + kAdvertise + 6, advertiseNop, sizeof(advertiseNop)) == 0,
               "310.9 advertise branch was not neutralised");
        const uint8_t validateAlways[] = { 0xb0, 0x01, 0x90 };
        Expect(std::memcmp(image.bytes + kValidate + 5, validateAlways, sizeof(validateAlways)) == 0,
               "310.9 capability flag was not forced");
        Expect(MfgUnlock::UnlockedMax() == 5, "310.9 patch did not publish the unlocked maximum");
    }
    else if (mode == "kernel-enabled")
    {
        Config::Instance()->FGDLSSGAdaBlackwellKernels.enabled = true;
        PeImage image;
        image.AddLegacy();
        image.AddKernel();
        MfgUnlock::TryApply(image.Module());
        VerifyLegacyGates(image);
        VerifyKernelRetarget(image);
        Expect(MfgUnlock::LastStatus().KernelsRewritten == 1, "successful kernel rewrite count was wrong");
        Expect(MfgUnlock::UnlockedMax() == 5, "kernel-enabled transaction did not publish success");
    }
    else if (mode == "kernel-missing" || mode == "kernel-malformed")
    {
        Config::Instance()->FGDLSSGAdaBlackwellKernels.enabled = true;
        PeImage image;
        image.AddLegacy();
        if (mode == "kernel-malformed")
            image.AddKernel(true);
        const auto before = image.Snapshot();
        MfgUnlock::TryApply(image.Module());
        ExpectUnchanged(image, before, "missing/malformed required kernels allowed a partial patch");
    }
    else if (mode == "protect-failure" || mode == "restore-failure" || mode == "flush-failure" ||
             mode == "kernel-transaction-failure" || mode == "rollback-failure")
    {
        PeImage image;
        image.AddLegacy();
        if (mode == "kernel-transaction-failure")
        {
            Config::Instance()->FGDLSSGAdaBlackwellKernels.enabled = true;
            image.AddKernel();
            MfgTestSeams::FailProtect(image.bytes + kValidate + 5, 1);
        }
        else if (mode == "protect-failure")
            MfgTestSeams::FailProtect(image.bytes + kValidate + 5, 1);
        else if (mode == "restore-failure")
            MfgTestSeams::FailProtect(image.bytes + kAdvertise + 7, 2);
        else if (mode == "rollback-failure")
        {
            MfgTestSeams::FailProtect(image.bytes + kValidate + 5, 1);
            MfgTestSeams::FailFlush(image.bytes + kAdvertise + 17, 2);
        }
        else
            MfgTestSeams::FailFlush(image.bytes + kAdvertise + 7);

        const auto before = image.Snapshot();
        MfgUnlock::TryApply(image.Module());
        ExpectUnchanged(image, before, "failed write was not rolled back transactionally");
        const auto status = MfgUnlock::LastStatus();
        Expect(!status.AdvertiseMatched && !status.ValidateMatched,
               "failed transaction reported successfully patched gates");
        Expect(status.PatchFailed, "write failure was not persisted in status");
        Expect(status.RollbackFailed == (mode == "rollback-failure"), "rollback outcome was not persisted precisely");
        Expect(MfgUnlock::EffectiveMax(5) == 1, "failed patch trusted a possibly partial native maximum");
        const auto expectedFailure =
            mode == "rollback-failure" ? MfgUnlock::Failure::RollbackFailed : MfgUnlock::Failure::PatchFailed;
        Expect(MfgUnlock::LastFailure() == expectedFailure, "cheap failure state lost severity");
        Expect(MfgTestFreeLibrary(image.Module()), "owner could not release failed module");
        Expect(MfgTestSeams::IsModuleLoaded(image.Module()) == (mode == "rollback-failure"),
               "module retention did not match rollback completeness");
    }
    else if (mode == "reference-failure")
    {
        PeImage image;
        image.AddLegacy();
        const auto before = image.Snapshot();
        MfgTestSeams::failModuleReference = true;
        MfgUnlock::TryApply(image.Module());
        ExpectUnchanged(image, before, "reference acquisition failure modified the module");
        const auto status = MfgUnlock::LastStatus();
        Expect(status.ModuleFound && status.PatchFailed && !status.RollbackFailed,
               "reference acquisition failure status was not fail-closed");
        Expect(MfgUnlock::EffectiveMax(5) == 1, "reference acquisition failure trusted native capability");
        Expect(MfgUnlock::LastFailure() == MfgUnlock::Failure::PatchFailed,
               "reference acquisition failure was not exposed cheaply");
        Expect(MfgTestFreeLibrary(image.Module()), "owner could not release reference-failed module");
        Expect(!MfgTestSeams::IsModuleLoaded(image.Module()), "failed acquisition fabricated a held reference");
    }
    else if (mode == "success-retains-module")
    {
        PeImage image;
        image.AddLegacy();
        MfgUnlock::TryApply(image.Module());
        Expect(MfgTestFreeLibrary(image.Module()), "owner could not release successfully patched module");
        Expect(MfgTestSeams::IsModuleLoaded(image.Module()), "successful patch did not retain its module");
        Expect(MfgUnlock::UnlockedMax() == 5, "retained patched module lost verified capability");
    }
    else if (mode == "terminal-no-retry")
    {
        PeImage unsupported;
        PeImage supported;
        supported.AddLegacy();
        const auto before = supported.Snapshot();
        MfgUnlock::TryApply(unsupported.Module());
        MfgUnlock::TryApply(supported.Module());
        ExpectUnchanged(supported, before, "terminal unsupported outcome retried a different module per frame");
    }
    else
        throw std::runtime_error("unknown CPU test case");

    std::cout << "PASS " << mode << '\n';
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << e.what() << '\n';
    return 1;
}

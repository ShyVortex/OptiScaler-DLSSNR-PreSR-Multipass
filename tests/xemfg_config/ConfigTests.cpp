// CPU-only tests compile the real loader, pacer and scanner. Only OS failure
// boundaries and runtime version/configuration dependencies are substituted.
#include "Mocks.h"
#include "../../OptiScaler/framegen/xefg/XeMfgLoader.cpp"
#include "../../OptiScaler/scanner/scanner.cpp"

static unsigned failures = 0;
static void Expect(bool condition, const char* message)
{
    if (!condition)
    {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}
struct Image
{
    uint8_t* bytes =
        static_cast<uint8_t*>(VirtualAlloc(nullptr, 22990848, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    Image()
    {
        if (!bytes)
            std::terminate();
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(bytes);
        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = 0x80;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(bytes + 0x80);
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        nt->FileHeader.TimeDateStamp = 1774915405;
        nt->FileHeader.NumberOfSections = 1;
        nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt->OptionalHeader.SizeOfImage = 22990848;
        auto* section = IMAGE_FIRST_SECTION(nt);
        section->VirtualAddress = 0x2000;
        section->Misc.VirtualSize = 0x22E000;
        section->Characteristics = IMAGE_SCN_MEM_EXECUTE;
        Set(0x20da4f, { 0x0f, 0x85, 0xcc, 0, 0, 0, 0x83, 0xfb, 1, 0x0f, 0x86, 0xc3, 0, 0, 0 });
        Set(0x1a5de4, { 0x74, 9, 0x80, 0x79, 0x64, 0, 0x74, 3, 0xb0, 1, 0xc3, 0x32, 0xc0, 0xc3 });
        Set(0x1a517d, { 0xbb, 3, 0, 0, 0, 0xe8, 0, 0, 0, 0, 0x84, 0xc0, 0x74, 0x0d, 0x48, 0x8b });
        Set(0x1a45c2, { 0xc7, 0x87, 0x6c, 1, 0, 0, 1, 0, 0, 0, 0xc6, 0x87, 0x68, 1, 0, 0 });
        Set(0x20973b, { 0xb8, 1, 0, 0, 0, 0x89, 0x47, 0x20, 0x33, 0xc0, 0x48, 0x8b, 0x9c, 0x24 });
        memcpy(bytes + 0x25c0, XeFGPacing::PresentThunkExpected, 16);
        memcpy(bytes + 0x3100, XeFGPacing::SchedThunkExpected, 16);
        memcpy(bytes + 0x3430, XeFGPacing::TimestampThunkExpected, 16);
    }
    void Set(size_t offset, std::initializer_list<uint8_t> values)
    {
        std::copy(values.begin(), values.end(), bytes + offset);
    }
    HMODULE Module() const { return reinterpret_cast<HMODULE>(bytes); }
    ~Image() { VirtualFree(bytes, 0, MEM_RELEASE); }
};

static bool GuardedScan(uint8_t* data)
{
    __try
    {
        return FindPattern(reinterpret_cast<uintptr_t>(data), 1, "74 09") == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    const std::string_view name = argv[1];
    Image image;
    auto* config = Config::Instance();
    config->XeMfgUnlock = true;
    if (name == "menu-recovery")
    {
        for (const bool xeUnlock : { false, true })
        {
            const bool adaActive = true, ampereActive = false, externalActive = false;
#include "production-menu-gate.inc"
            Expect(disableXeMfg == !xeUnlock,
                   "conflict must prevent enabling Xe but permit unchecking an already saved selection");
        }
    }
    else if (name == "scanner-end")
    {
        auto* data = static_cast<uint8_t*>(VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        DWORD previous = 0;
        ::VirtualProtect(data + 4096, 4096, PAGE_NOACCESS, &previous);
        data[4095] = 0x74;
        Expect(GuardedScan(data + 4095), "scanner must not read the exclusive end guard page");
        VirtualFree(data, 0, MEM_RELEASE);
    }
    else if (name == "default-off" || name == "saved-off" || name == "explicit-on")
    {
        if (name == "saved-off")
        {
            CustomOptional<bool> old(false);
            old = false;
            config->XeMfgExtraPacing.set_from_config(old.value_for_config());
        }
        if (name == "explicit-on")
            config->XeMfgExtraPacing = true;
        XeMfgLoader::TryApply(image.Module());
        Expect(XeMfgLoader::LastStatus().Patched, "known runtime must unlock");
        Expect(XeFGPacing::InstalledDetourCount() == (name == "explicit-on" ? 3u : 0u),
               "auto/saved OFF must not install a pacer; explicit ON must install it");
    }
    else if (name == "external-conflict" || name == "ada-conflict" || name == "ampere-conflict")
    {
        if (name == "external-conflict")
            config->ExternalFrameGeneration = true;
        else if (name == "ada-conflict")
            config->FGDLSSGAdaMfgUnlock = true;
        else
            config->FGDLSSGAmpereMfgUnlock = true;
        Expect(!XeMfgLoader::EnabledForSession(), "conflicting provider flags must reject Xe session admission");
        XeMfgLoader::TryApply(image.Module());
        Expect(!XeMfgLoader::LastStatus().Patched && image.bytes[0x20da4f] == 0x0f,
               "rejected provider must not patch Intel code");
        Expect(config->XeMfgUnlock.value_for_config() == true, "admission must preserve saved user intent");
    }
    else if (name == "ceiling-live")
    {
        config->XeMfgExtraPacing = false;
        XeMfgLoader::TryApply(image.Module());
        XeMfgLoader::SetMaxGeneratedFrames(5);
        Expect(XeMfgLoader::EffectiveMax(1) == 3 && image.bytes[0x1a517e] == 3,
               "live ceiling change must not publish or rewrite beyond the initialized session");
    }
    else
        return 2;
    std::printf("%s: %u failures; CPU-only memory fixture\n", argv[1], failures);
    return failures ? 1 : 0;
}

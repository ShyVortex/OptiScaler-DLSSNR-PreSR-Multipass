// Production patcher/scanner against controlled PE images; no NVIDIA code executes.
#include "Mocks.h"
#include "../../OptiScaler/framegen/dlssg/MfgUnlock.cpp"
#include "../../OptiScaler/scanner/scanner.cpp"
#include <stdexcept>

void Expect(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
template<class T> void Put(uint8_t* at, T value) { std::memcpy(at, &value, sizeof(value)); }
template<class T> T Read(uint8_t* at) { T value; std::memcpy(&value, at, sizeof(value)); return value; }
void Pattern(uint8_t* to, std::string_view pattern)
{
    unsigned offset = 0;
    for (size_t i = 0; i < pattern.size();)
    {
        if (pattern[i] == ' ') { ++i; continue; }
        if (pattern[i] == '?') { to[offset++] = 0; ++i; continue; }
        to[offset++] = static_cast<uint8_t>(std::stoul(std::string(pattern.substr(i, 2)), nullptr, 16));
        i += 2;
    }
}
int main(int argc, char** argv) try
{
    Expect(argc == 2 || argc == 3, "Pass a test case or runtime and DLL path");
    std::string mode = argv[1];
    Config::Instance()->FGDLSSGAdaMfgUnlock.enabled = mode != "disabled";
    if (mode == "runtime")
    {
        Expect(argc == 3, "Pass an installed DLSSG DLL path");
        // Map its image without imports/DllMain; never initialize FG or change the disk file.
        auto module = LoadLibraryExA(argv[2], nullptr, DONT_RESOLVE_DLL_REFERENCES);
        Expect(module != nullptr, "Map installed runtime");
        MfgUnlock::TryApply(module);
        const auto status = MfgUnlock::LastStatus();
        std::cout << "Runtime gates " << status.AdvertiseMatched << '/' << status.ValidateMatched
                  << ", kernel groups " << status.KernelsRewritten << '\n';
        Expect(MfgUnlock::UnlockedMax() == 5, "Installed runtime is not supported by this patch");
        FreeLibrary(module);
        std::cout << "PASS runtime image patch (simulated Ada; no GPU execution)\n";
        return 0;
    }
    if (mode == "blackwell") IdentifyGpu::gpu.nvidiaArchInfo.architecture_id = 0x1b0;
    if (mode == "ampere") IdentifyGpu::gpu.nvidiaArchInfo.architecture_id = 0x170;
    if (mode == "other-vendor") IdentifyGpu::gpu.vendorId = VendorId::Other;
    if (mode == "restart")
    {
        Config::Instance()->FGDLSSGAdaMfgUnlock.enabled = false;
        Expect(!MfgUnlock::EnabledForSession(), "Default must be inactive");
        Config::Instance()->FGDLSSGAdaMfgUnlock.enabled = true;
        Expect(!MfgUnlock::EnabledForSession() && !MfgUnlock::Pending(), "UI enabled a live patch without restart");
        std::cout << "PASS " << mode << '\n'; return 0;
    }
    auto* memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x5000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Expect(memory != nullptr, "VirtualAlloc");
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(memory);
    dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 0x100;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(memory + 0x100);
    nt->Signature = IMAGE_NT_SIGNATURE; nt->FileHeader.NumberOfSections = 2;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->OptionalHeader.SizeOfImage = 0x5000;
    auto* sections = IMAGE_FIRST_SECTION(nt);
    sections[0].VirtualAddress = 0x1000; sections[0].Misc.VirtualSize = 0x1000;
    sections[0].Characteristics = IMAGE_SCN_MEM_EXECUTE;
    sections[1].VirtualAddress = 0x3000; sections[1].Misc.VirtualSize = 0x1000;
    const bool newer = mode == "3109";
    Pattern(memory + 0x1100, newer ? kAdvertisePattern309 : kAdvertisePattern);
    if (mode != "missing-gate") Pattern(memory + 0x1200, newer ? kValidatePattern309 : kValidatePattern);
    if (mode == "duplicate-gate") Pattern(memory + 0x1300, kAdvertisePattern);
    if (mode == "unknown") memory[0x1100] = 0;

    // One complete fatbin: an Ada image and Blackwell PTX with an equal-length target directive.
    auto* container = memory + 0x3000;
    Put<uint32_t>(container, 0xba55ed50); Put<uint16_t>(container + 6, 16);
    Put<uint64_t>(container + 8, 128);
    auto* ada = container + 16;
    auto* blackwell = ada + 64;
    Put<uint16_t>(ada, 2); Put<uint32_t>(ada + 4, 32);
    Put<uint64_t>(ada + 8, 32); Put<uint32_t>(ada + 28, 89);
    Put<uint16_t>(blackwell, 1); Put<uint32_t>(blackwell + 4, 32);
    Put<uint64_t>(blackwell + 8, 32); Put<uint32_t>(blackwell + 28, 120);
    std::memcpy(blackwell + 32, ".target sm_120", 14);
    if (mode == "no-kernel") std::memset(blackwell + 32, 0, 14);
    if (mode == "malformed") Put<uint64_t>(blackwell + 8, UINT64_MAX);
    const std::vector<uint8_t> before(memory, memory + 0x5000);
    auto module = reinterpret_cast<HMODULE>(memory);
    MfgUnlock::TryApply(module);
    const bool shouldPatch = mode == "legacy" || mode == "3109";
    if (shouldPatch)
    {
        Expect(MfgUnlock::UnlockedMax() == 5, "Maximum must require successful kernels and both gates");
        Expect(Read<uint32_t>(ada + 28) == 122 && Read<uint32_t>(blackwell + 28) == 89, "Wrong kernel routing");
        Expect(std::memcmp(blackwell + 32, ".target sm_89 ", 14) == 0, "PTX target was not retargeted");
        Expect(MfgUnlock::LastStatus().KernelsRewritten == 1, "Wrong kernel count");
        if (newer)
            Expect(memory[0x1106] == 0x0f && memory[0x1107] == 0x1f && memory[0x1205] == 0xb0, "310.9 gates");
        else
            Expect(memory[0x1107] == 5 && memory[0x1111] == 0x0f && memory[0x1205] == 0x90 && memory[0x1209] == 5, "Legacy gates");
        const std::vector<uint8_t> patched(memory, memory + 0x5000);
        MfgUnlock::TryApply(module);
        Expect(std::memcmp(memory, patched.data(), patched.size()) == 0, "Repeated patch changed the image");
        Config::Instance()->FGDLSSGAdaMfgUnlock.enabled = false;
        Expect(MfgUnlock::EnabledForSession(), "Disabling must wait for restart");
    }
    else
    {
        Expect(MfgUnlock::UnlockedMax() == 0, "Unsupported case advertised MFG");
        Expect(std::memcmp(memory, before.data(), before.size()) == 0, "Unsupported case modified memory");
    }
    VirtualFree(memory, 0, MEM_RELEASE);
    std::cout << "PASS " << mode << '\n';
    return 0;
}
catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }

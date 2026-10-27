#include "../OptiScaler/dlssnr/DlssNr_DriverGate.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

// DLSS Unlocked #60: an RTX 2080 Ti on driver 576.88 hung the whole GPU when NR was enabled. NGX rejected
// feature 18 with FAIL_OutOfDate and the direct runtime then ran the model anyway. Neural Rendering must not be
// activatable on Windows below 610.88, the oldest driver verified to work; 616.64 (first official) is recommended.
//
// Validates:
// 1. Resolve(): 610.87 and 576.88 are too old, 610.88 and newer (including 616.64) are supported.
// 2. Wine/Proton is exempt (dxvk-nvapi reports Linux numbers); an unknown version (0) does not block.
// 3. Version formatting and the user-facing message.
// 4. Publish() exposes the verdict to the menu and proxy; only TooOld blocks.
// 5. IdentifyGpu reads the version with NvAPI_SYS_GetDriverAndBranchVersion.
// 6. Startup turns NR off with a volatile value (INI untouched) only when too old, from the GPU worker.
// 7. The volatile off survives Config::Reload / profile loads: set_from_config never replaces a set value, and
//    value_for_config still saves the user's own setting.
// 8. The NR proxy refuses creation before any NGX call while blocked.

using namespace DlssNr::DriverGate;

static std::string ReadFile(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(1);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

static std::string Between(const std::string& text, const std::string& from, const std::string& to)
{
    const auto start = text.find(from);
    assert(start != std::string::npos && "start marker not found");
    const auto end = text.find(to, start + from.size());
    assert(end != std::string::npos && "end marker not found");
    return text.substr(start, end - start);
}

int main()
{
    // Test 1
    static_assert(MinimumWindowsDriver == 61088);
    static_assert(RecommendedWindowsDriver == 61664);
    static_assert(Resolve(false, 57688) == Verdict::TooOld);
    static_assert(Resolve(false, 61087) == Verdict::TooOld);
    static_assert(Resolve(false, 61088) == Verdict::Supported);
    static_assert(Resolve(false, 61663) == Verdict::Supported);
    static_assert(Resolve(false, 61664) == Verdict::Supported);
    static_assert(Resolve(false, 70012) == Verdict::Supported);
    std::puts("Test 1 PASS: 610.88 is the oldest allowed Windows driver");

    // Test 2
    static_assert(Resolve(true, 57688) == Verdict::NotApplicable);
    static_assert(Resolve(true, 0) == Verdict::NotApplicable);
    static_assert(Resolve(false, 0) == Verdict::Unknown);
    std::puts("Test 2 PASS: Wine exempt, unknown version does not block");

    // Test 3
    assert(FormatVersion(61664) == "616.64");
    assert(FormatVersion(57688) == "576.88");
    assert(FormatVersion(60005) == "600.05");
    const auto message = BlockedMessage(57688);
    assert(message.find("610.88 or newer") != std::string::npos);
    assert(message.find("616.64+ recommended") != std::string::npos);
    assert(message.find("detected 576.88") != std::string::npos);
    assert(message.find("Update your driver") != std::string::npos);
    std::puts("Test 3 PASS: versions and message are formatted for the user");

    // Test 4
    Publish(Verdict::TooOld, 57688);
    assert(Blocked() && DetectedVersion() == 57688);
    Publish(Verdict::Supported, 61664);
    assert(!Blocked() && DetectedVersion() == 61664);
    Publish(Verdict::Unknown, 0);
    assert(!Blocked());
    Publish(Verdict::NotApplicable, 58017);
    assert(!Blocked());
    std::puts("Test 4 PASS: only a too-old driver blocks");

    const std::string identify = ReadFile("OptiScaler/misc/IdentifyGpu.cpp");
    const std::string identifyHeader = ReadFile("OptiScaler/misc/IdentifyGpu.h");
    const std::string dllmain = ReadFile("OptiScaler/dllmain.cpp");
    const std::string config = ReadFile("OptiScaler/Config.h");
    const std::string proxy = ReadFile("OptiScaler/dlssnr/DlssNr_Proxy.cpp");

    // Test 5
    assert(identifyHeader.find("uint32_t nvidiaDriverVersion = 0;") != std::string::npos);
    const auto query =
        Between(identify, "void IdentifyGpu::queryNvapi", "NtdllProxy::FreeLibrary_Ldr(nvapiModule);\n\n");
    assert(query.find("GET_INTERFACE(NvAPI_SYS_GetDriverAndBranchVersion") != std::string::npos);
    assert(query.find("gpuInfo.nvidiaDriverVersion = driverVersion;") != std::string::npos);
    std::puts("Test 5 PASS: driver version comes from NVAPI");

    // Test 6
    const auto worker = Between(dllmain, "DWORD WINAPI getGpuInfo(", "\n}\n");
    const auto resolve = worker.find("DlssNr::DriverGate::Resolve(State::Instance().isRunningOnLinux");
    const auto publish = worker.find("DlssNr::DriverGate::Publish(verdict");
    const auto volatileOff = worker.find("DlssNrEnabled.set_volatile_value(false)");
    assert(resolve != std::string::npos && publish != std::string::npos && volatileOff != std::string::npos);
    assert(resolve < publish && publish < volatileOff);
    assert(worker.find("verdict == DlssNr::DriverGate::Verdict::TooOld") < volatileOff);
    assert(dllmain.find("DlssNrEnabled = false") == std::string::npos && "never rewrite the user's setting");
    std::puts("Test 6 PASS: startup disables NR with a volatile value only when too old");

    // Test 7
    const auto fromConfig = Between(config, "constexpr void set_from_config(const std::optional<T>& opt)", "}\n    }");
    assert(fromConfig.find("if (!this->has_value())") != std::string::npos &&
           "Reload must not replace the volatile off with the INI's Enabled=true");
    const auto forConfig = Between(config, "constexpr std::optional<T> value_for_config()", "}\n");
    assert(forConfig.find("if (_volatile)") != std::string::npos && "saving keeps the user's own value");
    std::puts("Test 7 PASS: the block survives reloads and never reaches the INI");

    // Test 8
    const auto prepare = Between(proxy, "unsigned int Context::Impl::Prepare(", "D3D12_CreateFeature()");
    const auto gate = prepare.find("if (DriverGate::Blocked())");
    const auto ngxInit = prepare.find("NVNGXProxy::IsDx12Inited()");
    assert(gate != std::string::npos && ngxInit != std::string::npos && gate < ngxInit);
    assert(prepare.find("NVSDK_NGX_Result_FAIL_OutOfDate") != std::string::npos);
    std::puts("Test 8 PASS: the proxy refuses creation before touching NGX");

    std::puts("All NR driver gate tests passed (8/8).");
    return 0;
}

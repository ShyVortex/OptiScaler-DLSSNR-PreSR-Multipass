#include "../OptiScaler/framegen/dlssg/AmpereMfgLoader.h"
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>

int main()
{
    std::printf("=== Running Turing Dual Runtime & Router Safety Unit Tests ===\n");

    using namespace AmpereMfgLoader;

    // Test 1: Binary detection of SM75 kernel family and runtime model (0.3.1 310.9 vs 310.1 vs legacy 0.3.0)
    {
        std::filesystem::path p3109 = std::filesystem::exists("dlssg_for_sm86/sdli1995/version.dll")
                                          ? "dlssg_for_sm86/sdli1995/version.dll"
                                          : "dlssg_for_sm86/version.dll";
        std::filesystem::path p3101 = std::filesystem::exists("dlssg_for_sm86/sdli1995/310.1/version.dll")
                                          ? "dlssg_for_sm86/sdli1995/310.1/version.dll"
                                          : "dlssg_for_sm86/310.1/version.dll";

        if (std::filesystem::exists(p3109) && std::filesystem::exists(p3101))
        {
            // In 0.3.1, root 310.9 contains native SM75 kernels (unified build)
            assert(HasSm75KernelFamily(p3109) && "0.3.1 310.9 root runtime must be identified as having SM75 kernels!");
            assert(HasSm75KernelFamily(p3101) && "310.1 runtime must be identified as having SM75 kernels!");

            // Runtime discrimination: 310.1 vs 310.9
            assert(!Is3101Runtime(p3109) && "Root 310.9 runtime must not be identified as 310.1!");
            assert(Is3101Runtime(p3101) && "310.1 runtime must be identified as 310.1!");

            std::printf("  [PASS] Case 1a: Binary inspection detects unified SM75 in 0.3.1 root 310.9 and in 310.1\n");
            std::printf("  [PASS] Case 1b: Is3101Runtime discriminates 310.1 (ceiling 3/4X) vs 310.9 (ceiling 5/6X)\n");
        }
        else
        {
            std::printf("  [SKIP] Case 1: dlssg_for_sm86 binaries not in working directory\n");
        }

        // Verify rejection of legacy 0.3.0 310.9 binary via synthetic mock file
        {
            std::filesystem::path mockLegacy = std::filesystem::temp_directory_path() / "mock_dlssg_030.dll";
            std::ofstream f(mockLegacy, std::ios::binary);
            std::string content = "prefix...sm75_family...The 310.9 backend has no SM75...suffix";
            f.write(content.data(), content.size());
            f.close();

            assert(!HasSm75KernelFamily(mockLegacy) && "Legacy 0.3.0 310.9 binary with abort error must be rejected!");
            std::filesystem::remove(mockLegacy);
            std::printf("  [PASS] Case 1c: Legacy 0.3.0 binary with no-SM75 error string correctly rejected\n");
        }
    }

    // Test 2: Legacy 310.9 runtime abort prevention on Turing hardware (Router must be Auto, not SM75)
    {
        // Turing 0x160 on legacy 310.9 (hasSm75Support = false): must yield "Auto"
        std::string routerAuto = ResolveRouter(0x00000160, "RTX 2070", "Auto", false);
        assert(routerAuto == "Auto" &&
               "On legacy 310.9, Turing Auto router must resolve to Auto to prevent runtime abort!");

        // Turing 0x160 with explicit "SM75" on legacy 310.9: must safely fall back to "Auto"
        std::string routerFallback = ResolveRouter(0x00000160, "RTX 2070", "SM75", false);
        assert(routerFallback == "Auto" && "Explicit SM75 on legacy 310.9 must fall back to Auto!");

        std::printf("  [PASS] Case 2: Legacy 310.9 runtime router safely set to Auto on Turing to prevent abort\n");
    }

    // Test 3: Runtime on Turing hardware with SM75 support uses dedicated SM75 kernels
    {
        // Turing 0x160 on the 310.1 runtime with SM75 kernels: Auto router resolves to SM75
        std::string routerSm75 = ResolveRouter(0x00000160, "RTX 2070", "Auto", true, true);
        assert(routerSm75 == "SM75" && "On the 310.1 runtime, Turing Auto router resolves to SM75!");

        // 310.9 with SM75 kernels (0.4.x): Auto stays Auto, the mod picks SM75 from the physical GPU (rule 4)
        assert(ResolveRouter(0x00000160, "RTX 2070", "Auto", true, false) == "Auto");
        assert(ResolveRouter(0, "NVIDIA GeForce RTX 2080 Ti", "Auto", true, false) == "Auto");

        // Explicit "SM75" honored whenever the runtime has SM75 kernels
        assert(ResolveRouter(0x00000160, "RTX 2070", "SM75", true) == "SM75");
        assert(ResolveRouter(0x00000160, "RTX 2070", "SM75", true, true) == "SM75");

        std::printf("  [PASS] Case 3: Turing routes SM75 on 310.1, Auto on 310.9; explicit SM75 honoured\n");
    }

    // Test 4: Ampere (0x170) is completely unaffected and always routes to SM86
    {
        assert(ResolveRouter(0x00000170, "RTX 3080", "Auto", false) == "SM86");
        assert(ResolveRouter(0x00000170, "RTX 3080", "Auto", true) == "SM86");
        assert(ResolveRouter(0x00000170, "RTX 3080", "SM86", false) == "SM86");
        std::printf("  [PASS] Case 4: Ampere routing strictly preserved as SM86 across runtimes\n");
    }

    // Test 5: Expanded GPU name heuristics for Turing & Ampere
    {
        // Turing names without archId (310.1 runtime with SM75 kernels, so the name decides SM75)
        assert(ResolveRouter(0, "NVIDIA TITAN RTX", "Auto", true, true) == "SM75");
        assert(ResolveRouter(0, "NVIDIA TITAN RTX", "Auto", false, true) == "Auto");
        assert(ResolveRouter(0, "TU104 [GeForce RTX 2080]", "Auto", true, true) == "SM75");
        assert(ResolveRouter(0, "TU116 [GeForce GTX 1660 SUPER]", "Auto", true, true) == "SM75");
        assert(ResolveRouter(0, "NVIDIA GeForce Turing Card", "Auto", true, true) == "SM75");

        // Ampere names
        assert(ResolveRouter(0, "NVIDIA RTX A4000", "Auto", false) == "SM86");
        assert(ResolveRouter(0, "GA102 [GeForce RTX 3090]", "Auto", false) == "SM86");

        std::printf("  [PASS] Case 5: Expanded GPU heuristics (TITAN RTX, TU10, TU11, RTX A) verified\n");
    }

    // Test 6: INI formatting with Auto, SM75, and SM86
    {
        std::string iniAuto = FormatIniContent(3, "Auto", 0, "Auto", 1);
        assert(iniAuto.find("Router=Auto\n") != std::string::npos);

        std::string iniSm75 = FormatIniContent(3, "PTX", 0, "SM75", 1);
        assert(iniSm75.find("Router=SM75\n") != std::string::npos);

        std::string iniSm86 = FormatIniContent(3, "PTX", 0, "SM86", 1);
        assert(iniSm86.find("Router=SM86\n") != std::string::npos);

        std::printf("  [PASS] Case 6: FormatIniContent accepts and renders Router=Auto, SM75, SM86\n");
    }

    // Test 7: ResolveAutoKernelImage stability on Turing
    {
        assert(ResolveAutoKernelImage(0x160, "RTX 2080", false) == "PTX");
        assert(ResolveAutoKernelImage(0, "NVIDIA TITAN RTX", false) == "PTX");
        assert(ResolveAutoKernelImage(0, "TU104", false) == "PTX");
        assert(ResolveAutoKernelImage(0, "TU116", false) == "PTX");
        assert(ResolveAutoKernelImage(0x170, "RTX 3070", false) == "Auto");

        std::printf("  [PASS] Case 7: ResolveAutoKernelImage correctly selects PTX for all Turing variants\n");
    }

    // Test 8: 0.4.1 moved the SM75 marker out of the proxy; the modular backend beside it still has the kernels
    {
        const auto root = std::filesystem::temp_directory_path() / "optiscaler_sm75_backend";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "dlssg_sm86" / "backend");
        const auto write = [](const std::filesystem::path& p, const std::string& text)
        {
            std::ofstream f(p, std::ios::binary);
            f << "MZ\x90\x00" << text;
        };
        const auto proxy = root / "dlssg_sm86.dll";
        const auto backend = root / "dlssg_sm86" / "backend" / "sm86_backend.dll";

        // Proxy without markers, no backend: no SM75
        write(proxy, "Auto was removed in 0.4.0 and sm86_backend.dll is next to the proxy");
        assert(!HasSm75KernelFamily(proxy));

        // Backend with SM75 kernel markers (0.4.1 layout): SM75 present
        write(backend, "cubin_sm75 ... executed_on_sm75_hardware");
        assert(HasSm75KernelFamily(proxy) && "0.4.1: SM75 kernels detected in the modular backend");

        // Backend without markers: no SM75
        write(backend, "sm86 kernels only");
        assert(!HasSm75KernelFamily(proxy));

        // A proxy that states it has no SM75 family wins over the backend
        write(backend, "cubin_sm75");
        write(proxy, "The 310.9 backend has no SM75 kernel family; use Router=Auto or SM86");
        assert(!HasSm75KernelFamily(proxy));

        // Real 0.4.1 release laid out as packaged, when present
        const std::filesystem::path real041 = "dlssg_for_sm86/sdli1995/version.dll";
        if (std::filesystem::exists(real041) &&
            std::filesystem::exists("dlssg_for_sm86/sdli1995/dlssg_sm86/backend/sm86_backend.dll"))
        {
            assert(HasSm75KernelFamily(real041) && "real 0.4.1 release must report SM75 kernels");
            std::printf("  [PASS] Case 8b: real sdli1995 0.4.1 release reports SM75 kernels from its backend\n");
        }

        std::filesystem::remove_all(root);
        std::printf("  [PASS] Case 8: SM75 kernels detected in the 0.4.x modular backend\n");
    }

    std::printf("=== All Turing Dual Runtime & Router Safety Unit Tests PASSED! ===\n");
    return 0;
}

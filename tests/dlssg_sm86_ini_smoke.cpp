#include "../OptiScaler/framegen/dlssg/AmpereMfgLoader.h"
#include <cassert>
#include <cstdio>
#include <string>

int main()
{
    using namespace AmpereMfgLoader;

    // 1. Clamping of MaxGeneratedFrames: Native 0.2.3 requires strictly 1, 2, or 3
    {
        // 0 (Runtime default in OptiScaler) must be clamped to 3 capability limit
        std::string ini0 = FormatIniContent(0, "PTX", 0, "SM86", 1);
        assert(ini0.find("MaxGeneratedFrames=3") != std::string::npos);
        assert(ini0.find("MaxGeneratedFrames=0") == std::string::npos);

        // Negative values must be clamped to 3
        std::string iniNeg = FormatIniContent(-5, "PTX", 0, "SM86", 1);
        assert(iniNeg.find("MaxGeneratedFrames=3") != std::string::npos);

        // Values above 3 must be clamped to 3
        std::string iniOver = FormatIniContent(4, "PTX", 0, "SM86", 1);
        assert(iniOver.find("MaxGeneratedFrames=3") != std::string::npos);

        // Valid values 1, 2, 3 must be preserved
        std::string ini1 = FormatIniContent(1, "PTX", 0, "SM86", 1);
        assert(ini1.find("MaxGeneratedFrames=1") != std::string::npos);

        std::string ini2 = FormatIniContent(2, "PTX", 0, "SM86", 1);
        assert(ini2.find("MaxGeneratedFrames=2") != std::string::npos);

        std::string ini3 = FormatIniContent(3, "PTX", 0, "SM86", 1);
        assert(ini3.find("MaxGeneratedFrames=3") != std::string::npos);
    }

    // 2. HardwareBilinear validation
    {
        std::string iniExact = FormatIniContent(3, "PTX", 0, "SM86", 1);
        assert(iniExact.find("HardwareBilinear=0") != std::string::npos);

        std::string iniApprox = FormatIniContent(3, "PTX", 1, "SM86", 1);
        assert(iniApprox.find("HardwareBilinear=1") != std::string::npos);

        std::string iniSanitized = FormatIniContent(3, "PTX", 42, "SM86", 1);
        assert(iniSanitized.find("HardwareBilinear=0") != std::string::npos);
    }

    // 3. KernelImage validation
    {
        std::string ptx = FormatIniContent(3, "PTX", 0, "SM86", 1);
        assert(ptx.find("KernelImage=PTX") != std::string::npos);

        std::string cubin = FormatIniContent(3, "Cubin", 0, "SM86", 1);
        assert(cubin.find("KernelImage=Cubin") != std::string::npos);

        std::string autoImg = FormatIniContent(3, "Auto", 0, "SM86", 1);
        assert(autoImg.find("KernelImage=Auto") != std::string::npos);

        std::string invalidImg = FormatIniContent(3, "Invalid", 0, "SM86", 1);
        assert(invalidImg.find("KernelImage=Auto") != std::string::npos);
    }

    // 4. Router validation
    {
        std::string sm86 = FormatIniContent(3, "PTX", 0, "SM86", 1);
        assert(sm86.find("Router=SM86") != std::string::npos);

        std::string sm75 = FormatIniContent(3, "PTX", 0, "SM75", 1);
        assert(sm75.find("Router=SM75") != std::string::npos);

        std::string fallback = FormatIniContent(3, "PTX", 0, "Unknown", 1);
        assert(fallback.find("Router=SM86") != std::string::npos);
    }

    // 5. Logging Level: Default 1 in Native 0.2.3
    {
        std::string defLog = FormatIniContent(3, "PTX", 0, "SM86");
        assert(defLog.find("Level=1") != std::string::npos);

        std::string diagLog = FormatIniContent(3, "PTX", 0, "SM86", 2);
        assert(diagLog.find("Level=2") != std::string::npos);

        std::string invalidLog = FormatIniContent(3, "PTX", 0, "SM86", -1);
        assert(invalidLog.find("Level=1") != std::string::npos);
    }

    // 6. Verify absence of obsolete 0.1.0 keys
    {
        std::string cleanIni = FormatIniContent(3, "PTX", 0, "SM86", 1);
        assert(cleanIni.find("ForceSM86Route") == std::string::npos);
        assert(cleanIni.find("SimulateAmpere") == std::string::npos);
        assert(cleanIni.find("[Runtime]") == std::string::npos);
        assert(cleanIni.find("[Backends]") == std::string::npos);
    }

    // 7. Architecture detection: Turing (SM75) and Ampere (SM86)
    {
        // Turing architecture ID (0x0160, TU100/TU102/TU104/TU106/TU116)
        assert(IsTuringArch(0x00000160) == true);
        assert(IsTuringArch(0x00000162) == true);
        assert(IsTuringArch(0x00000170) == false); // Ampere is not Turing
        assert(IsTuringArch(0x00000130) == false); // Pascal is not Turing

        // Ampere architecture ID (0x0170, GA100/GA102/GA104/GA106)
        assert(IsAmpereArch(0x00000170) == true);
        assert(IsAmpereArch(0x00000172) == true);
        assert(IsAmpereArch(0x00000160) == false); // Turing is not Ampere
        assert(IsAmpereArch(0x00000190) == false); // Ada is not Ampere
    }

    // 8. ResolveRouter hardware routing
    {
        // Direct architecture ID resolution
        assert(ResolveRouter(0x00000160) == "SM75");
        assert(ResolveRouter(0x00000170) == "SM86");

        // GPU name fallback resolution when arch ID is masked/generic
        assert(ResolveRouter(0, "NVIDIA GeForce RTX 2070 SUPER") == "SM75");
        assert(ResolveRouter(0, "NVIDIA GeForce GTX 1660 Ti") == "SM75");
        assert(ResolveRouter(0, "NVIDIA GeForce RTX 3080 Ti Laptop GPU") == "SM86");
        assert(ResolveRouter(0, "NVIDIA GeForce RTX 3060") == "SM86");

        // Default fallback for unrecognized hardware
        assert(ResolveRouter(0, "") == "SM86");

        // End-to-end INI output with router resolution
        std::string turingIni = FormatIniContent(3, "PTX", 0, ResolveRouter(0x160));
        assert(turingIni.find("Router=SM75") != std::string::npos);

        std::string ampereIni = FormatIniContent(3, "PTX", 0, ResolveRouter(0x170));
        assert(ampereIni.find("Router=SM86") != std::string::npos);
    }

    // 9. Single-Frame (2X) DLSS Frame Generation exact INI verification
    {
        std::string fg2x = FormatIniContent(1, "PTX", 0, "SM86", 1);
        assert(fg2x.find("[FrameGeneration]\nMaxGeneratedFrames=1\n") != std::string::npos);
        assert(fg2x.find("Router=SM86\n") != std::string::npos);
        assert(fg2x.find("KernelImage=PTX\n") != std::string::npos);
        assert(fg2x.find("HardwareBilinear=0\n") != std::string::npos);
        assert(fg2x.find("Level=1\n") != std::string::npos);

        std::string expected2x =
            "; Native 0.2.3. Restart the game after changing this file.\n"
            "[Compatibility]\n"
            "Router=SM86\n"
            "KernelImage=PTX\n"
            "HardwareBilinear=0\n\n"
            "[FrameGeneration]\n"
            "MaxGeneratedFrames=1\n\n"
            "[Logging]\n"
            "Level=1\n";
        assert(fg2x == expected2x);
    }

    assert(ResolveAutoKernelImage(0x170, "NVIDIA GeForce RTX 3060", true) == "PTX");
    assert(ResolveAutoKernelImage(0x170, "NVIDIA GeForce RTX 3060", false) == "Auto");
    assert(ResolveAutoKernelImage(0x170, "NVIDIA GeForce RTX 3070 Laptop GPU", false) == "PTX");
    assert(ResolveAutoKernelImage(0x160, "NVIDIA GeForce RTX 2080", false) == "PTX");
    std::puts("PASS: dlssg_sm86_ini_smoke (INI, architecture and environment routing)");
    return 0;
}


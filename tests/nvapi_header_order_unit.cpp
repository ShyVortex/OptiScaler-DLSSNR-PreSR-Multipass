#include "../OptiScaler/framegen/dlssg/AmpereMfgLoader.h"

#ifndef _WIN32
#ifndef __cdecl
#define __cdecl
#endif
#ifndef __success
#define __success(x)
#endif
#ifndef __NVAPI_EMPTY_SAL
#define __NVAPI_EMPTY_SAL
#endif
#endif

#include <nvapi.h>

#include <cassert>
#include <cstdio>
#include <string>

int main()
{
    std::printf("=== Running NVAPI Header Inclusion Order & Architecture Enum Unit Tests ===\n");

    // Case 1: NV_GPU_ARCH_INFO_V2 struct and architecture_id member validity
    {
        NV_GPU_ARCH_INFO_V2 archInfo {};
        archInfo.version = sizeof(NV_GPU_ARCH_INFO_V2) | (2 << 16);
        archInfo.architecture = NV_GPU_ARCHITECTURE_TU100;
        archInfo.architecture_id = NV_GPU_ARCHITECTURE_TU100;

        assert(archInfo.architecture_id == NV_GPU_ARCHITECTURE_TU100 &&
               "NV_GPU_ARCH_INFO_V2::architecture_id must hold NV_GPU_ARCHITECTURE_TU100");
        std::printf("  [PASS] Case 1: NV_GPU_ARCH_INFO_V2 struct definition and architecture_id member compiled "
                    "cleanly\n");
    }

    // Case 2: Exact numerical values of NV_GPU_ARCHITECTURE_ID enum constants
    {
        assert(NV_GPU_ARCHITECTURE_TU100 == 0x00000160 && "TU100 must evaluate to 0x160");
        assert(NV_GPU_ARCHITECTURE_GA100 == 0x00000170 && "GA100 must evaluate to 0x170");
        assert(NV_GPU_ARCHITECTURE_AD100 == 0x00000190 && "AD100 must evaluate to 0x190");
        assert(NV_GPU_ARCHITECTURE_GB200 == 0x000001B0 && "GB200 must evaluate to 0x1B0");
        std::printf("  [PASS] Case 2: Architecture enum constants (TU100, GA100, AD100, GB200) match specifications\n");
    }

    // Case 3: AmpereMfgLoader architecture detection helpers
    {
        assert(AmpereMfgLoader::IsTuringArch(NV_GPU_ARCHITECTURE_TU100) &&
               "IsTuringArch must return true for NV_GPU_ARCHITECTURE_TU100");
        assert(AmpereMfgLoader::IsTuringArch(0x00000160) && "IsTuringArch must return true for 0x160");
        assert(AmpereMfgLoader::IsTuringArch(0x00000162) && "IsTuringArch must return true for TU102 (0x162)");
        assert(!AmpereMfgLoader::IsTuringArch(NV_GPU_ARCHITECTURE_GA100) && "IsTuringArch must return false for GA100");
        assert(!AmpereMfgLoader::IsTuringArch(NV_GPU_ARCHITECTURE_AD100) && "IsTuringArch must return false for AD100");

        assert(AmpereMfgLoader::IsAmpereArch(NV_GPU_ARCHITECTURE_GA100) &&
               "IsAmpereArch must return true for NV_GPU_ARCHITECTURE_GA100");
        assert(AmpereMfgLoader::IsAmpereArch(0x00000170) && "IsAmpereArch must return true for 0x170");
        assert(AmpereMfgLoader::IsAmpereArch(0x00000172) && "IsAmpereArch must return true for GA102 (0x172)");
        assert(!AmpereMfgLoader::IsAmpereArch(NV_GPU_ARCHITECTURE_TU100) && "IsAmpereArch must return false for TU100");
        assert(!AmpereMfgLoader::IsAmpereArch(NV_GPU_ARCHITECTURE_AD100) && "IsAmpereArch must return false for AD100");

        std::printf("  [PASS] Case 3: AmpereMfgLoader::IsTuringArch and IsAmpereArch discrimination verified\n");
    }

    // Case 4: FormatIniContent030 Turing arch safety with realArch = NV_GPU_ARCHITECTURE_TU100
    {
        std::string iniTuring =
            AmpereMfgLoader::FormatIniContent030(3, 1, "Auto", "Auto", 0, "Auto", 1, "Auto", false, 0.0f, false, false,
                                                 static_cast<uint32_t>(NV_GPU_ARCHITECTURE_TU100));
        assert(iniTuring.find("SpoofArchToGame=0") != std::string::npos &&
               "On Turing hardware, SpoofArchToGame must resolve to 0 in FormatIniContent030");

        std::string iniAmpere =
            AmpereMfgLoader::FormatIniContent030(3, 1, "Auto", "Auto", 0, "Auto", 1, "Auto", false, 0.0f, false, false,
                                                 static_cast<uint32_t>(NV_GPU_ARCHITECTURE_GA100));
        assert(iniAmpere.find("SpoofArchToGame=0") == std::string::npos &&
               "On Ampere hardware without internal mode, SpoofArchToGame must not be forced to 0");

        std::printf("  [PASS] Case 4: FormatIniContent030 architecture safety verified for Turing and Ampere\n");
    }

    std::printf("=== All NVAPI Header Inclusion Order & Architecture Enum Unit Tests PASSED! ===\n");
    return 0;
}

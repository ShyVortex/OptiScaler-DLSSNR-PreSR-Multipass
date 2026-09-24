#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <algorithm>

constexpr uint32_t NV_GPU_ARCHITECTURE_TU100 = 0x00000160;
constexpr uint32_t NV_GPU_ARCHITECTURE_GA100 = 0x00000170;
constexpr uint32_t NV_GPU_ARCHITECTURE_AD100 = 0x00000190;
constexpr uint32_t NV_GPU_ARCHITECTURE_GB200 = 0x000001b0;

struct MockArchInfo
{
    uint32_t architecture = 0;
    uint32_t architecture_id = 0;
    uint32_t implementation = 0;
    uint32_t implementation_id = 0;
    uint32_t revision = 0;
    uint32_t revision_id = 0;
};

struct MockGpuInfo
{
    bool isNvidia = true;
    MockArchInfo nvidiaArchInfo {};
};

// Emulates the bidirectional architecture isolation logic in hkNvAPI_GPU_GetArchInfo
void FilterArchInfoForCaller(MockArchInfo* pGpuArchInfo, const MockGpuInfo& primaryGpu, const std::string& caller,
                             bool mfgUnlock = true)
{
    if (!pGpuArchInfo)
        return;

    const auto realArch = primaryGpu.nvidiaArchInfo.architecture_id != 0 ? primaryGpu.nvidiaArchInfo.architecture_id
                                                                         : primaryGpu.nvidiaArchInfo.architecture;

    if (primaryGpu.isNvidia && realArch != 0 && realArch < NV_GPU_ARCHITECTURE_AD100)
    {
        std::string callerLower = caller;
        std::transform(callerLower.begin(), callerLower.end(), callerLower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        const bool isExplicitNonFgCaller =
            (callerLower.find("nvngx_dlss.") != std::string::npos ||
             callerLower.find("nvngx_dlssd") != std::string::npos || callerLower.ends_with(".exe"));

        const bool isFgCaller =
            !isExplicitNonFgCaller &&
            (callerLower.find("sl.common") != std::string::npos || callerLower.find("sl.dlss_g") != std::string::npos ||
             callerLower.find("sl.interposer") != std::string::npos || callerLower.find("dlssg") != std::string::npos ||
             callerLower.find("version") != std::string::npos || callerLower == "_nvngx.dll" ||
             callerLower == "nvngx.dll" || callerLower.find("_nvngx") != std::string::npos);

        if (mfgUnlock && isFgCaller)
        {
            if (pGpuArchInfo->architecture < NV_GPU_ARCHITECTURE_AD100)
            {
                pGpuArchInfo->architecture = NV_GPU_ARCHITECTURE_AD100;
                pGpuArchInfo->architecture_id = NV_GPU_ARCHITECTURE_AD100;
                pGpuArchInfo->implementation = 0x102;
                pGpuArchInfo->implementation_id = 0x102;
            }
        }
        else if (!isFgCaller && pGpuArchInfo->architecture >= NV_GPU_ARCHITECTURE_AD100)
        {
            pGpuArchInfo->architecture = realArch;
            pGpuArchInfo->architecture_id = realArch;
            pGpuArchInfo->implementation = primaryGpu.nvidiaArchInfo.implementation;
            pGpuArchInfo->implementation_id = primaryGpu.nvidiaArchInfo.implementation_id;
            pGpuArchInfo->revision = primaryGpu.nvidiaArchInfo.revision;
            pGpuArchInfo->revision_id = primaryGpu.nvidiaArchInfo.revision_id;
        }
    }
}

int main()
{
    std::printf("=== Running NvAPI Architecture Caller Isolation Unit Tests ===\n");

    // Case 1: RTX 3090 (Ampere 0x170) with external mod spoofed to Blackwell (0x1b0)
    {
        MockGpuInfo gpuAmpere;
        gpuAmpere.isNvidia = true;
        gpuAmpere.nvidiaArchInfo.architecture = NV_GPU_ARCHITECTURE_GA100;
        gpuAmpere.nvidiaArchInfo.architecture_id = NV_GPU_ARCHITECTURE_GA100;
        gpuAmpere.nvidiaArchInfo.implementation = 0x102;
        gpuAmpere.nvidiaArchInfo.implementation_id = 0x102;
        gpuAmpere.nvidiaArchInfo.revision = 0xa1;
        gpuAmpere.nvidiaArchInfo.revision_id = 0xa1;

        // 1a: Caller nvngx_dlss.dll (DLSS Super Resolution) MUST see Ampere (0x170)
        {
            MockArchInfo arch { NV_GPU_ARCHITECTURE_GB200, NV_GPU_ARCHITECTURE_GB200, 0x202, 0x202, 0, 0 };
            FilterArchInfoForCaller(&arch, gpuAmpere, "nvngx_dlss.dll");
            assert(arch.architecture == NV_GPU_ARCHITECTURE_GA100 && "nvngx_dlss must receive real Ampere arch!");
            assert(arch.implementation == 0x102 && "nvngx_dlss must receive real Ampere implementation!");
        }

        // 1b: Caller nvngx_dlssd.dll (Ray Reconstruction) MUST see Ampere (0x170)
        {
            MockArchInfo arch { NV_GPU_ARCHITECTURE_GB200, NV_GPU_ARCHITECTURE_GB200, 0x202, 0x202, 0, 0 };
            FilterArchInfoForCaller(&arch, gpuAmpere, "nvngx_dlssd.dll");
            assert(arch.architecture == NV_GPU_ARCHITECTURE_GA100 && "nvngx_dlssd must receive real Ampere arch!");
        }

        // 1c: Caller tlou-ii.exe (The Last of Us Part II) MUST see Ampere (0x170)
        {
            MockArchInfo arch { NV_GPU_ARCHITECTURE_GB200, NV_GPU_ARCHITECTURE_GB200, 0x202, 0x202, 0, 0 };
            FilterArchInfoForCaller(&arch, gpuAmpere, "tlou-ii.exe");
            assert(arch.architecture == NV_GPU_ARCHITECTURE_GA100 && "Game exe must receive real Ampere arch!");
        }

        // 1d: Caller Resonance.exe MUST see Ampere (0x170)
        {
            MockArchInfo arch { NV_GPU_ARCHITECTURE_GB200, NV_GPU_ARCHITECTURE_GB200, 0x202, 0x202, 0, 0 };
            FilterArchInfoForCaller(&arch, gpuAmpere, "Resonance.exe");
            assert(arch.architecture == NV_GPU_ARCHITECTURE_GA100 && "Resonance.exe must receive real Ampere arch!");
        }

        // 1e: Caller sl.dlss_g.dll (Streamline DLSS-G) MUST see Blackwell (0x1b0)
        {
            MockArchInfo arch { NV_GPU_ARCHITECTURE_GB200, NV_GPU_ARCHITECTURE_GB200, 0x202, 0x202, 0, 0 };
            FilterArchInfoForCaller(&arch, gpuAmpere, "sl.dlss_g.dll");
            assert(arch.architecture == NV_GPU_ARCHITECTURE_GB200 && "sl.dlss_g must preserve spoofed Blackwell arch!");
        }

        // 1f: Caller dlssg_sm86.dll MUST see Blackwell (0x1b0)
        {
            MockArchInfo arch { NV_GPU_ARCHITECTURE_GB200, NV_GPU_ARCHITECTURE_GB200, 0x202, 0x202, 0, 0 };
            FilterArchInfoForCaller(&arch, gpuAmpere, "dlssg_sm86.dll");
            assert(arch.architecture == NV_GPU_ARCHITECTURE_GB200 &&
                   "dlssg_sm86 must preserve spoofed Blackwell arch!");
        }

        // 1g: Caller version.dll (SilyNoMeta external mod) MUST see Blackwell (0x1b0)
        {
            MockArchInfo arch { NV_GPU_ARCHITECTURE_GB200, NV_GPU_ARCHITECTURE_GB200, 0x202, 0x202, 0, 0 };
            FilterArchInfoForCaller(&arch, gpuAmpere, "version.dll");
            assert(arch.architecture == NV_GPU_ARCHITECTURE_GB200 &&
                   "version.dll must preserve spoofed Blackwell arch!");
        }

        std::printf(
            "  [PASS] Case 1: RTX 3090 Ampere arch protected for DLSS SR/RR, Blackwell preserved for Streamline FG\n");
    }

    // Case 2: Physical RTX 3080/3090 (Ampere 0x170) without external mod spoofing (OptiScaler Ampere MFG Unlock)
    {
        MockGpuInfo gpuAmpere;
        gpuAmpere.isNvidia = true;
        gpuAmpere.nvidiaArchInfo.architecture = NV_GPU_ARCHITECTURE_GA100;
        gpuAmpere.nvidiaArchInfo.architecture_id = NV_GPU_ARCHITECTURE_GA100;
        gpuAmpere.nvidiaArchInfo.implementation = 0x102;
        gpuAmpere.nvidiaArchInfo.implementation_id = 0x102;
        gpuAmpere.nvidiaArchInfo.revision = 0xa1;
        gpuAmpere.nvidiaArchInfo.revision_id = 0xa1;

        // 2a: Caller _nvngx.dll (NGX Snippet Loader) MUST be spoofed to Ada (0x190) so snippet validation accepts GPU
        {
            MockArchInfo arch { NV_GPU_ARCHITECTURE_GA100, NV_GPU_ARCHITECTURE_GA100, 0x102, 0x102, 0xa1, 0xa1 };
            FilterArchInfoForCaller(&arch, gpuAmpere, "_nvngx.dll", true);
            assert(arch.architecture == NV_GPU_ARCHITECTURE_AD100 && "_nvngx.dll must be spoofed to Ada (0x190)!");
            assert(arch.architecture_id == NV_GPU_ARCHITECTURE_AD100);
            assert(arch.implementation == 0x102);
        }

        // 2b: Caller version.dll (SilyNoMeta mod) MUST be spoofed to Ada (0x190)
        {
            MockArchInfo arch { NV_GPU_ARCHITECTURE_GA100, NV_GPU_ARCHITECTURE_GA100, 0x102, 0x102, 0xa1, 0xa1 };
            FilterArchInfoForCaller(&arch, gpuAmpere, "version.dll", true);
            assert(arch.architecture == NV_GPU_ARCHITECTURE_AD100 && "version.dll must be spoofed to Ada (0x190)!");
        }

        // 2c: Caller sl.dlss_g.dll MUST be spoofed to Ada (0x190)
        {
            MockArchInfo arch { NV_GPU_ARCHITECTURE_GA100, NV_GPU_ARCHITECTURE_GA100, 0x102, 0x102, 0xa1, 0xa1 };
            FilterArchInfoForCaller(&arch, gpuAmpere, "sl.dlss_g.dll", true);
            assert(arch.architecture == NV_GPU_ARCHITECTURE_AD100 && "sl.dlss_g.dll must be spoofed to Ada (0x190)!");
        }

        // 2d: Caller sl.common.dll MUST be spoofed to Ada (0x190)
        {
            MockArchInfo arch { NV_GPU_ARCHITECTURE_GA100, NV_GPU_ARCHITECTURE_GA100, 0x102, 0x102, 0xa1, 0xa1 };
            FilterArchInfoForCaller(&arch, gpuAmpere, "sl.common.dll", true);
            assert(arch.architecture == NV_GPU_ARCHITECTURE_AD100 && "sl.common.dll must be spoofed to Ada (0x190)!");
        }

        // 2e: Caller nvngx_dlss.dll (DLSS SR) MUST NOT be spoofed (remains real Ampere 0x170)
        {
            MockArchInfo arch { NV_GPU_ARCHITECTURE_GA100, NV_GPU_ARCHITECTURE_GA100, 0x102, 0x102, 0xa1, 0xa1 };
            FilterArchInfoForCaller(&arch, gpuAmpere, "nvngx_dlss.dll", true);
            assert(arch.architecture == NV_GPU_ARCHITECTURE_GA100 && "nvngx_dlss.dll must remain real Ampere (0x170)!");
        }

        // 2f: Caller nvngx_dlssd.dll (Ray Reconstruction) MUST NOT be spoofed (remains real Ampere 0x170)
        {
            MockArchInfo arch { NV_GPU_ARCHITECTURE_GA100, NV_GPU_ARCHITECTURE_GA100, 0x102, 0x102, 0xa1, 0xa1 };
            FilterArchInfoForCaller(&arch, gpuAmpere, "nvngx_dlssd.dll", true);
            assert(arch.architecture == NV_GPU_ARCHITECTURE_GA100 &&
                   "nvngx_dlssd.dll must remain real Ampere (0x170)!");
        }

        // 2g: Caller Resonance.exe (Game executable) MUST NOT be spoofed (remains real Ampere 0x170)
        {
            MockArchInfo arch { NV_GPU_ARCHITECTURE_GA100, NV_GPU_ARCHITECTURE_GA100, 0x102, 0x102, 0xa1, 0xa1 };
            FilterArchInfoForCaller(&arch, gpuAmpere, "Resonance.exe", true);
            assert(arch.architecture == NV_GPU_ARCHITECTURE_GA100 && "Resonance.exe must remain real Ampere (0x170)!");
        }

        // 2h: When mfgUnlock == false, _nvngx.dll and sl.dlss_g MUST NOT be spoofed
        {
            MockArchInfo archNvngx { NV_GPU_ARCHITECTURE_GA100, NV_GPU_ARCHITECTURE_GA100, 0x102, 0x102, 0xa1, 0xa1 };
            FilterArchInfoForCaller(&archNvngx, gpuAmpere, "_nvngx.dll", false);
            assert(archNvngx.architecture == NV_GPU_ARCHITECTURE_GA100 &&
                   "When mfgUnlock is false, _nvngx must not be spoofed!");

            MockArchInfo archSl { NV_GPU_ARCHITECTURE_GA100, NV_GPU_ARCHITECTURE_GA100, 0x102, 0x102, 0xa1, 0xa1 };
            FilterArchInfoForCaller(&archSl, gpuAmpere, "sl.dlss_g.dll", false);
            assert(archSl.architecture == NV_GPU_ARCHITECTURE_GA100 &&
                   "When mfgUnlock is false, sl.dlss_g must not be spoofed!");
        }

        std::printf("  [PASS] Case 2: RTX 3080/3090 Ampere bidirectional spoofing: _nvngx/version/Streamline get Ada "
                    "(0x190), DLSS SR/RR and game exe keep Ampere (0x170)\n");
    }

    // Case 3: RTX 2080 (Turing 0x160) bidirectional spoofing
    {
        MockGpuInfo gpuTuring;
        gpuTuring.isNvidia = true;
        gpuTuring.nvidiaArchInfo.architecture = NV_GPU_ARCHITECTURE_TU100;
        gpuTuring.nvidiaArchInfo.architecture_id = NV_GPU_ARCHITECTURE_TU100;
        gpuTuring.nvidiaArchInfo.implementation = 0x104;

        // 3a: _nvngx.dll spoofed to Ada
        MockArchInfo archNvngx { NV_GPU_ARCHITECTURE_TU100, NV_GPU_ARCHITECTURE_TU100, 0x104, 0x104, 0, 0 };
        FilterArchInfoForCaller(&archNvngx, gpuTuring, "_nvngx.dll", true);
        assert(archNvngx.architecture == NV_GPU_ARCHITECTURE_AD100 && "_nvngx must be spoofed to Ada on Turing!");

        // 3b: nvngx_dlss.dll preserved as Turing
        MockArchInfo archDlss { NV_GPU_ARCHITECTURE_TU100, NV_GPU_ARCHITECTURE_TU100, 0x104, 0x104, 0, 0 };
        FilterArchInfoForCaller(&archDlss, gpuTuring, "nvngx_dlss.dll", true);
        assert(archDlss.architecture == NV_GPU_ARCHITECTURE_TU100 && "nvngx_dlss must receive real Turing arch!");

        // 3c: sl.dlss_g.dll spoofed to Ada
        MockArchInfo archSl { NV_GPU_ARCHITECTURE_TU100, NV_GPU_ARCHITECTURE_TU100, 0x104, 0x104, 0, 0 };
        FilterArchInfoForCaller(&archSl, gpuTuring, "sl.dlss_g.dll", true);
        assert(archSl.architecture == NV_GPU_ARCHITECTURE_AD100 && "sl.dlss_g must receive Ada arch on Turing!");

        std::printf(
            "  [PASS] Case 3: RTX 2080 Turing arch protected for DLSS SR, Ada spoofed for Streamline FG & _nvngx\n");
    }

    // Case 4: Native RTX 4090 (Ada 0x190)
    {
        MockGpuInfo gpuAda;
        gpuAda.isNvidia = true;
        gpuAda.nvidiaArchInfo.architecture = NV_GPU_ARCHITECTURE_AD100;
        gpuAda.nvidiaArchInfo.architecture_id = NV_GPU_ARCHITECTURE_AD100;

        MockArchInfo arch { NV_GPU_ARCHITECTURE_AD100, NV_GPU_ARCHITECTURE_AD100, 0x102, 0x102, 0, 0 };
        FilterArchInfoForCaller(&arch, gpuAda, "nvngx_dlss.dll", true);
        assert(arch.architecture == NV_GPU_ARCHITECTURE_AD100 && "Native Ada arch must remain Ada for nvngx_dlss!");

        std::printf("  [PASS] Case 4: Native Ada hardware remains Ada for all callers\n");
    }

    std::printf("=== All NvAPI Architecture Caller Isolation Unit Tests PASSED! ===\n");
    return 0;
}

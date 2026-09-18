#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <algorithm>

constexpr uint32_t NV_GPU_ARCHITECTURE_TU100 = 0x00000160;
constexpr uint32_t NV_GPU_ARCHITECTURE_GA100 = 0x00000170;
constexpr uint32_t NV_GPU_ARCHITECTURE_AD100 = 0x00000190;
constexpr uint32_t NV_GPU_ARCHITECTURE_GB200 = 0x000001b0;

struct MockArchInfo {
    uint32_t architecture = 0;
    uint32_t architecture_id = 0;
    uint32_t implementation = 0;
    uint32_t implementation_id = 0;
    uint32_t revision = 0;
    uint32_t revision_id = 0;
};

struct MockGpuInfo {
    bool isNvidia = true;
    MockArchInfo nvidiaArchInfo {};
};

// Emulates the architecture isolation logic in hkNvAPI_GPU_GetArchInfo
void FilterArchInfoForCaller(MockArchInfo* pGpuArchInfo, const MockGpuInfo& primaryGpu, const std::string& caller)
{
    if (!pGpuArchInfo)
        return;

    // When external mods (e.g. dlssg_sm86) spoof architecture to Ada (0x190) or Blackwell (0x1b0)
    // to enable Streamline DLSS-G, non-FG callers (like nvngx_dlss for Super Resolution or nvngx_dlssd for Ray Reconstruction)
    // must NOT see the spoofed architecture. Otherwise, DLSS SR/RR loads Blackwell/Ada-only cubin shaders
    // (e.g. DLTSS NW E5M3_SKIP FP8 kernels) that execute illegal instructions on Ampere/Turing hardware,
    // causing DXGI_ERROR_DEVICE_HUNG (0x887A0006) crashes on startup (e.g. in The Last of Us Part II).
    if (pGpuArchInfo->architecture >= NV_GPU_ARCHITECTURE_AD100)
    {
        const auto realArch = primaryGpu.nvidiaArchInfo.architecture_id != 0 ?
                                  primaryGpu.nvidiaArchInfo.architecture_id :
                                  primaryGpu.nvidiaArchInfo.architecture;

        if (primaryGpu.isNvidia && realArch != 0 && realArch < NV_GPU_ARCHITECTURE_AD100)
        {
            std::string callerLower = caller;
            std::transform(callerLower.begin(), callerLower.end(), callerLower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            const bool isFgCaller = (callerLower.find("sl.common") != std::string::npos ||
                                     callerLower.find("sl.dlss_g") != std::string::npos ||
                                     callerLower.find("sl.interposer") != std::string::npos ||
                                     callerLower.find("dlssg_sm86") != std::string::npos);

            if (!isFgCaller)
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
}

int main()
{
    std::printf("=== Running NvAPI Architecture Caller Isolation Unit Tests ===\n");

    // Case 1: RTX 3090 (Ampere 0x170) with dlssg_sm86 spoofed to Blackwell (0x1b0)
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

        // 1d: Caller Cyberpunk2077.exe MUST see Ampere (0x170)
        {
            MockArchInfo arch { NV_GPU_ARCHITECTURE_GB200, NV_GPU_ARCHITECTURE_GB200, 0x202, 0x202, 0, 0 };
            FilterArchInfoForCaller(&arch, gpuAmpere, "Cyberpunk2077.exe");
            assert(arch.architecture == NV_GPU_ARCHITECTURE_GA100 && "Cyberpunk2077 must receive real Ampere arch!");
        }

        // 1e: Caller sl.dlss_g.dll (Streamline DLSS-G) MUST see Blackwell (0x1b0)
        {
            MockArchInfo arch { NV_GPU_ARCHITECTURE_GB200, NV_GPU_ARCHITECTURE_GB200, 0x202, 0x202, 0, 0 };
            FilterArchInfoForCaller(&arch, gpuAmpere, "sl.dlss_g.dll");
            assert(arch.architecture == NV_GPU_ARCHITECTURE_GB200 && "sl.dlss_g must preserve spoofed Blackwell arch!");
        }

        // 1f: Caller sl.common.dll MUST see Blackwell (0x1b0)
        {
            MockArchInfo arch { NV_GPU_ARCHITECTURE_GB200, NV_GPU_ARCHITECTURE_GB200, 0x202, 0x202, 0, 0 };
            FilterArchInfoForCaller(&arch, gpuAmpere, "sl.common.dll");
            assert(arch.architecture == NV_GPU_ARCHITECTURE_GB200 && "sl.common must preserve spoofed Blackwell arch!");
        }

        // 1g: Caller dlssg_sm86.dll MUST see Blackwell (0x1b0)
        {
            MockArchInfo arch { NV_GPU_ARCHITECTURE_GB200, NV_GPU_ARCHITECTURE_GB200, 0x202, 0x202, 0, 0 };
            FilterArchInfoForCaller(&arch, gpuAmpere, "dlssg_sm86.dll");
            assert(arch.architecture == NV_GPU_ARCHITECTURE_GB200 && "dlssg_sm86 must preserve spoofed Blackwell arch!");
        }

        std::printf("  [PASS] Case 1: RTX 3090 Ampere arch protected for DLSS SR/RR, Blackwell preserved for Streamline FG\n");
    }

    // Case 2: RTX 2080 (Turing 0x160) with spoofed Ada (0x190)
    {
        MockGpuInfo gpuTuring;
        gpuTuring.isNvidia = true;
        gpuTuring.nvidiaArchInfo.architecture = NV_GPU_ARCHITECTURE_TU100;
        gpuTuring.nvidiaArchInfo.architecture_id = NV_GPU_ARCHITECTURE_TU100;
        gpuTuring.nvidiaArchInfo.implementation = 0x104;

        MockArchInfo arch { NV_GPU_ARCHITECTURE_AD100, NV_GPU_ARCHITECTURE_AD100, 0x102, 0x102, 0, 0 };
        FilterArchInfoForCaller(&arch, gpuTuring, "nvngx_dlss.dll");
        assert(arch.architecture == NV_GPU_ARCHITECTURE_TU100 && "nvngx_dlss must receive real Turing arch!");

        MockArchInfo archSl { NV_GPU_ARCHITECTURE_AD100, NV_GPU_ARCHITECTURE_AD100, 0x102, 0x102, 0, 0 };
        FilterArchInfoForCaller(&archSl, gpuTuring, "sl.dlss_g.dll");
        assert(archSl.architecture == NV_GPU_ARCHITECTURE_AD100 && "sl.dlss_g must preserve Ada arch on Turing!");

        std::printf("  [PASS] Case 2: RTX 2080 Turing arch protected for DLSS SR, Ada preserved for Streamline FG\n");
    }

    // Case 3: Native RTX 4090 (Ada 0x190)
    {
        MockGpuInfo gpuAda;
        gpuAda.isNvidia = true;
        gpuAda.nvidiaArchInfo.architecture = NV_GPU_ARCHITECTURE_AD100;
        gpuAda.nvidiaArchInfo.architecture_id = NV_GPU_ARCHITECTURE_AD100;

        MockArchInfo arch { NV_GPU_ARCHITECTURE_AD100, NV_GPU_ARCHITECTURE_AD100, 0x102, 0x102, 0, 0 };
        FilterArchInfoForCaller(&arch, gpuAda, "nvngx_dlss.dll");
        assert(arch.architecture == NV_GPU_ARCHITECTURE_AD100 && "Native Ada arch must remain Ada for nvngx_dlss!");

        std::printf("  [PASS] Case 3: Native Ada hardware remains Ada for all callers\n");
    }

    std::printf("=== All NvAPI Architecture Caller Isolation Unit Tests PASSED! ===\n");
    return 0;
}

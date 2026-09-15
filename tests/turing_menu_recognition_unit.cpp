#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Mock NV_GPU_ARCHITECTURE constants
constexpr uint32_t NV_GPU_ARCHITECTURE_GP100 = 0x00000130;
constexpr uint32_t NV_GPU_ARCHITECTURE_TU100 = 0x00000160;
constexpr uint32_t NV_GPU_ARCHITECTURE_GA100 = 0x00000170;
constexpr uint32_t NV_GPU_ARCHITECTURE_AD100 = 0x00000190;

enum class VendorId {
    Unknown,
    Nvidia,
    Amd,
    Intel
};

enum class FGOutput {
    NoFG,
    FSRFG,
    DLSSG,
    XeFG
};

enum class FGNvngxReplacement {
    None,
    Nukems,
    Arturs,
    FFX,
    Combo
};

struct MockGpu {
    VendorId vendorId = VendorId::Nvidia;
    uint32_t archId = NV_GPU_ARCHITECTURE_TU100;
    std::string name = "NVIDIA GeForce RTX 2070 SUPER";
};

inline bool IsTuringArch(uint32_t archId) {
    return (archId == 0x00000160) || ((archId & 0xFFF0) == 0x0160);
}

inline bool IsAmpereArch(uint32_t archId) {
    return (archId == 0x00000170) || ((archId & 0xFFF0) == 0x0170);
}

// Simulates the exact supportsDlssg logic in menu_common.cpp
bool EvaluateSupportsDlssg(const MockGpu& primaryGpu, bool ampereActive)
{
    const bool isNvidia = primaryGpu.vendorId == VendorId::Nvidia;
    const uint32_t archId = primaryGpu.archId;
    const bool isAdaOrNewer = isNvidia && (archId >= NV_GPU_ARCHITECTURE_AD100);
    const bool isTuringOrAmpere = isNvidia && (
        IsTuringArch(archId) ||
        IsAmpereArch(archId) ||
        primaryGpu.name.find("RTX 20") != std::string::npos ||
        primaryGpu.name.find("GTX 16") != std::string::npos ||
        primaryGpu.name.find("RTX 30") != std::string::npos ||
        primaryGpu.name.find("TITAN RTX") != std::string::npos ||
        primaryGpu.name.find("Turing") != std::string::npos ||
        primaryGpu.name.find("Ampere") != std::string::npos);

    return isAdaOrNewer || isTuringOrAmpere || ampereActive;
}

int main()
{
    printf("=== Running Turing Menu Recognition Unit Tests ===\n");

    // Case 1: RTX 2070 SUPER (Turing TU104)
    {
        MockGpu gpu;
        gpu.vendorId = VendorId::Nvidia;
        gpu.archId = 0x00000164; // TU104
        gpu.name = "NVIDIA GeForce RTX 2070 SUPER";

        bool supported = EvaluateSupportsDlssg(gpu, /*ampereActive=*/false);
        assert(supported == true);
        printf("  [PASS] Case 1: RTX 2070 SUPER recognized as supporting DLSSG\n");
    }

    // Case 2: TITAN RTX (Turing TU102) without 'RTX 20' in string
    {
        MockGpu gpu;
        gpu.vendorId = VendorId::Nvidia;
        gpu.archId = 0x00000162; // TU102
        gpu.name = "NVIDIA TITAN RTX";

        bool supported = EvaluateSupportsDlssg(gpu, /*ampereActive=*/false);
        assert(supported == true);
        printf("  [PASS] Case 2: TITAN RTX recognized as supporting DLSSG via arch and name\n");
    }

    // Case 3: RTX 2060 Mobile with unpopulated archId (0) relying on name
    {
        MockGpu gpu;
        gpu.vendorId = VendorId::Nvidia;
        gpu.archId = 0; // nvapi arch query failed
        gpu.name = "NVIDIA GeForce RTX 2060 Mobile";

        bool supported = EvaluateSupportsDlssg(gpu, /*ampereActive=*/false);
        assert(supported == true);
        printf("  [PASS] Case 3: RTX 2060 Mobile with unpopulated archId recognized via name\n");
    }

    // Case 4: RTX 3080 (Ampere GA102)
    {
        MockGpu gpu;
        gpu.vendorId = VendorId::Nvidia;
        gpu.archId = 0x00000170;
        gpu.name = "NVIDIA GeForce RTX 3080";

        bool supported = EvaluateSupportsDlssg(gpu, /*ampereActive=*/false);
        assert(supported == true);
        printf("  [PASS] Case 4: RTX 3080 recognized as supporting DLSSG\n");
    }

    // Case 5: GTX 1080 (Pascal GP104) - not DLSSG capable
    {
        MockGpu gpu;
        gpu.vendorId = VendorId::Nvidia;
        gpu.archId = NV_GPU_ARCHITECTURE_GP100;
        gpu.name = "NVIDIA GeForce GTX 1080";

        bool supported = EvaluateSupportsDlssg(gpu, /*ampereActive=*/false);
        assert(supported == false);
        printf("  [PASS] Case 5: Pascal GTX 1080 correctly identified as unsupported\n");
    }

    // Case 6: AMD Radeon RX 7900 XTX - not native Nvidia DLSSG
    {
        MockGpu gpu;
        gpu.vendorId = VendorId::Amd;
        gpu.archId = 0;
        gpu.name = "AMD Radeon RX 7900 XTX";

        bool supported = EvaluateSupportsDlssg(gpu, /*ampereActive=*/false);
        assert(supported == false);
        printf("  [PASS] Case 6: AMD GPU correctly identified as not supporting native DLSSG\n");
    }

    // Case 7: Explicit ampereActive=true overrides hardware restrictions
    {
        MockGpu gpu;
        gpu.vendorId = VendorId::Nvidia;
        gpu.archId = NV_GPU_ARCHITECTURE_GP100;
        gpu.name = "NVIDIA GeForce GTX 1080";

        bool supported = EvaluateSupportsDlssg(gpu, /*ampereActive=*/true);
        assert(supported == true);
        printf("  [PASS] Case 7: Explicit unlock flag successfully forces support\n");
    }

    printf("=== All Turing Menu Recognition Unit Tests PASSED! ===\n");
    return 0;
}

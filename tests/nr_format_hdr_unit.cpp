#include <cassert>
#include <cstdint>
#include <iostream>

#ifndef DXGI_FORMAT_DEFINED
enum DXGI_FORMAT : uint32_t
{
    DXGI_FORMAT_UNKNOWN = 0,
    DXGI_FORMAT_R32G32B32A32_TYPELESS = 1,
    DXGI_FORMAT_R32G32B32A32_FLOAT = 2,
    DXGI_FORMAT_R32G32B32_FLOAT = 6,
    DXGI_FORMAT_R16G16B16A16_TYPELESS = 9,
    DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
    DXGI_FORMAT_R10G10B10A2_UNORM = 24,
    DXGI_FORMAT_R11G11B10_FLOAT = 26,
    DXGI_FORMAT_R8G8B8A8_UNORM = 28,
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB = 29,
    DXGI_FORMAT_B8G8R8A8_UNORM = 87
};
#endif

namespace DlssNr
{
inline bool FormatCanHoldLinearHdr(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return true;
    default:
        return false;
    }
}
}

int main()
{
    std::cout << "[INFO] Running nr_format_hdr_unit..." << std::endl;

    // Linear HDR capable formats
    assert(DlssNr::FormatCanHoldLinearHdr(DXGI_FORMAT_R16G16B16A16_FLOAT));
    assert(DlssNr::FormatCanHoldLinearHdr(DXGI_FORMAT_R16G16B16A16_TYPELESS));
    assert(DlssNr::FormatCanHoldLinearHdr(DXGI_FORMAT_R32G32B32A32_FLOAT));
    assert(DlssNr::FormatCanHoldLinearHdr(DXGI_FORMAT_R32G32B32A32_TYPELESS));
    assert(DlssNr::FormatCanHoldLinearHdr(DXGI_FORMAT_R32G32B32_FLOAT));
    assert(DlssNr::FormatCanHoldLinearHdr(DXGI_FORMAT_R11G11B10_FLOAT));

    // Non-linear / display-referred SDR or clamped formats
    assert(!DlssNr::FormatCanHoldLinearHdr(DXGI_FORMAT_R8G8B8A8_UNORM));
    assert(!DlssNr::FormatCanHoldLinearHdr(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB));
    assert(!DlssNr::FormatCanHoldLinearHdr(DXGI_FORMAT_R10G10B10A2_UNORM));
    assert(!DlssNr::FormatCanHoldLinearHdr(DXGI_FORMAT_B8G8R8A8_UNORM));
    assert(!DlssNr::FormatCanHoldLinearHdr(DXGI_FORMAT_UNKNOWN));

    std::cout << "[PASS] All FormatCanHoldLinearHdr checks succeeded!" << std::endl;
    return 0;
}

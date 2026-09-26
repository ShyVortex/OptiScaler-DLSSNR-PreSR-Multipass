#pragma once

#include <d3d12.h>
#include <optional>

namespace DlssNr
{
struct ColorExtent
{
    unsigned int width;
    unsigned int height;
};

// Keep pre-SR admission aligned with the format used by the owned scratch and guide clones.
inline DXGI_FORMAT TypedNrResourceFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return format;
    }
}

// NGX reports the active image separately from the allocation. No preset names or standard
// resolutions belong here. Non-zero origins still need a separate guide/colour-offset integration.
inline std::optional<ColorExtent> PreSrColorExtent(const D3D12_RESOURCE_DESC& allocation, unsigned int renderWidth,
                                                   unsigned int renderHeight, unsigned int baseX = 0,
                                                   unsigned int baseY = 0)
{
    if (allocation.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || allocation.SampleDesc.Count != 1 ||
        allocation.DepthOrArraySize != 1 || allocation.Width == 0 ||
        allocation.Width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || allocation.Height == 0 ||
        allocation.Height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || baseX != 0 || baseY != 0)
        return std::nullopt;

    if (renderWidth == 0 && renderHeight == 0)
        return ColorExtent { (unsigned int) allocation.Width, allocation.Height };

    if (renderWidth == 0 || renderHeight == 0 || renderWidth > allocation.Width || renderHeight > allocation.Height)
        return std::nullopt;

    return ColorExtent { renderWidth, renderHeight };
}

inline bool PreSrColorCanUseScratch(const D3D12_RESOURCE_DESC& allocation, unsigned int renderWidth,
                                    unsigned int renderHeight)
{
    if (!PreSrColorExtent(allocation, renderWidth, renderHeight))
        return false;
    // A changed format fails the existing source/scratch equality check. Keep the previous post-SR
    // route for newly admitted multi-mip inputs instead of silently dropping NR for the whole frame.
    return allocation.MipLevels == 1 || TypedNrResourceFormat(allocation.Format) == allocation.Format;
}

// Pre-SR NR only copies mip 0. Other mips may have independent game-owned states.
inline void TransitionActiveColor(ID3D12GraphicsCommandList* commands, ID3D12Resource* color,
                                  D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after)
        return;
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = { color, 0, before, after };
    commands->ResourceBarrier(1, &barrier);
}

// Both resources must be in COPY_SOURCE/COPY_DEST respectively. An explicit box is essential:
// a whole-resource copy either has mismatched dimensions or overwrites the game's padding.
inline void CopyActiveColor(ID3D12GraphicsCommandList* commands, ID3D12Resource* destination, ID3D12Resource* source,
                            ColorExtent active)
{
    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = source;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = destination;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box { 0, 0, 0, active.width, active.height, 1 };
    commands->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
}
} // namespace DlssNr

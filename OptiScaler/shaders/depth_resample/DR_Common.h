#pragma once

#include <cstdint>
#include <string>

// Nearest-sample depth resample for frame generation. Some games hand display-resolution motion vectors with
// render-resolution depth, and XeFG requires both at the same resolution (DLSS Unlocked #57). Depth is copied to the
// motion-vector size with point sampling: values are never filtered or rescaled, so edges keep real depths.
struct alignas(256) DRConstants
{
    uint32_t SourceWidth;
    uint32_t SourceHeight;
    uint32_t TargetWidth;
    uint32_t TargetHeight;
};

// Source texel whose area contains the centre of a target texel. Mirrors SourceTexel() in drShaderCode exactly.
constexpr uint32_t DRSourceTexel(uint32_t target, uint32_t targetSize, uint32_t sourceSize)
{
    if (targetSize == 0 || sourceSize == 0)
        return 0;
    const uint32_t texel = ((2u * target + 1u) * sourceSize) / (2u * targetSize);
    return texel < sourceSize ? texel : sourceSize - 1u;
}

inline static const std::string drShaderCode = R"(
cbuffer Params : register(b0)
{
    uint SourceWidth;
    uint SourceHeight;
    uint TargetWidth;
    uint TargetHeight;
};

Texture2D<float> SourceTexture : register(t0);
RWTexture2D<float> DestinationTexture : register(u0);

// Source texel whose area contains the centre of a target texel (nearest sample, no filtering).
uint SourceTexel(uint target, uint targetSize, uint sourceSize)
{
    return min(((2u * target + 1u) * sourceSize) / (2u * targetSize), sourceSize - 1u);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= TargetWidth || id.y >= TargetHeight)
        return;

    uint2 source = uint2(SourceTexel(id.x, TargetWidth, SourceWidth), SourceTexel(id.y, TargetHeight, SourceHeight));
    DestinationTexture[id.xy] = SourceTexture.Load(int3(source, 0));
}
)";

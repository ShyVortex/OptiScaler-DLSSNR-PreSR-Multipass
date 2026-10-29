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

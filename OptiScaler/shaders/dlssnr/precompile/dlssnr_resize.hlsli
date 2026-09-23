// Optional subnative reconstruction: resize relative lighting and chromaticity,
// not an RGB difference between differently filtered images. As in Display Filter,
// compose the reconstructed answer against the full-resolution proxy only once.
float3 NrEncodeResizeField(float3 field)
{
    return 0.5 + 0.5 * field / (1 + abs(field));
}

float3 NrDecodeResizeField(float3 carrier)
{
    float3 d = clamp(2 * SanitizeFinite3(carrier, 0.5) - 1, -0.999, 0.999);
    return d / (1 - abs(d));
}

float2 NrResizeChroma(float3 colour)
{
    float y = dot(colour, kLuma);
    return y > 1e-6 ? colour.rb / y : float2(1, 1);
}

float3 NrResizeLinear(float3 colour)
{
    return gPassthrough != 0 ? colour : SrgbToLinear(colour);
}

float3 NrPairedResizeField(int2 pixel, int2 size)
{
    float3 source = NrResizeLinear(gSource.Load(int3(pixel, 0)).rgb);
    float3 model = NrResizeLinear(gModel.Load(int3(pixel, 0)).rgb);
    float sourceY = dot(source, kLuma), modelY = dot(model, kLuma);
    if (sourceY < 1.0 / 1024.0)
    {
        // Infer undefined near-black gain from paired neighbouring samples.
        sourceY = modelY = 0;
        [unroll] for (int y = -1; y <= 1; ++y) [unroll] for (int x = -1; x <= 1; ++x)
        {
            int2 q = clamp(pixel + int2(x, y), int2(0, 0), size - 1);
            sourceY += dot(NrResizeLinear(gSource.Load(int3(q, 0)).rgb), kLuma);
            modelY += dot(NrResizeLinear(gModel.Load(int3(q, 0)).rgb), kLuma);
        }
    }
    return SanitizeFinite3(float3((modelY - sourceY) / max(sourceY, 1.0 / 1024.0),
                                  NrResizeChroma(model) - NrResizeChroma(source)), 0);
}

float3 NrSampleResizeField(float2 uv, bool encoded)
{
    uint w, h; gSource.GetDimensions(w, h);
    float2 p = uv * float2(w, h) - 0.5;
    int2 base = int2(floor(p));
    float2 f = frac(p);
    float3 field = 0;
    [unroll] for (int y = 0; y < 2; ++y) [unroll] for (int x = 0; x < 2; ++x)
    {
        int2 q = clamp(base + int2(x, y), int2(0, 0), int2(w - 1, h - 1));
        // Decode before filtering: the bounded DLSS carrier is nonlinear.
        float3 tap = encoded ? NrDecodeResizeField(gSource.Load(int3(q, 0)).rgb)
                             : NrPairedResizeField(q, int2(w, h));
        field += tap * (x ? f.x : 1 - f.x) * (y ? f.y : 1 - f.y);
    }
    return field;
}

float3 NrRestoreResizeField(float3 base, float3 field)
{
    float y = max(0, dot(base, kLuma) * (1 + field.x));
    float2 chroma = field.yz + NrResizeChroma(base);
    float r = chroma.x * y, b = chroma.y * y;
    return float3(r, (y - kLuma.r * r - kLuma.b * b) / kLuma.g, b);
}

float3 NrBoundResizeCarrier(float2 uv, float3 candidate)
{
    uint w, h; gSource.GetDimensions(w, h);
    int2 base = int2(floor(uv * float2(w, h) - 0.5));
    float3 lo = 1, hi = 0;
    [unroll] for (int y = 0; y < 2; ++y) [unroll] for (int x = 0; x < 2; ++x)
    {
        int2 q = clamp(base + int2(x, y), int2(0, 0), int2(w - 1, h - 1));
        float3 tap = SanitizeFinite3(gSource.Load(int3(q, 0)).rgb, 0.5);
        lo = min(lo, tap); hi = max(hi, tap);
    }
    float3 reference = clamp(SanitizeFinite3(gSource.SampleLevel(gLinear, uv, 0).rgb, 0.5), lo, hi);
    candidate = SanitizeFinite3(candidate, reference);
    float3 delta = candidate - reference;
    float amount = 1;
    [unroll] for (int ch = 0; ch < 3; ++ch)
    {
        if (delta[ch] > 0) amount = min(amount, (hi[ch] - reference[ch]) / delta[ch]);
        else if (delta[ch] < 0) amount = min(amount, (lo[ch] - reference[ch]) / delta[ch]);
    }
    return clamp(reference + saturate(amount) * delta, lo, hi);
}

float3 NrReconstructModel(float3 base, float2 uv, float3 carrier)
{
    bool dlss = gTransfer == 4;
    float3 field = dlss ? NrDecodeResizeField(NrBoundResizeCarrier(uv, carrier)) : NrSampleResizeField(uv, false);
    float3 result = NrRestoreResizeField(base, field);
    if (dlss && gPassthrough == 0 && (gReversibleMode == 2 || gReversibleMode == 4))
    {
        float3 reference = NrRestoreResizeField(base, NrSampleResizeField(uv, true));
        // Reject a DLSS-only crossing of the HDR inverse's white pole.
        if (any(result >= 1) && all(reference < 1)) result = reference;
    }
    // Fit chroma around the requested luminance, preserving the lighting edit.
    float y = saturate(dot(result, kLuma));
    float3 delta = result - y;
    float amount = 1;
    [unroll] for (int ch = 0; ch < 3; ++ch)
    {
        if (delta[ch] > 0) amount = min(amount, (1 - y) / delta[ch]);
        else if (delta[ch] < 0) amount = min(amount, -y / delta[ch]);
    }
    return saturate(y + saturate(amount) * delta);
}

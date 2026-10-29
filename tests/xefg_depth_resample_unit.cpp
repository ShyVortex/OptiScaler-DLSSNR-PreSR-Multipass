#include "../OptiScaler/shaders/depth_resample/DR_Common.h"
#include "../OptiScaler/inputs/FG/FgDepthResample.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

// DLSS Unlocked #57 follow-up: XeFG needs depth at the resolution of display-resolution motion vectors. DR_Dx12 copies
// depth with nearest sampling. This test runs the production mapping (DRSourceTexel, the C++ twin of the HLSL) and
// checks that the shader source, the .hlsl file, the generated blob and the Linux build script stay consistent.
//
// Validates:
// 1. Equal sizes are an identity.
// 2. When upsampling, corners map exactly; for every size the mapping picks the source texel containing each
//    target texel's centre.
// 3. Upsampling 1279x720 -> 1920x1080 covers every source texel, monotonically, with no gaps.
// 4. Output values are bit copies of source values (no filtering), including non-finite and negative-zero depths.
// 5. The 32-bit HLSL arithmetic cannot overflow for sizes up to 16384.
// 6. drShaderCode, precompiled/DR.hlsl and the C++ mapping use the same formula; the blob is signed DXIL.
// 7. build_precompiled_shader.sh exists, is executable and uses the .bat's DXC flags plus dxv validation.
// 8. FgDepthResample::Needed: only XeFG output, display-resolution motion vectors and different sizes resample.
// 9. Both upscaler paths (DX12, DX11 w/ DX12) resample render -> target size, tag the buffer at the target size,
//    restore the depth state, and fall back to the original depth tag when the resample does not happen.
// 10. The resample comes after the opt-in depth-scale path and before the plain depth tag in both paths.

static std::string ReadFile(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        std::fprintf(stderr, "cannot open %s (run from the repository root)\n", path);
        std::exit(1);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Collapses whitespace runs, so source checks do not depend on clang-format's line breaks.
static std::string Squash(const std::string& text)
{
    std::string out;
    bool space = false;
    for (char c : text)
    {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
            space = true;
        else
        {
            if (space && !out.empty())
                out.push_back(' ');
            space = false;
            out.push_back(c);
        }
    }
    return out;
}

static std::vector<float> Resample(const std::vector<float>& src, uint32_t sw, uint32_t sh, uint32_t tw, uint32_t th)
{
    std::vector<float> out(size_t(tw) * th);
    for (uint32_t y = 0; y < th; ++y)
        for (uint32_t x = 0; x < tw; ++x)
            out[size_t(y) * tw + x] = src[size_t(DRSourceTexel(y, th, sh)) * sw + DRSourceTexel(x, tw, sw)];
    return out;
}

int main()
{
    // Test 1
    for (uint32_t size : { 1u, 2u, 720u, 1279u, 1920u })
        for (uint32_t t = 0; t < size; ++t)
            assert(DRSourceTexel(t, size, size) == t);
    std::puts("Test 1 PASS: equal sizes are an identity");

    // Test 2
    for (auto [s, t] : { std::pair { 1279u, 1920u }, std::pair { 720u, 1080u }, std::pair { 1920u, 1279u },
                         std::pair { 3u, 7u }, std::pair { 7u, 3u } })
    {
        if (t >= s) // upsampling (the XeFG case) or equal size: corners land on corners
        {
            assert(DRSourceTexel(0, t, s) == 0);
            assert(DRSourceTexel(t - 1, t, s) == s - 1);
        }
        for (uint32_t x = 0; x < t; ++x)
        {
            // Centre of target texel x, in source units: (x + 0.5) * s / t. The chosen source texel contains it.
            const uint32_t src = DRSourceTexel(x, t, s);
            const uint64_t centre2 = (2ull * x + 1ull) * s; // 2 * t * centre
            assert(2ull * t * src <= centre2 && centre2 < 2ull * t * (src + 1ull));
        }
    }
    std::puts("Test 2 PASS: corners exact when upsampling, nearest texel contains each target centre");

    // Test 3
    for (auto [s, t] : { std::pair { 1279u, 1920u }, std::pair { 720u, 1080u }, std::pair { 545u, 817u } })
    {
        uint32_t previous = 0;
        std::vector<bool> used(s, false);
        for (uint32_t x = 0; x < t; ++x)
        {
            const uint32_t src = DRSourceTexel(x, t, s);
            assert(src >= previous && src - previous <= 1 && "monotonic, one source step at most when upsampling");
            used[src] = true;
            previous = src;
        }
        for (bool u : used)
            assert(u && "every source texel reaches the output");
    }
    std::puts("Test 3 PASS: 1279x720 -> 1920x1080 covers every source texel without gaps");

    // Test 4
    {
        const uint32_t sw = 5, sh = 3, tw = 8, th = 5;
        std::vector<float> src(size_t(sw) * sh);
        for (size_t i = 0; i < src.size(); ++i)
            src[i] = 0.1f * float(i) + 1e-7f;
        src[2] = -0.0f;
        src[4] = 1.0f;
        uint32_t nanBits = 0x7fc00001u;
        std::memcpy(&src[6], &nanBits, sizeof(float));
        const auto out = Resample(src, sw, sh, tw, th);
        for (uint32_t y = 0; y < th; ++y)
            for (uint32_t x = 0; x < tw; ++x)
            {
                const float a = out[size_t(y) * tw + x];
                const float b = src[size_t(DRSourceTexel(y, th, sh)) * sw + DRSourceTexel(x, tw, sw)];
                assert(std::memcmp(&a, &b, sizeof(float)) == 0 && "bit-identical copy, no filtering");
            }
    }
    std::puts("Test 4 PASS: output texels are bit copies of source texels");

    // Test 5
    for (uint32_t s : { 1u, 4096u, 8192u, 16384u })
        for (uint32_t t : { 1u, 4096u, 8192u, 16384u })
            for (uint32_t x : { 0u, t / 2, t - 1 })
            {
                const uint64_t wide = ((2ull * x + 1ull) * s) / (2ull * t);
                const uint32_t expected = uint32_t(wide < s ? wide : s - 1);
                assert(DRSourceTexel(x, t, s) == expected && "32-bit arithmetic matches 64-bit up to 16384");
            }
    assert(DRSourceTexel(0, 0, 10) == 0 && DRSourceTexel(3, 10, 0) == 0);
    std::puts("Test 5 PASS: no overflow up to 16384, zero sizes guarded");

    // Test 6
    const std::string header = ReadFile("OptiScaler/shaders/depth_resample/DR_Common.h");
    const std::string hlsl = ReadFile("OptiScaler/shaders/depth_resample/precompiled/DR.hlsl");
    assert(drShaderCode.substr(drShaderCode.find_first_not_of('\n')) == hlsl && "DR.hlsl must equal drShaderCode");
    const std::string formula = "((2u * target + 1u) * sourceSize) / (2u * targetSize)";
    assert(hlsl.find(formula) != std::string::npos && hlsl.find("sourceSize - 1u") != std::string::npos);
    assert(header.find("const uint32_t texel = " + formula + ";") != std::string::npos && "C++ twin, same formula");
    assert(hlsl.find("Sample(") == std::string::npos && hlsl.find("SampleLevel(") == std::string::npos &&
           "Load only: never filtered");
    const std::string cso = ReadFile("OptiScaler/shaders/depth_resample/precompiled/DR_Shader.cso");
    assert(cso.size() > 32 && cso.compare(0, 4, "DXBC") == 0 && cso.find("DXIL") != std::string::npos);
    bool signedBlob = false;
    for (int i = 4; i < 20; ++i)
        signedBlob |= cso[i] != 0;
    assert(signedBlob && "validator digest present (dxv / dxil.dll signed)");
    const std::string blobHeader = ReadFile("OptiScaler/shaders/depth_resample/precompiled/DR_Shader.h");
    assert(blobHeader.find("inline static const unsigned char DR_cso[] = {") != std::string::npos);
    std::puts("Test 6 PASS: shader source, .hlsl, C++ twin and signed blob agree");

    // Test 7
    const char* scriptPath = "OptiScaler/shaders/shader_tools/build_precompiled_shader.sh";
    struct stat info {};
    assert(stat(scriptPath, &info) == 0 && (info.st_mode & S_IXUSR) && "the Linux build script is executable");
    const std::string script = ReadFile(scriptPath);
    const std::string bat = ReadFile("OptiScaler/shaders/shader_tools/build_precompiled_shader.bat");
    const std::string flags = "-T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect";
    assert(bat.find(flags) != std::string::npos && script.find(flags) != std::string::npos &&
           "same DXC flags as the Windows script");
    assert(script.find("dxv.exe") != std::string::npos && script.find("Validation succeeded") != std::string::npos);
    assert(script.find("$toolsDir/fxc.exe") == std::string::npos && "never invokes FXC under Wine");
    std::puts("Test 7 PASS: Linux build script mirrors the .bat DXC step and validates");

    // Test 8
    using FgDepthResample::Needed;
    assert(Needed(true, false, 1279, 720, 1920, 1080) && "Silent Hill Townfall: display MV, render depth -> resample");
    assert(!Needed(true, true, 1279, 720, 1920, 1080) && "low-res MV: sizes already match");
    assert(!Needed(true, false, 1920, 1080, 1920, 1080) && "DLAA / native: nothing to do");
    assert(!Needed(false, false, 1279, 720, 1920, 1080) && "other FG outputs keep their behaviour");
    assert(!Needed(true, false, 0, 720, 1920, 1080) && !Needed(true, false, 1279, 720, 1920, 0));
    assert(Needed(true, false, 1279, 545, 1920, 817) && "letterboxed sizes as well");
    std::puts("Test 8 PASS: resample only for XeFG with display-resolution motion vectors and mismatched sizes");

    // Test 9 / 10
    for (const char* path :
         { "OptiScaler/inputs/FG/Upscaler_Inputs_Dx12.cpp", "OptiScaler/inputs/FG/Upscaler_Inputs_Dx11wDx12.cpp" })
    {
        const std::string src = Squash(ReadFile(path));
        assert(src.find("#include \"FgDepthResample.h\"") != std::string::npos);
        assert(src.find("#include \"shaders/depth_resample/DR_Dx12.h\"") != std::string::npos);
        const auto decision = src.find("FgDepthResample::Needed(State::Instance().activeFgOutput == FGOutput::XeFG, "
                                       "feature->LowResMV(),");
        assert(decision != std::string::npos);
        const auto block = src.substr(decision, src.find("if (!done)", decision) - decision);
        assert(block.find("feature->RenderWidth(), feature->RenderHeight()") != std::string::npos &&
               block.find("feature->TargetWidth(), feature->TargetHeight()") != std::string::npos);
        const auto create = block.find("->CreateBufferResource(");
        const auto toRead = block.find("D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);");
        const auto dispatch = block.find("->Dispatch(");
        const auto restore = block.find("D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, depthState);");
        const auto tag = block.find("setResource.width = feature->TargetWidth();");
        assert(create != std::string::npos && toRead != std::string::npos && dispatch != std::string::npos &&
               restore != std::string::npos && tag != std::string::npos);
        assert(create < toRead && toRead < dispatch && dispatch < restore && restore < tag &&
               "create, make readable, dispatch, restore the game's state, then tag");
        assert(block.find("setResource.height = feature->TargetHeight();") != std::string::npos &&
               block.find("->Buffer();") != std::string::npos && block.find("done = true;") != std::string::npos);

        const auto depthScale = src.find("FGEnableDepthScale.value_or_default()");
        const auto plainTag = src.find("if (!done)", decision);
        assert(depthScale != std::string::npos && depthScale < decision && decision < plainTag &&
               "after the opt-in depth scale, before the original depth tag");
        assert(src.find("setResource.resource = paramDepth;", plainTag) != std::string::npos &&
               "fallback keeps tagging the game's depth");
    }
    std::puts("Test 9 PASS: both upscaler paths resample render -> target and restore the depth state");
    std::puts("Test 10 PASS: resample sits between the depth-scale path and the original depth tag");

    std::puts("All XeFG depth resample tests passed (10/10).");
    return 0;
}

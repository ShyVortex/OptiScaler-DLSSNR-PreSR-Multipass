// MatchGuides / RenderMotionScale: the shared model motion-vector scale (DlssNrModelMotionScale) and its
// DX12 and Vulkan wiring, including the FF7 Rebirth rule that format bounces never touch the matched guides.
// Build and run from the repo root: g++ -std=c++20 tests/nr_motion_scale_unit.cpp && ./a.out
#include "../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef SOURCE_DIR
#define SOURCE_DIR "."
#endif

static int failures = 0;

static void Check(bool ok, const char* name)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    failures += ok ? 0 : 1;
}

static std::string Read(const char* path)
{
    std::ifstream file(std::string(SOURCE_DIR) + "/" + path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

static std::string Between(const std::string& text, const char* from, const char* to)
{
    const auto begin = text.find(from);
    const auto end = begin == std::string::npos ? std::string::npos : text.find(to, begin);
    return end == std::string::npos ? std::string() : text.substr(begin, end - begin);
}

// Pre-SR at 50% working scale in a 2x-upscaling game: NR frame = render = 1920x1080, output 3840x2160.
static DlssNrMotionScaleInput Base()
{
    DlssNrMotionScaleInput in;
    in.workW = 960, in.workH = 540;
    in.guideW = 1920, in.guideH = 1080;
    in.frameW = 1920, in.frameH = 1080;
    in.renderW = 1920, in.renderH = 1080;
    in.outputW = 3840, in.outputH = 2160;
    return in;
}

int main()
{
    std::printf("NR model motion-vector scale unit tests\n");

    {
        auto in = Base();
        in.renderMotionScale = false;
        in.matchedGuides = true;
        const auto s = DlssNrModelMotionScale(in);
        Check(s.x == 0.5f && s.y == 0.5f && s.referenceW == 1920 && s.textureW == 960,
              "Test 1: RenderMotionScale=false keeps the old working size / frame size rule");
    }
    {
        auto in = Base();
        in.lowResVectors = true;
        in.matchedGuides = true;
        const auto s = DlssNrModelMotionScale(in);
        Check(s.x == 0.5f && s.textureW == 960 && s.referenceW == 1920,
              "Test 2: matched guides, low-res vectors: working size / render size");
    }
    {
        auto in = Base();
        in.lowResVectors = true;
        in.workW = 1920, in.workH = 1080;
        const auto s = DlssNrModelMotionScale(in);
        Check(s.x == 1.0f && s.y == 1.0f && s.textureW == 1920,
              "Test 3: 100% with low-res vectors keeps the game's own scale (Onimusha 2x regression)");
    }
    {
        auto in = Base();
        in.matchedGuides = true;
        const auto s = DlssNrModelMotionScale(in);
        Check(s.x == 0.25f && s.referenceW == 3840,
              "Test 4: output-resolution vectors are measured against the output");
    }
    {
        auto in = Base();
        in.lowResVectors = true;
        in.renderW = 0;
        in.outputH = 0;
        const auto low = DlssNrModelMotionScale(in);
        in.lowResVectors = false;
        const auto out = DlssNrModelMotionScale(in);
        Check(low.x == 0.5f && low.referenceW == 1920 && low.textureW == 960 && out.y == 0.5f &&
                  out.referenceH == 1080 && out.x == 0.5f,
              "Test 5: an unknown reference size falls back to the frame-size rule on that axis");
    }

    const std::string run = Read("OptiScaler/shaders/dlssnr/DlssNr_Dx12_Run.cpp");
    const auto resample = run.find("resize.Mode = DlssNrMode_ResizePrivateGuides;");
    const auto scale = run.find("DlssNrModelMotionScale(");
    const auto modelGuides = run.find("modelFrame.guides = (spatial || matchedGuides)");
    const auto restore = run.find("Barrier(cmdList, nr.depthSmall, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE");
    Check(resample != std::string::npos && resample < scale && scale < modelGuides && modelGuides < restore &&
              run.find("cfg.DlssNrMatchGuides.value_or_default()") < resample &&
              run.find("mvToWorkX = width != 0") == std::string::npos,
          "Test 6: DX12 resamples the guides, scales vectors with the shared rule and restores UAV after use");

    const std::string vk = Read("OptiScaler/dlssnr/DlssNrFeature_Vk.cpp");
    Check(vk.find("DlssNrModelMotionScale(") != std::string::npos &&
              vk.find("Mirrors DlssNr_Dx12_Run.cpp") == std::string::npos,
          "Test 7: Vulkan uses the same motion-scale rule");

    // FF7 Rebirth (NR-FF7-REBIRTH.md): the format-change swap stays zero-allocation and never touches the
    // matched guides; they retire only on resolution/placement changes and full release.
    const std::string resources = Read("OptiScaler/shaders/dlssnr/DlssNr_Dx12_Resources.cpp");
    const std::string swap = Between(resources, "::ReleaseSurfacesIfFormatChanged(", "\n}\n");
    const std::string release = Between(resources, "::ReleaseResources(", "\n}\n");
    const std::string models = Read("OptiScaler/shaders/dlssnr/DlssNr_Dx12_Models.cpp");
    const std::string park = Between(models, "if (resolutionChanged || placementChanged)\n", "nr.altSurfaces = {};");
    Check(!swap.empty() && swap.find("depthSmall") == std::string::npos &&
              swap.find("motionSmall") == std::string::npos &&
              release.find("&nr.depthSmall, &nr.motionSmall") != std::string::npos &&
              park.find("ParkNrResource(nr.depthSmall);") != std::string::npos &&
              park.find("ParkNrResource(nr.motionSmall);") != std::string::npos,
          "Test 8: matched guides stay out of the format swap and retire with resolution changes and release");

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

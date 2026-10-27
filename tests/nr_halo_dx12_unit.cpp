// Halo suppression on DX12: the shared DlssNr_Common rules, and the DX12 wiring that feeds them
// (Encode quality constants, the Run dispatch, passClamp allocation without a feature rebuild).
// Build and run from the repo root: g++ -std=c++20 tests/nr_halo_dx12_unit.cpp && ./a.out
#include "../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

#ifndef SOURCE_DIR
#define SOURCE_DIR "."
#endif

template <typename T> struct Option
{
    T value;
    T value_or_default() const { return value; }
};

struct FakeConfig
{
    Option<float> DlssNrHaloSuppression { 0.0f };
    Option<float> DlssNrHaloBrightSuppression { 0.0f };
    Option<float> DlssNrHaloRadius { 2.0f };
    Option<bool> DlssNrHistoryRejection { false };
    Option<uint32_t> DlssNrDebugView { 0u };
};

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

int main()
{
    std::printf("NR halo suppression DX12 unit tests\n");
    const float nan = std::numeric_limits<float>::quiet_NaN();

    {
        FakeConfig off, dark, bright, bad;
        dark.DlssNrHaloSuppression.value = 0.5f;
        bright.DlssNrHaloBrightSuppression.value = 0.25f;
        bad.DlssNrHaloSuppression.value = nan;
        bad.DlssNrHaloBrightSuppression.value = -1.0f;
        Check(!DlssNr_Common::HaloEnabled(off) && DlssNr_Common::HaloEnabled(dark) &&
                  DlssNr_Common::HaloEnabled(bright) && !DlssNr_Common::HaloEnabled(bad),
              "Test 1: either strength above zero enables the filter; defaults, NaN and negatives do not");
    }
    {
        FakeConfig on;
        on.DlssNrHaloSuppression.value = 1.0f;
        FakeConfig raw = on;
        raw.DlssNrDebugView.value = 2;
        Check(DlssNr_Common::HaloRuns(on) && !DlssNr_Common::HaloRuns(raw),
              "Test 2: the pass is skipped in the raw model-output debug view");
    }
    {
        FakeConfig off, on;
        on.DlssNrHaloSuppression.value = 0.1f;
        Check(!DlssNr_Common::NeedsPassClamp(1, off) && DlssNr_Common::NeedsPassClamp(1, on) &&
                  DlssNr_Common::NeedsPassClamp(2, off),
              "Test 3: one pass needs passClamp only for the halo filter; multipass always does");
    }
    {
        FakeConfig cfg;
        cfg.DlssNrHaloSuppression.value = 3.0f;
        cfg.DlssNrHaloBrightSuppression.value = nan;
        cfg.DlssNrHaloRadius.value = 9.0f;
        cfg.DlssNrHistoryRejection.value = true;
        DlssNrConstants c {};
        DlssNr_Common::SetQualityConstants(c, cfg);
        FakeConfig badRadius;
        badRadius.DlssNrHaloRadius.value = nan;
        DlssNrConstants d {};
        DlssNr_Common::SetQualityConstants(d, badRadius);
        Check(c.HaloSuppression == 1.0f && c.HaloBrightSuppression == 0.0f && c.HaloRadius == 4.0f &&
                  c.HistoryRejection == 1 && d.HaloRadius == 2.0f,
              "Test 4: quality constants are clamped to the shader's ranges, with NaN falling back");
    }

    const std::string encode = Read("OptiScaler/shaders/dlssnr/DlssNr_Dx12_Encode.cpp");
    Check(encode.find("DlssNr_Common::SetQualityConstants(resolveParams, cfg);") != std::string::npos,
          "Test 5: DX12 resolve constants carry the quality settings");

    const std::string run = Read("OptiScaler/shaders/dlssnr/DlssNr_Dx12_Run.cpp");
    const auto halo = run.find("halo.Mode = DlssNrMode_SuppressHalo;");
    Check(halo != std::string::npos && run.find("DlssNr_Common::HaloRuns(cfg)") < halo &&
              run.find("ngxTime->End(cmdList);") < halo && run.find("nr.reset = clampFailed") > halo,
          "Test 6: DX12 Run dispatches the halo pass after the model chain, before composition");

    // FF7 Rebirth (NR-FF7-REBIRTH.md): allocating passClamp must never rebuild models or stop NR.
    const std::string models = Read("OptiScaler/shaders/dlssnr/DlssNr_Dx12_Models.cpp");
    const auto begin = models.find("DlssNr_Common::NeedsPassClamp(requestedPasses, cfg)");
    const auto end = models.find("nr.colorSmall == nullptr", begin);
    const std::string block =
        begin != std::string::npos && end != std::string::npos ? models.substr(begin, end - begin) : std::string();
    Check(!block.empty() && block.find("RetryAfterFailure") == std::string::npos &&
              block.find("modelRunning") == std::string::npos && block.find("Release()") == std::string::npos &&
              models.find("ReleaseSurfacesIfFormatChanged(") < begin,
          "Test 7: passClamp allocation runs after the format swap and never rebuilds the models");

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

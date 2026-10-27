// RR residual "Reject stale history" on DX12: the production history rules from DlssNr_ResidualHistory.h and the
// constant slots dlssnr_residual.hlsl reads them from.
// Build and run from the repo root: g++ -std=c++20 tests/nr_history_rejection_unit.cpp && ./a.out
#include "../OptiScaler/shaders/dlssnr/DlssNr_ResidualHistory.h"
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

#ifndef SOURCE_DIR
#define SOURCE_DIR "."
#endif

namespace History = DlssNr::ResidualHistory;

static int failures = 0;

static void Check(bool ok, const char* name)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    failures += ok ? 0 : 1;
}

static DlssNrConstants Frame(History::State& history, bool rejection, float preExposure, bool valid = true,
                             bool reset = false)
{
    DlssNrConstants c {};
    History::Fill(c, history, rejection, preExposure, valid, reset);
    History::Record(history, c);
    return c;
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
    std::printf("NR residual history rejection unit tests\n");

    {
        History::State h;
        const auto c = Frame(h, false, 1.0f);
        Check(c.HistoryRejection == 0 && c.ResidualHistoryValid == 1 && c.PreExposure == 1.0f,
              "Test 1: rejection off keeps the original history behaviour");
    }
    {
        History::State h;
        Check(Frame(h, false, 1.0f, true, true).ResidualHistoryValid == 0 &&
                  Frame(h, false, 1.0f, false).ResidualHistoryValid == 0,
              "Test 2: a reset or an invalid accumulation still drops history");
    }
    {
        History::State h;
        Frame(h, false, 1.0f);
        const auto on = Frame(h, true, 1.0f);
        const auto next = Frame(h, true, 1.0f);
        Check(on.HistoryRejection == 1 && on.ResidualHistoryValid == 0 && next.ResidualHistoryValid == 1,
              "Test 3: turning rejection on drops history for one frame only");
    }
    {
        History::State h;
        Frame(h, true, 1.0f);
        Check(Frame(h, false, 1.0f).ResidualHistoryValid == 0, "Test 4: turning rejection off drops history");
    }
    {
        History::State h;
        Frame(h, true, 2.0f);
        const auto c = Frame(h, true, 4.0f);
        Check(c.ResidualScale == 4.0f && c.PreExposure == 2.0f && h.preExposure == 4.0f,
              "Test 5: history is rescaled by current / previous pre-exposure");
    }
    {
        History::State h;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const auto a = Frame(h, true, nan);
        const auto b = Frame(h, true, 0.0f);
        const auto c = Frame(h, true, 3.0f);
        Check(a.ResidualScale == 1.0f && b.ResidualScale == 1.0f && std::isfinite(b.PreExposure) &&
                  c.PreExposure == 3.0f,
              "Test 6: NaN and zero pre-exposure fall back to 1 and never poison the ratio");
    }

    // dlssnr_residual.hlsl reads PreExposure as gHistoryExposureRatio and ResidualScale as gResidualScale.
    const std::string hlsl = Read("OptiScaler/shaders/dlssnr/precompile/dlssnr_residual.hlsl");
    Check(offsetof(DlssNrConstants, ExposureMode) == offsetof(DlssNrConstants, ResidualConfidenceSensitivity) + 4 &&
              offsetof(DlssNrConstants, PreExposure) == offsetof(DlssNrConstants, ExposureMode) + 4 &&
              offsetof(DlssNrConstants, HistoryRejection) == 252 &&
              hlsl.find(
                  "gResidualConfidenceSensitivity;\n    uint gExposureModeUnused;\n    float gHistoryExposureRatio") !=
                  std::string::npos &&
              hlsl.find("uint gHistoryRejection;\n};") != std::string::npos,
          "Test 7: C++ constants match the residual shader's history slots");

    const std::string deferred = Read("OptiScaler/shaders/dlssnr/DlssNr_Dx12_DeferredSr.cpp");
    Check(deferred.find("ResidualHistory::Fill(accum, g.history, cfg.DlssNrHistoryRejection.value_or_default()") !=
                  std::string::npos &&
              deferred.find("ResidualHistory::Record(g.history, accum);") != std::string::npos,
          "Test 8: the DX12 deferred path fills and records history through the shared rules");

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

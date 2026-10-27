// NR model preset menu: production labels and previews, and every pass keeps its preset control
// (NR-LIGHTWEIGHT-MODEL-PRESET-SUPPORT.md).
// Build and run from the repo root: g++ -std=c++20 tests/nr_model_preset_menu_unit.cpp && ./a.out
#include "../OptiScaler/dlssnr/DlssNr_MenuPresets.h"

#ifndef SOURCE_DIR
#define SOURCE_DIR "."
#endif

#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace Presets = DlssNr::MenuPresets;

static int failures = 0;

static void Check(bool ok, const char* name)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    failures += ok ? 0 : 1;
}

static bool Same(const char* a, const char* b) { return std::strcmp(a, b) == 0; }

int main()
{
    std::printf("NR model preset menu unit tests\n");

    Check(Presets::Count == 3 && Same(Presets::Names[0], "Automatic") && Same(Presets::Names[1], "Original") &&
              Same(Presets::Names[2], "Optimized"),
          "Test 1: preset indices 0/1/2 map to Automatic/Original/Optimized");

    Check(Same(Presets::Preview(false, std::nullopt), "Automatic"),
          "Test 2: pass 1 without a value previews Automatic (no inherit option)");

    Check(Same(Presets::Preview(true, std::nullopt), Presets::Inherit) && Presets::Inherited(true, std::nullopt),
          "Test 3: later pass without a value previews Inherit pass 1");

    Check(Same(Presets::Preview(true, 2u), "Optimized") && !Presets::Inherited(true, 2u) &&
              Same(Presets::Preview(true, 0u), "Automatic"),
          "Test 4: explicit per-pass presets, including 0, are shown rather than inherited");

    Check(Same(Presets::Preview(false, 7u), Presets::Unavailable) &&
              Same(Presets::Preview(true, 3u), Presets::Unavailable),
          "Test 5: out-of-range presets preview as Unavailable preset");

    std::ifstream file(std::string(SOURCE_DIR) + "/OptiScaler/dlssnr/DlssNr_MenuControls.cpp");
    std::stringstream text;
    text << file.rdbuf();
    const std::string source = text.str();
    Check(!source.empty() && source.find("NrModelCombo(&pass.preset, true);") != std::string::npos &&
              source.find("selectedPass <= 2") == std::string::npos &&
              source.find("NR model: inherits pass 1") == std::string::npos,
          "Test 6: passes 2..10 all get the preset combo (no selectedPass <= 2 gate)");

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

#include "../OptiScaler/framegen/dlssg/AmpereMfgStatusText.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

// The RTX 20/30 MFG unlocker status read "Live: n/a" while the unlocker was loaded and working. Live control is only
// the SilyNoMeta command API; sdli1995 v0.4.0 has none. The status must say in plain language whether the unlocker
// runs and where the multiplier is changed, and keep the technical fields for bug reports.
//
// Validates:
// 1. Headline states: running, not running (with the error), starting.
// 2. "RTX 20/30" order matches the "RTX 20 / 30 (SM75 / SM86) MFG Unlock" header; mod name falls back to sdli1995.
// 3. Multiplier line: game menu + restart for sdli1995, live from OptiScaler for SilyNoMeta.
// 4. Details: live control is active / ready / "not supported by this mod", never "n/a"; fields kept.
// 5. The menu uses the helpers in place of the old one-line status.

using AmpereMfgLoader::ModVariant;
using AmpereMfgLoader::Status;
using namespace AmpereMfgStatusText;

static std::string ReadFile(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(1);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

static Status Sdli1995Loaded()
{
    Status s;
    s.Enabled = s.DllFound = s.IniWritten = s.DllLoaded = true;
    s.Variant = ModVariant::Sdli1995;
    s.ModName = "sdli1995 v0.4.0";
    return s;
}

int main()
{
    // Test 1
    auto running = Sdli1995Loaded();
    assert(ResolveHeadline(running) == Headline::Running);
    assert(HeadlineText(running) == "RTX 20/30 frame generation unlock: running (sdli1995 v0.4.0)");
    auto failed = running;
    failed.ErrorMessage = "dlssg_sm86.dll not found";
    assert(ResolveHeadline(failed) == Headline::NotRunning);
    assert(HeadlineText(failed) == "RTX 20/30 frame generation unlock: not running: dlssg_sm86.dll not found");
    auto starting = running;
    starting.DllLoaded = false;
    assert(ResolveHeadline(starting) == Headline::Starting);
    assert(HeadlineText(starting) == "RTX 20/30 frame generation unlock: starting...");
    std::puts("Test 1 PASS: headline says running, not running or starting");

    // Test 2
    auto unnamed = running;
    unnamed.ModName.clear();
    assert(HeadlineText(unnamed).find("(sdli1995)") != std::string::npos);
    assert(HeadlineText(running).find("RTX 30/20") == std::string::npos);
    std::puts("Test 2 PASS: RTX 20/30 order and mod-name fallback");

    // Test 3
    assert(std::string(MultiplierText(running)) ==
           "Multiplier: set in the game's graphics menu. Ceiling changes need Save Settings + restart.");
    auto sily = running;
    sily.Variant = ModVariant::SilyNoMeta;
    sily.ModName = "SilyNoMeta v0.3.5";
    sily.LiveControlSupported = true;
    assert(std::string(MultiplierText(sily)) ==
           "Multiplier: set in the game's menu or live from OptiScaler (Dynamic MFG available).");
    std::puts("Test 3 PASS: multiplier line matches what each mod supports");

    // Test 4
    const auto details = DetailsText(running, "SM86");
    for (const char* field : { "Mod: sdli1995 v0.4.0", "DLL: found", "Router: SM86", "INI: written", "Loaded: yes",
                               "Live control: not supported by this mod" })
        assert(details.find(field) != std::string::npos);
    assert(details.find("n/a") == std::string::npos && "never the ambiguous n/a");
    assert(std::string(LiveControlText(sily)) == "ready");
    sily.LiveControlActive = true;
    assert(std::string(LiveControlText(sily)) == "active");
    std::puts("Test 4 PASS: details keep the technical fields with a clear live-control value");

    // Test 5
    const std::string menu = ReadFile("OptiScaler/menu/menu_common.cpp");
    assert(menu.find("#include <framegen/dlssg/AmpereMfgStatusText.h>") != std::string::npos);
    assert(menu.find("Live: %s") == std::string::npos && "the old one-line status is gone");
    const auto header = menu.find("CollapsingHeader(\"RTX 20 / 30 (SM75 / SM86) MFG Unlock\")");
    const auto headline = menu.find("AmpereMfgStatusText::HeadlineText(status)");
    const auto marker = menu.find("ShowHelpMarker(details.c_str());");
    const auto multiplier = menu.find("AmpereMfgStatusText::MultiplierText(status)");
    assert(header != std::string::npos && headline != std::string::npos && marker != std::string::npos &&
           multiplier != std::string::npos);
    assert(header < headline && headline < marker && marker < multiplier);
    std::puts("Test 5 PASS: the menu shows headline, details marker and multiplier line");

    std::puts("All Ampere MFG status text tests passed (5/5).");
    return 0;
}

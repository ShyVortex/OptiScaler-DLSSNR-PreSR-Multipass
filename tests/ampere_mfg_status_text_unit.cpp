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
// 6. R580 floor: only a known Windows driver below 580.00 shows the note; Wine and unknown versions never do.
// 7. HardwareBilinear is labelled as 0.3.x / 0.4.0 only, and the menu uses the label and the floor note.
// 8. CompareModVersion orders dotted versions numerically and ignores -suffixes.
// 9. HardwareBilinear is locked only for sdli1995 that states 0.4.1 or newer; 0.4.0 / no version / SilyNoMeta /
//    not loaded keep it enabled.
// 10. The menu greys the checkbox, never writes the value while locked, and explains why.
// 11. The Preset combo's Auto entry says it runs as A on sdli1995 0.4.1+, while the stored value and the companion
//     INI stay "Auto".
// 12. A pre-310 game DLSS-G runtime (Remnant 2 ships 3.7.10, DLSS Unlocked #61) makes the headline "not running for
//     this game" only on sdli1995 0.4.1+ with the mod loaded; a loader error keeps priority; details show the version.
// 13. The menu shows the alternatives line only for a refused runtime, after the headline.

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

    // Test 6
    assert(DriverBelowFloor(false, 57688) && "576.88 is below R580");
    assert(DriverBelowFloor(false, 57999));
    assert(!DriverBelowFloor(false, 58000) && !DriverBelowFloor(false, 61664));
    assert(!DriverBelowFloor(false, 0) && "unknown version says nothing");
    assert(!DriverBelowFloor(true, 57688) && "Wine reports Linux driver numbers");
    assert(DriverFloorText(57688) ==
           "NVIDIA driver 576.88 is too old: this unlocker needs driver 580 or newer and refuses frame generation "
           "below it.");
    assert(DriverFloorText(57605).find("576.05") != std::string::npos);
    std::puts("Test 6 PASS: R580 floor note only for a known Windows driver below 580");

    // Test 7
    assert(std::string(HardwareBilinearLabel) == "Hardware Bilinear (0.3.x / 0.4.0 only)##sm86");
    assert(menu.find("ImGui::Checkbox(AmpereMfgStatusText::HardwareBilinearLabel, &hwBilinear)") != std::string::npos);
    assert(menu.find("\"Hardware Bilinear (approximate sampling)##sm86\"") == std::string::npos);
    const auto floor = menu.find("AmpereMfgStatusText::DriverBelowFloor(onLinux, primaryGpu.nvidiaDriverVersion)");
    assert(floor != std::string::npos && multiplier < floor && "the floor note follows the status lines");
    std::puts("Test 7 PASS: HardwareBilinear label and floor note wired into the menu");

    // Test 8
    assert(CompareModVersion("0.4.1", "0.4.1") == 0);
    assert(CompareModVersion("0.4.1", "0.4.0") > 0 && CompareModVersion("0.4.0", "0.4.1") < 0);
    assert(CompareModVersion("0.4.10", "0.4.9") > 0 && "numeric, not lexical");
    assert(CompareModVersion("0.5.0", "0.4.1") > 0 && CompareModVersion("1.0", "0.4.1") > 0);
    assert(CompareModVersion("0.4", "0.4.0") == 0 && CompareModVersion("0.4.1", "0.4") > 0);
    assert(CompareModVersion("0.3.5-5", "0.3.5") == 0 && CompareModVersion("0.3.5-5", "0.4.1") < 0);
    std::puts("Test 8 PASS: mod versions compare numerically");

    // Test 9
    auto v041 = running;
    v041.ModVersion = "0.4.1";
    assert(!HardwareBilinearHonoured(v041) && "0.4.1 no longer reads HardwareBilinear");
    for (const char* newer : { "0.4.2", "0.4.10", "0.5.0" })
    {
        auto s = running;
        s.ModVersion = newer;
        assert(!HardwareBilinearHonoured(s));
    }
    auto v040 = running;
    v040.ModVersion = "0.4.0";
    assert(HardwareBilinearHonoured(v040));
    auto unversioned = running; // 0.4.0 and older state no version and still read the key
    unversioned.ModVersion.clear();
    assert(HardwareBilinearHonoured(unversioned));
    auto silyVersioned = sily;
    silyVersioned.ModVersion = "0.4.5";
    assert(HardwareBilinearHonoured(silyVersioned) && "only sdli1995 removed the key");
    Status notLoaded;
    assert(HardwareBilinearHonoured(notLoaded));
    std::puts("Test 9 PASS: locked only for sdli1995 stating 0.4.1 or newer");

    // Test 10
    const auto block =
        menu.substr(menu.find("const bool hwBilinearHonoured"),
                    menu.find("// Dynamic Multi-Frame Generation") - menu.find("const bool hwBilinearHonoured"));
    assert(block.find("AmpereMfgStatusText::HardwareBilinearHonoured(status)") != std::string::npos);
    const auto disabled = block.find("ImGui::BeginDisabled(!hwBilinearHonoured);");
    const auto checkbox = block.find("&hwBilinear) && hwBilinearHonoured)");
    const auto write = block.find("config->FGDLSSGAmpereMfgHardwareBilinear = hwBilinear;");
    assert(disabled != std::string::npos && checkbox != std::string::npos && write != std::string::npos &&
           disabled < checkbox && checkbox < write && "no write while locked");
    assert(block.find("ImGuiHoveredFlags_AllowWhenDisabled") != std::string::npos &&
           block.find("HardwareBilinearIgnoredText") != std::string::npos);
    assert(block.find("FGDLSSGAmpereMfgHardwareBilinear.reset") == std::string::npos &&
           block.find("FGDLSSGAmpereMfgHardwareBilinear = false") == std::string::npos &&
           "the saved value is never reset");
    std::puts("Test 10 PASS: menu locks HardwareBilinear without touching the saved value");

    // Test 11
    assert(std::string(PresetAutoLabel(v041)) == "Auto (runs as A in this build)");
    assert(std::string(PresetAutoLabel(v040)) == "Auto (Game / Profile default)");
    assert(std::string(PresetAutoLabel(unversioned)) == "Auto (Game / Profile default)");
    assert(std::string(PresetAutoLabel(silyVersioned)) == "Auto (Game / Profile default)");
    assert(menu.find("const char* presetOptions[] = { AmpereMfgStatusText::PresetAutoLabel(status),") !=
           std::string::npos);
    assert(menu.find("const char* storedPresetOptions[] = { \"Auto\", \"A\", \"B\", \"C\" };") != std::string::npos &&
           "the stored value stays Auto");
    assert(AmpereMfgLoader::FormatIniContent030(3, 1, "Auto").find("[DLSSG]\nOptimized=1\nPreset=Auto\n") !=
               std::string::npos &&
           "the companion INI keeps Preset=Auto (0.3.x reads it; 0.4.1 runs it as A)");
    std::puts("Test 11 PASS: Preset Auto label per build; stored value and INI unchanged");

    // Test 12
    auto refused = v041;
    refused.ModName = "sdli1995 v0.4.1";
    refused.GameDlssgPath = L"C:\\Remnant2\\Plugins\\Shared\\Streamline\\nvngx_dlssg.dll";
    refused.GameDlssgVersion = "3.7.10";
    refused.GameDlssgGenerationA = true;
    assert(GameRuntimeRefused(refused));
    assert(ResolveHeadline(refused) == Headline::NotRunning);
    assert(HeadlineText(refused) == "RTX 20/30 frame generation unlock: not running for this game: it ships DLSS-G "
                                    "3.7.10 (older than 310.x), which sdli1995 v0.4.1 does not take over.");
    assert(DetailsText(refused, "SM86").find("Game DLSS-G: 3.7.10") != std::string::npos);
    for (auto accepted : { v040, unversioned, silyVersioned })
    {
        accepted.GameDlssgVersion = "3.7.10";
        accepted.GameDlssgGenerationA = true;
        assert(!GameRuntimeRefused(accepted) && ResolveHeadline(accepted) == Headline::Running);
    }
    auto modern = refused;
    modern.GameDlssgVersion = "310.9.16";
    modern.GameDlssgGenerationA = false;
    assert(!GameRuntimeRefused(modern) && ResolveHeadline(modern) == Headline::Running);
    auto refusedNotLoaded = refused;
    refusedNotLoaded.DllLoaded = false;
    assert(!GameRuntimeRefused(refusedNotLoaded));
    auto refusedWithError = refused;
    refusedWithError.ErrorMessage = "dlssg_sm86.dll not found";
    assert(HeadlineText(refusedWithError) ==
           "RTX 20/30 frame generation unlock: not running: dlssg_sm86.dll not found");
    assert(DetailsText(running, "SM86").find("Game DLSS-G: not seen yet") != std::string::npos);
    auto unreadVersion = running;
    unreadVersion.GameDlssgPath = L"C:\\G\\nvngx_dlssg.dll";
    assert(DetailsText(unreadVersion, "SM86").find("Game DLSS-G: version unknown") != std::string::npos);
    std::string alternatives = GameRuntimeAlternativesText;
    assert(alternatives.find("DLSS Swapper") != std::string::npos && alternatives.find("XeMFG") != std::string::npos &&
           alternatives.find("OptiFG") != std::string::npos);
    std::puts("Test 12 PASS: a refused pre-310 game runtime reads as not running for this game");

    // Test 13
    const auto refusedLine =
        menu.find("if (AmpereMfgStatusText::GameRuntimeRefused(status) && status.ErrorMessage.empty())");
    const auto alternativesLine = menu.find("AmpereMfgStatusText::GameRuntimeAlternativesText");
    assert(refusedLine != std::string::npos && alternativesLine != std::string::npos);
    assert(headline < refusedLine && refusedLine < alternativesLine);
    std::puts("Test 13 PASS: the menu adds the alternatives under the headline for a refused runtime");

    std::puts("All Ampere MFG status text tests passed (13/13).");
    return 0;
}

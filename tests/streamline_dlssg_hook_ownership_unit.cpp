#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

// DLSS Unlocked #58 follow-up: 2a0dbf5e cleared the sl.dlss_g plugin's swapchain hooks and resource tags whenever
// the Ampere/Turing unlocker was on. That setup is for OptiScaler taking over DLSS-G and presenting the frames; with
// the external unlocker the game's own DLSS-G presents, so clearing them leaves no presenter.
//
// Validates against the production hkdlssg_slOnPluginLoad:
// 1. Hooks/tags are cleared only when OptiScaler owns DLSS-G (FGInput::DLSSG / FGOutput::DLSSG), never for the
//    external unlocker or XeMFG alone.
// 2. Architecture spoofing still includes ampereMfgActive (architecture rule 4) and xeMfgActive.
// 3. The vsync / hardware-scheduling relaxation still includes ampereMfgActive and xeMfgActive.
// 4. Decision table mirrored from the production conditions.

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

static std::string Between(const std::string& text, const std::string& from, const std::string& to)
{
    const auto start = text.find(from);
    assert(start != std::string::npos && "start marker not found");
    const auto end = text.find(to, start + from.size());
    assert(end != std::string::npos && "end marker not found");
    return text.substr(start, end - start);
}

// The condition of the function-level `if` (4-space indent) that guards a statement, skipping nested ifs.
static std::string ConditionBefore(const std::string& body, const std::string& statement)
{
    const auto at = body.find(statement);
    assert(at != std::string::npos && "guarded statement not found");
    const auto ifAt = body.rfind("\n    if (", at);
    assert(ifAt != std::string::npos);
    const auto braceAt = body.find("\n    {", ifAt);
    assert(braceAt != std::string::npos && braceAt < at);
    std::string condition = body.substr(ifAt, braceAt - ifAt);
    condition = std::regex_replace(condition, std::regex("\\s+"), " ");
    return condition;
}

struct Inputs
{
    bool dlssgInput, dlssgOutput, ampere, xeMfg;
};

// Mirrors the production conditions checked in Tests 1-3.
static bool ClearsHooks(const Inputs& in) { return in.dlssgInput || in.dlssgOutput; }
static bool RelaxesVsync(const Inputs& in) { return in.dlssgInput || in.ampere || in.xeMfg; }

int main()
{
    const std::string source = ReadFile("OptiScaler/hooks/Streamline_Hooks.cpp");
    const auto body = Between(source, "bool StreamlineHooks::hkdlssg_slOnPluginLoad(", "\n}\n");

    // Test 1
    const auto clear = ConditionBefore(body, "configJson[\"hooks\"].clear();");
    assert(clear.find("FGInput::DLSSG") != std::string::npos && clear.find("FGOutput::DLSSG") != std::string::npos);
    assert(clear.find("ampereMfgActive") == std::string::npos &&
           "the external unlocker's DLSS-G needs its swapchain hooks");
    assert(clear.find("xeMfgActive") == std::string::npos);
    const auto tags = ConditionBefore(body, "configJson[\"external\"][\"feature\"][\"tags\"].clear();");
    assert(tags == clear && "hooks and resource tags are cleared under the same ownership condition");
    std::puts("Test 1 PASS: DLSS-G hooks are cleared only when OptiScaler owns DLSS-G");

    // Test 2
    const auto spoof = Between(body, "bool shouldSpoofArch", ";");
    assert(spoof.find("ampereMfgActive") != std::string::npos && "architecture rule 4: spoof for the unlocker");
    assert(spoof.find("xeMfgActive") != std::string::npos);
    std::puts("Test 2 PASS: architecture spoofing still covers the external unlocker");

    // Test 3
    const auto vsync = ConditionBefore(body, "configJson[\"vsync\"][\"supported\"] = true;");
    assert(vsync.find("ampereMfgActive") != std::string::npos && vsync.find("xeMfgActive") != std::string::npos);
    std::puts("Test 3 PASS: vsync / HWS relaxation still covers the external unlocker");

    // Test 4
    // External Ampere/Turing MFG: game DLSS-G keeps its hooks, but still gets the relaxed requirements.
    assert(!ClearsHooks({ false, false, true, false }) && RelaxesVsync({ false, false, true, false }));
    // XeMFG Source=Game routes DLSS-G input into XeFG: OptiScaler owns it, hooks cleared.
    assert(ClearsHooks({ true, false, false, true }));
    // OptiScaler's own DLSS-G output: hooks cleared.
    assert(ClearsHooks({ false, true, false, false }));
    // Nothing active: untouched.
    assert(!ClearsHooks({ false, false, false, false }) && !RelaxesVsync({ false, false, false, false }));
    std::puts("Test 4 PASS: ownership decides hook clearing");

    std::puts("All Streamline external MFG hook tests passed (4/4).");
    return 0;
}

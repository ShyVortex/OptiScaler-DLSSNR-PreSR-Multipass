#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

// Replicates the logic from menu_common.cpp for SM86 MFG Multiplier Ceiling UI
struct CeilingUiState
{
    int maxFrames = 3;
    bool dmfgActive = false;
};

struct CeilingUiResult
{
    const char* sliderLabel = "Max Multiplier Ceiling##sm86";
    std::string currentLabel;
    bool showNotice = false;
    std::string noticeText;
    std::string tooltipText;
};

CeilingUiResult EvaluateCeilingUi(const CeilingUiState& state)
{
    CeilingUiResult res;
    const char* frameLabels[] = { "Factory default (up to 4X)",
                                  "1 (up to 2X only)",
                                  "2 (up to 3X)",
                                  "3 (up to 4X)",
                                  "4 (up to 5X)",
                                  "5 (up to 6X)" };

    res.currentLabel = state.dmfgActive
                           ? "Dynamic (Up to 6X)"
                           : ((state.maxFrames >= 0 && state.maxFrames <= 5) ? frameLabels[state.maxFrames]
                                                                             : "Factory default (up to 4X)");

    if (!state.dmfgActive && state.maxFrames == 1)
    {
        res.showNotice = true;
        res.noticeText = "Notice: Ceiling set to 2X. In-game 2x/3x multiplier menus will be hidden by the game engine.";
    }

    res.tooltipText =
        "Maximum multiplier capability advertised to the game engine (1=2X, 2=3X, 3=4X, 4=5X, 5=6X).\n"
        "The game's graphics settings menu chooses the actual active multiplier.\n\n"
        "Important: Setting this to 1 (up to 2X only) tells the game engine that Multi-Frame Generation (3X/4X) "
        "is not supported. Games with in-engine multiplier selectors (e.g. Where Winds Meet, Black Myth: Wukong, "
        "Final Fantasy) will hide their 2x/3x selection dropdowns and lock to standard 2X.\n"
        "Keep this at Factory default (4X) or higher so in-game menus remain accessible.\n\n"
        "Save Settings and restart to apply.";

    return res;
}

int main()
{
    std::printf("=== Running SM86 MFG Multiplier Ceiling UI Unit Tests ===\n");

    // Case 1: Default ceiling (3 = up to 4X)
    {
        CeilingUiState s { 3, false };
        auto res = EvaluateCeilingUi(s);
        assert(res.currentLabel == "3 (up to 4X)");
        assert(!res.showNotice);
        assert(std::strcmp(res.sliderLabel, "Max Multiplier Ceiling##sm86") == 0);
        std::printf("  [PASS] Case 1: Default ceiling 3 resolves to '3 (up to 4X)' without notice\n");
    }

    // Case 2: Factory default limit (0)
    {
        CeilingUiState s { 0, false };
        auto res = EvaluateCeilingUi(s);
        assert(res.currentLabel == "Factory default (up to 4X)");
        assert(!res.showNotice);
        std::printf("  [PASS] Case 2: Factory default 0 resolves to 'Factory default (up to 4X)'\n");
    }

    // Case 3: Clamped ceiling 1 (Issue #50 condition: Where Winds Meet)
    {
        CeilingUiState s { 1, false };
        auto res = EvaluateCeilingUi(s);
        assert(res.currentLabel == "1 (up to 2X only)");
        assert(res.showNotice);
        assert(res.noticeText.find("Ceiling set to 2X") != std::string::npos);
        assert(res.noticeText.find("In-game 2x/3x multiplier menus will be hidden") != std::string::npos);
        assert(res.tooltipText.find("Where Winds Meet") != std::string::npos);
        assert(res.tooltipText.find("Black Myth: Wukong") != std::string::npos);
        std::printf("  [PASS] Case 3: Ceiling 1 correctly displays '1 (up to 2X only)' and warning banner\n");
    }

    // Case 4: Higher ceilings (2 = 3X, 4 = 5X, 5 = 6X)
    {
        CeilingUiState s2 { 2, false };
        auto res2 = EvaluateCeilingUi(s2);
        assert(res2.currentLabel == "2 (up to 3X)");
        assert(!res2.showNotice);

        CeilingUiState s4 { 4, false };
        auto res4 = EvaluateCeilingUi(s4);
        assert(res4.currentLabel == "4 (up to 5X)");
        assert(!res4.showNotice);

        CeilingUiState s5 { 5, false };
        auto res5 = EvaluateCeilingUi(s5);
        assert(res5.currentLabel == "5 (up to 6X)");
        assert(!res5.showNotice);
        std::printf("  [PASS] Case 4: Ceilings 2, 4, 5 resolve to up to 3X, 5X, 6X without notice\n");
    }

    // Case 5: Dynamic MFG active
    {
        CeilingUiState s { 1, true };
        auto res = EvaluateCeilingUi(s);
        assert(res.currentLabel == "Dynamic (Up to 6X)");
        assert(!res.showNotice && "Dynamic MFG overrides notice since runtime handles scaling");
        std::printf("  [PASS] Case 5: Dynamic MFG overrides label to 'Dynamic (Up to 6X)' and suppresses notice\n");
    }

    // Case 6: Out of bounds values fallback
    {
        CeilingUiState sNegative { -1, false };
        auto resNeg = EvaluateCeilingUi(sNegative);
        assert(resNeg.currentLabel == "Factory default (up to 4X)");
        assert(!resNeg.showNotice);

        CeilingUiState sLarge { 99, false };
        auto resLarge = EvaluateCeilingUi(sLarge);
        assert(resLarge.currentLabel == "Factory default (up to 4X)");
        assert(!resLarge.showNotice);
        std::printf("  [PASS] Case 6: Out-of-bounds ceiling values fall back cleanly to default\n");
    }

    std::printf("=== All SM86 MFG Multiplier Ceiling UI Unit Tests PASSED ===\n");
    return 0;
}

#include "../OptiScaler/framegen/dlssg/AmpereMfgLoader.h"
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

// Standalone unit test validating DLSSG.AmpereMfgMode (commit 7f1c15fc
// "[PATCH 2/3] Add DLSSG.AmpereMfgMode=Internal"):
//
//  Suite 1 — parsing + State mirroring, MIRRORED from OptiScaler/Config.cpp:75-92 (Config.cpp is
//            not standalone-compilable: pch.h / SimpleIni / State / Util dependencies), and
//            State.h:61-65 / :191-192.
//  Suite 2 — internal-mode companion-INI SpoofArchToGame resolution via the REAL
//            AmpereMfgLoader::FormatIniContent030 (AmpereMfgLoader.h:230-300), which the loader
//            calls with internalModeActive = State::ampereMfgInternalActive
//            (AmpereMfgLoader.cpp:296/309-311; State.h:193).

// Mirrors OptiScaler/State.h:61-65
enum class AmpereMfgMode : uint32_t
{
    External,
    Internal,
};

// Mirrors OptiScaler/State.h:191-192 — startup-only mirror of the parsed DLSSG.AmpereMfgMode value
struct MockState
{
    AmpereMfgMode ampereMfgMode = AmpereMfgMode::External;
};

// Minimal stand-in for the CustomOptional<std::string, NoDefault> FGDLSSGAmpereMfgMode member
// (Config.h:660). On a freshly-constructed optional the real CustomOptional::set_from_config
// (Config.h:51-58, only applies while unset) is equivalent to plain assignment, so a plain
// std::optional is used; value_or() is inherited from std::optional exactly as in the real code.
using MockModeConfig = std::optional<std::string>;

// Case-insensitive equality mirroring lstrcmpiA(a, b) == 0 as used by the real parse
// comparisons (Config.cpp:77/79).
bool ModeEqualsIgnoreCase(const std::string& value, const char* literal)
{
    const size_t len = std::strlen(literal);
    if (value.size() != len)
        return false;

    for (size_t i = 0; i < len; ++i)
    {
        if (std::tolower(static_cast<unsigned char>(value[i])) != std::tolower(static_cast<unsigned char>(literal[i])))
            return false;
    }

    return true;
}

// Mirror of Config.cpp:75-92 — parse the DLSSG.AmpereMfgMode ini value and mirror the effective
// mode into State. warnFallback is set where the real code LOG_WARNs (Config.cpp:83) before
// falling back to External; an absent key leaves both the config value and the State mirror
// untouched (startup-only update).
void SimulateAmpereMfgModeParse(MockState& state, MockModeConfig& cfg, const std::optional<std::string>& iniValue,
                                bool& warnFallback)
{
    warnFallback = false;

    if (iniValue.has_value())
    {
        if (ModeEqualsIgnoreCase(iniValue.value(), "internal"))
            cfg = "Internal";
        else if (ModeEqualsIgnoreCase(iniValue.value(), "external"))
            cfg = "External";
        else
        {
            // LOG_WARN("Config: Invalid DLSSG.AmpereMfgMode value: {}. Falling back to External.")
            warnFallback = true;
            cfg = "External";
        }

        // Startup-only: mirror the effective parsed mode into State (Config.cpp:88-91)
        state.ampereMfgMode = cfg.value_or("External") == "Internal" ? AmpereMfgMode::Internal : AmpereMfgMode::External;
    }
}

int main()
{
    std::printf("=== Running Ampere MFG Mode (External/Internal) Config Unit Tests ===\n");

    using namespace AmpereMfgLoader;

    // =========================================================================
    // Suite 1: DLSSG.AmpereMfgMode parsing + State mirror (mirrors Config.cpp:75-92)
    // =========================================================================
    std::printf("\n--- Suite 1: AmpereMfgMode Parsing & State Mirror ---\n");

    // 1.1 "internal" parses to Internal and mirrors Internal into State
    {
        MockState state;
        MockModeConfig cfg;
        bool warn = true;
        SimulateAmpereMfgModeParse(state, cfg, std::optional<std::string>("internal"), warn);
        assert(cfg.has_value() && cfg.value() == "Internal");
        assert(state.ampereMfgMode == AmpereMfgMode::Internal);
        assert(!warn);
        std::printf("  [PASS] 1.1 \"internal\" parses to Internal and mirrors into State\n");
    }

    // 1.2 Parsing is case-insensitive (lstrcmpiA semantics, Config.cpp:77-80)
    {
        for (const char* spelling : { "Internal", "INTERNAL", "iNtErNaL" })
        {
            MockState state;
            MockModeConfig cfg;
            bool warn = false;
            SimulateAmpereMfgModeParse(state, cfg, std::optional<std::string>(spelling), warn);
            assert(cfg.value() == "Internal");
            assert(state.ampereMfgMode == AmpereMfgMode::Internal);
            assert(!warn);
        }
        std::printf("  [PASS] 1.2 Case-insensitive \"internal\" spellings all parse to Internal\n");
    }

    // 1.3 "external" (any casing) parses to External and mirrors External into State
    {
        for (const char* spelling : { "external", "External", "EXTERNAL" })
        {
            MockState state;
            MockModeConfig cfg;
            bool warn = false;
            SimulateAmpereMfgModeParse(state, cfg, std::optional<std::string>(spelling), warn);
            assert(cfg.value() == "External");
            assert(state.ampereMfgMode == AmpereMfgMode::External);
            assert(!warn);
        }
        std::printf("  [PASS] 1.3 \"external\" spellings parse to External and mirror External into State\n");
    }

    // 1.4 Invalid values warn and fall back to External (Config.cpp:81-86)
    {
        MockState state;
        MockModeConfig cfg;
        bool warn = false;
        SimulateAmpereMfgModeParse(state, cfg, std::optional<std::string>("hybrid"), warn);
        assert(warn); // LOG_WARN before the fallback
        assert(cfg.value() == "External");
        assert(state.ampereMfgMode == AmpereMfgMode::External);

        warn = false;
        SimulateAmpereMfgModeParse(state, cfg, std::optional<std::string>(""), warn);
        assert(warn);
        assert(cfg.value() == "External");
        assert(state.ampereMfgMode == AmpereMfgMode::External);
        std::printf("  [PASS] 1.4 Invalid AmpereMfgMode values warn and fall back to External\n");
    }

    // 1.5 Absent key: config stays unset (behaves as External via value_or), State mirror untouched
    {
        MockState state;
        state.ampereMfgMode = AmpereMfgMode::Internal; // sentinel proving the mirror is untouched
        MockModeConfig cfg;
        bool warn = false;
        SimulateAmpereMfgModeParse(state, cfg, std::nullopt, warn);
        assert(!cfg.has_value());                      // key stays unset
        assert(cfg.value_or("External") == "External"); // ...but behaves as External in the callers
        assert(state.ampereMfgMode == AmpereMfgMode::Internal); // startup-only mirror NOT touched
        assert(!warn);
        std::printf("  [PASS] 1.5 Absent key stays unset (effective External) and leaves the State mirror "
                    "untouched\n");
    }

    // =========================================================================
    // Suite 2: Internal-mode companion-INI SpoofArchToGame formatting
    //           (REAL FormatIniContent030, AmpereMfgLoader.h:230-300)
    // =========================================================================
    std::printf("\n--- Suite 2: Companion-INI SpoofArchToGame (FormatIniContent030) ---\n");

    // 2.1 Internal mode + SpoofArch auto/absent -> SpoofArchToGame=0 is emitted
    {
        std::string ini = FormatIniContent030(5, 1, "Auto", "Auto", 0, "Auto", 1, "Auto", false, 0.0f, false,
                                              /*internalModeActive=*/true);
        assert(ini.find("SpoofArchToGame=0\n") != std::string::npos);
        std::printf("  [PASS] 2.1 Internal mode resolves auto/absent SpoofArch to SpoofArchToGame=0\n");
    }

    // 2.2 Internal mode + explicit "1" -> the explicit escape hatch is preserved as-is
    {
        std::string ini = FormatIniContent030(3, 1, "Auto", "Auto", 0, "Auto", 1, "1", false, 0.0f, false, true);
        assert(ini.find("SpoofArchToGame=1\n") != std::string::npos);
        std::printf("  [PASS] 2.2 Internal mode keeps an explicit SpoofArchToGame=1 escape hatch\n");
    }

    // 2.3 Internal mode + explicit "0"/"false"/"true" spellings behave exactly as the key layout
    //     specifies (AmpereMfgLoader.h:285-290)
    {
        std::string iniFalse = FormatIniContent030(3, 1, "Auto", "Auto", 0, "Auto", 1, "false", false, 0.0f, false,
                                                   true);
        assert(iniFalse.find("SpoofArchToGame=0\n") != std::string::npos);

        std::string iniTrue = FormatIniContent030(3, 1, "Auto", "Auto", 0, "Auto", 1, "true", false, 0.0f, false,
                                                  true);
        assert(iniTrue.find("SpoofArchToGame=1\n") != std::string::npos);

        std::string iniZero = FormatIniContent030(3, 1, "Auto", "Auto", 0, "Auto", 1, "0", false, 0.0f, false, true);
        assert(iniZero.find("SpoofArchToGame=0\n") != std::string::npos);
        std::printf("  [PASS] 2.3 Internal mode preserves explicit 0/false/true spellings\n");
    }

    // 2.4 Internal mode + any other non-explicit value ("banana") still resolves to 0
    //     (the internalModeActive branch is the auto/else branch, AmpereMfgLoader.h:289-290)
    {
        std::string ini = FormatIniContent030(3, 1, "Auto", "Auto", 0, "Auto", 1, "banana", false, 0.0f, false, true);
        assert(ini.find("SpoofArchToGame=0\n") != std::string::npos);
        std::printf("  [PASS] 2.4 Internal mode resolves arbitrary non-explicit SpoofArch values to 0\n");
    }

    // 2.5 External mode (internalModeActive=false) + auto -> NO SpoofArchToGame line
    //     (factory INI convention preserved)
    {
        std::string ini = FormatIniContent030(5, 1, "Auto", "Auto", 0, "Auto", 1, "Auto", false, 0.0f, false,
                                              /*internalModeActive=*/false);
        assert(ini.find("SpoofArchToGame") == std::string::npos);
        std::printf("  [PASS] 2.5 External mode with auto SpoofArch omits the SpoofArchToGame line\n");
    }

    // 2.6 External mode + explicit values still emits them (pre-feature behavior unchanged)
    {
        std::string iniOne = FormatIniContent030(3, 1, "Auto", "Auto", 0, "Auto", 1, "1", false, 0.0f, false, false);
        assert(iniOne.find("SpoofArchToGame=1\n") != std::string::npos);

        std::string iniZero = FormatIniContent030(3, 1, "Auto", "Auto", 0, "Auto", 1, "0", false, 0.0f, false, false);
        assert(iniZero.find("SpoofArchToGame=0\n") != std::string::npos);
        std::printf("  [PASS] 2.6 External mode keeps explicit SpoofArchToGame 1/0 emission unchanged\n");
    }

    // 2.7 Defaulted call (internalModeActive defaults to false) is byte-identical to pre-feature
    //     output: no SpoofArchToGame line
    {
        std::string iniDefault = FormatIniContent030(3, 1);
        assert(iniDefault.find("SpoofArchToGame") == std::string::npos);

        std::string iniAuto = FormatIniContent030(3, 1, "Auto", "Auto", 0, "Auto", 1, "Auto");
        assert(iniAuto.find("SpoofArchToGame") == std::string::npos);
        assert(iniDefault == iniAuto);
        std::printf("  [PASS] 2.7 Defaulted internalModeActive=false keeps the factory INI byte-identical\n");
    }

    // 2.8 Internal mode leaves the full 0.3.x key layout intact: SpoofArchToGame=0 is inserted in
    //     [Compatibility] after HardwareBilinear, all other sections/keys unchanged
    {
        std::string ini = FormatIniContent030(5, true, "Auto", "Auto", 0, "Auto", 1, "Auto", false, 0.0f, false,
                                              true);
        assert(ini.find("[General]\nEnabled=1\n") != std::string::npos);
        assert(ini.find("[FrameGeneration]\nOptimized=1\nMaxGeneratedFrames=5\n") != std::string::npos);
        assert(ini.find("[Compatibility]\nPreset=Auto\nRouter=Auto\nKernelImage=Auto\nHardwareBilinear=0\n"
                        "SpoofArchToGame=0\n") != std::string::npos);
        assert(ini.find("[Logging]\nLevel=1\nDirectory=dlssg_sm86\\logs\n") != std::string::npos);
        assert(ini.find("[Runtime]\nMode=Bundled\nCacheDirectory=\n") != std::string::npos);
        assert(ini.find("DynamicMFG") == std::string::npos); // dynamic MFG off + no SilyNoMeta support
        std::printf("  [PASS] 2.8 Internal mode preserves the full 0.3.x INI key layout\n");
    }

    std::printf("\n=== All Ampere MFG Mode (External/Internal) Config Unit Tests PASSED! ===\n");
    return 0;
}

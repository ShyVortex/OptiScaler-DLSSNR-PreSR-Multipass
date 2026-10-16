#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

// Standalone unit test validating Ada MFG / Blackwell kernels config parsing, Ada-Ampere mutual
// exclusion, AmpereMfgMode (External/Internal) gating, and menu status display.
//
// Config semantics are MIRRORED from the current committed code (Config.cpp is not
// standalone-compilable: pch.h / SimpleIni / State / Util dependencies):
//   - SimulateConfigLoad  mirrors OptiScaler/Config.cpp:75-92   (DLSSG.AmpereMfgMode parse + State mirror)
//                          and OptiScaler/Config.cpp:207-215   (Ampere unlock forcing block)
//   - SimulateConfigSave  mirrors OptiScaler/Config.cpp:1143-1162 (SaveIni FrameGen/DLSSG write-back)
//   - Menu helpers        mirror OptiScaler/menu/menu_common.cpp:3204-3219 (locked External checkbox),
//                          3337-3350 (Ampere toggle side-effects) and 3412-3432 (mode combo + restart hint)
// as of commit 7f1c15fc "[PATCH 2/3] Add DLSSG.AmpereMfgMode=Internal".

// Mock NV_GPU_ARCHITECTURE constants
constexpr uint32_t NV_GPU_ARCHITECTURE_TU100 = 0x00000160;
constexpr uint32_t NV_GPU_ARCHITECTURE_GA100 = 0x00000170;
constexpr uint32_t NV_GPU_ARCHITECTURE_AD100 = 0x00000190;

enum class VendorId
{
    Unknown,
    Nvidia,
    Amd,
    Intel
};

// Minimal CustomOptional matching Config.h behavior.
// Simplification vs the real CustomOptional (Config.h:16-146): the real set_from_config
// (Config.h:51-58) only applies while the optional is still unset, while this mock overwrites
// whenever the incoming value has a value. The simulations below therefore model the effective
// (persisted/session) semantics; the raw in-memory optional nuance is not under test here.
template <class T> class TestCustomOptional : public std::optional<T>
{
    T _defaultValue {};

  public:
    TestCustomOptional(T defaultValue)
    {
        _defaultValue = defaultValue;
        this->reset();
    }

    TestCustomOptional() { this->reset(); }

    T value_or_default() const
    {
        if (this->has_value())
            return this->value();
        return _defaultValue;
    }

    std::optional<T> value_for_config() const
    {
        if (this->has_value())
            return this->value();
        return std::nullopt;
    }

    T value_for_config_or(T def) const
    {
        if (this->has_value())
            return this->value();
        return def;
    }

    void set_from_config(std::optional<T> val)
    {
        if (val.has_value())
            *this = val.value();
    }

    TestCustomOptional& operator=(const T& value)
    {
        std::optional<T>::operator=(value);
        return *this;
    }
};

struct MockConfig
{
    TestCustomOptional<bool> FGEnabled { false };
    TestCustomOptional<bool> ExternalFrameGeneration { false };
    TestCustomOptional<bool> FGDLSSGAdaMfgUnlock { false };
    TestCustomOptional<bool> FGDLSSGAdaBlackwellKernels { true };
    TestCustomOptional<bool> FGDLSSGAmpereMfgUnlock { false };
    // Mirrors Config.h:660 — CustomOptional<std::string, NoDefault> FGDLSSGAmpereMfgMode
    // ("External" default / "Internal"); value_or("External") below mirrors Config.cpp:211/1155.
    TestCustomOptional<std::string> FGDLSSGAmpereMfgMode;
};

// Mirrors OptiScaler/State.h:61-65
enum class AmpereMfgMode : uint32_t
{
    External,
    Internal,
};

struct MockState
{
    bool externalFrameGeneration = false;
    // Mirrors State.h:191-192 — startup-only mirror of the parsed DLSSG.AmpereMfgMode value
    AmpereMfgMode ampereMfgMode = AmpereMfgMode::External;
};

struct MockGpu
{
    VendorId vendorId = VendorId::Nvidia;
    struct
    {
        uint32_t architecture_id = NV_GPU_ARCHITECTURE_AD100;
    } nvidiaArchInfo;
    std::string name = "NVIDIA GeForce RTX 4070";
};

struct MockMfgUnlockStatus
{
    bool ModuleFound = false;
    bool AdvertiseMatched = false;
    bool ValidateMatched = false;
    uint32_t KernelsRewritten = 0;
    std::string SnippetVersion = "310.9";
};

// Case-insensitive equality mirroring lstrcmpiA(a, b) == 0 as used by the real
// DLSSG.AmpereMfgMode parse comparisons (Config.cpp:77/79).
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

// Helper to simulate Config::Reload mutual exclusion + AmpereMfgMode gating.
// Mirrors OptiScaler/Config.cpp:75-92 (DLSSG.AmpereMfgMode parse: case-insensitive, invalid value
// -> warn + External fallback, absent key -> left unset with the State mirror untouched) and
// Config.cpp:207-215 (Ampere unlock forcing: External FG is forced on only while
// AmpereMfgMode != Internal; AdaMfgUnlock is suppressed in both modes) as of commit 7f1c15fc.
void SimulateConfigLoad(MockState& state, MockConfig& cfg, std::optional<bool> iniAdaUnlock,
                        std::optional<bool> iniBlackwell, std::optional<bool> iniAmpereUnlock,
                        std::optional<std::string> iniAmpereMode = std::nullopt)
{
    cfg.FGDLSSGAdaMfgUnlock.set_from_config(iniAdaUnlock);
    cfg.FGDLSSGAdaBlackwellKernels.set_from_config(iniBlackwell);
    cfg.FGDLSSGAmpereMfgUnlock.set_from_config(iniAmpereUnlock);

    // Mirror of Config.cpp:75-92 — parse DLSSG.AmpereMfgMode
    if (iniAmpereMode.has_value())
    {
        if (ModeEqualsIgnoreCase(iniAmpereMode.value(), "internal"))
            cfg.FGDLSSGAmpereMfgMode.set_from_config("Internal");
        else if (ModeEqualsIgnoreCase(iniAmpereMode.value(), "external"))
            cfg.FGDLSSGAmpereMfgMode.set_from_config("External");
        else
        {
            // LOG_WARN("Config: Invalid DLSSG.AmpereMfgMode value: {}. Falling back to External.")
            // in Config.cpp:83 before the fallback below
            cfg.FGDLSSGAmpereMfgMode.set_from_config("External");
        }

        // Startup-only: mirror the effective parsed mode into State (Config.cpp:88-91)
        state.ampereMfgMode = cfg.FGDLSSGAmpereMfgMode.value_or("External") == "Internal"
                                  ? AmpereMfgMode::Internal
                                  : AmpereMfgMode::External;
    }

    // Mirror of Config.cpp:207-215 — External (default) keeps the historic external-FG forcing;
    // Internal keeps the internal FG route active. AdaMfgUnlock is suppressed in both modes.
    if (cfg.FGDLSSGAmpereMfgUnlock.value_or_default())
    {
        if (cfg.FGDLSSGAmpereMfgMode.value_or("External") != "Internal")
            cfg.ExternalFrameGeneration.set_from_config(true);

        cfg.FGDLSSGAdaMfgUnlock.set_from_config(false);
    }
}

// Helper to simulate Config::SaveIni mutual exclusion + AmpereMfgMode write-back.
// Mirrors OptiScaler/Config.cpp:1143-1162 as of commit 7f1c15fc: the FrameGen.External term is
// mode-gated (the forced-ON lock is only persisted while AmpereMfgMode != Internal) and
// DLSSG.AmpereMfgMode itself is always persisted, defaulting to "External" when unset.
void SimulateConfigSave(const MockConfig& cfg, bool& outAdaUnlock, bool& outAdaBlackwell, bool& outAmpereUnlock,
                        bool& outExternalFG, std::string& outAmpereMode)
{
    bool ampereUnlock = cfg.FGDLSSGAmpereMfgUnlock.value_for_config_or(false);
    bool adaUnlock = cfg.FGDLSSGAdaMfgUnlock.value_for_config_or(false);
    if (ampereUnlock && adaUnlock)
        adaUnlock = false;

    outAmpereUnlock = ampereUnlock;
    outAdaUnlock = adaUnlock;
    outAdaBlackwell = cfg.FGDLSSGAdaBlackwellKernels.value_for_config_or(false);
    outExternalFG = cfg.ExternalFrameGeneration.value_for_config_or(false) ||
                    (ampereUnlock && cfg.FGDLSSGAmpereMfgMode.value_for_config_or("External") != "Internal");
    outAmpereMode = cfg.FGDLSSGAmpereMfgMode.value_for_config_or("External");
}

// Helper to evaluate menu UI status string formatting in menu_common.cpp
std::string EvaluateMenuStatusText(bool adaUnlock, bool adaEnabledForSession, const MockMfgUnlockStatus& status)
{
    char buffer[256];
    if (adaUnlock != adaEnabledForSession)
    {
        return "Save Settings and restart to apply this change.";
    }
    else if (!status.ModuleFound)
    {
        return "Waiting for DLSSG to load.";
    }
    else if (status.AdvertiseMatched && status.ValidateMatched)
    {
        if (status.KernelsRewritten > 0)
        {
            snprintf(buffer, sizeof(buffer),
                     "DLSSG %s: RTX 40 MFG unlock applied with Blackwell kernels (%u containers).",
                     status.SnippetVersion.c_str(), status.KernelsRewritten);
            return std::string(buffer);
        }
        else
        {
            snprintf(buffer, sizeof(buffer), "DLSSG %s: RTX 40 MFG unlock applied (stock Ada kernels).",
                     status.SnippetVersion.c_str());
            return std::string(buffer);
        }
    }
    else
    {
        snprintf(buffer, sizeof(buffer), "DLSSG %s: unlock unavailable for this runtime.",
                 status.SnippetVersion.c_str());
        return std::string(buffer);
    }
}

// Mirrors the locked External checkbox displayed value while the Ampere unlock is active
// (menu_common.cpp:3204-3219, commit 7f1c15fc): forced-ON in External mode, locked-OFF under
// Internal mode (Internal drives OptiScaler's own FG route); free (user-controlled) otherwise.
bool EvaluateExternalCheckboxValue(bool ampereActive, const MockConfig& cfg)
{
    if (!ampereActive)
        return cfg.ExternalFrameGeneration.value_or_default(); // free (user-controlled) checkbox

    const bool ampereInternal = cfg.FGDLSSGAmpereMfgMode.value_or("External") == "Internal";
    return !ampereInternal;
}

// Mirrors the Ampere unlock checkbox side-effects when toggled ON (menu_common.cpp:3337-3350,
// commit 7f1c15fc): External FG is forced on only in External mode (Internal mode drives
// OptiScaler's own FG route, so External FG must stay off) and the Ada unlock is cleared.
// (The XeMfgUnlock / FGDLSSGSmoothMotion side-effects of the same block are outside this mock.)
void ApplyAmpereToggleSideEffects(MockConfig& cfg, bool ampereUnlock)
{
    if (!ampereUnlock)
        return;

    if (cfg.FGDLSSGAmpereMfgMode.value_or("External") != "Internal")
        cfg.ExternalFrameGeneration = true;

    cfg.FGDLSSGAdaMfgUnlock = false;
}

// Mirrors the Ampere MFG Mode combo selection (menu_common.cpp:3412-3421):
// index 0 stores "External", index 1 stores "Internal".
void ApplyAmpereModeCombo(MockConfig& cfg, int ampereModeIdx)
{
    const char* storedAmpereModeOptions[] = { "External", "Internal" };
    cfg.FGDLSSGAmpereMfgMode = std::string(storedAmpereModeOptions[ampereModeIdx]);
}

// Mirrors the startup-only restart hint predicate (menu_common.cpp:3429-3432): the hint is
// shown while the saved config mode differs from the session (State) mode.
bool EvaluateAmpereModeRestartHint(const MockConfig& cfg, const MockState& state)
{
    const bool ampereInternalConfigured = cfg.FGDLSSGAmpereMfgMode.value_or("External") == "Internal";
    return ampereInternalConfigured != (state.ampereMfgMode == AmpereMfgMode::Internal);
}

int main()
{
    printf("=== Running Ada MFG & Blackwell Kernels Unit Tests ===\n");

    // =========================================================================
    // Test Suite 1: Config Parsing & Default Values
    // =========================================================================
    {
        printf("\n--- Test Suite 1: Config Parsing & Defaults ---\n");

        MockConfig cfg;
        // 1.1 Unset default value is true in code
        assert(!cfg.FGDLSSGAdaBlackwellKernels.has_value());
        assert(cfg.FGDLSSGAdaBlackwellKernels.value_or_default() == true);
        printf("  [PASS] 1.1 Default unconfigured AdaBlackwellKernels evaluates to true\n");

        // 1.2 INI says AdaBlackwellKernels=true
        cfg.FGDLSSGAdaBlackwellKernels.set_from_config(std::optional<bool>(true));
        assert(cfg.FGDLSSGAdaBlackwellKernels.has_value());
        assert(cfg.FGDLSSGAdaBlackwellKernels.value_or_default() == true);
        printf("  [PASS] 1.2 Config parsing AdaBlackwellKernels=true correctly sets value\n");

        // 1.3 INI says AdaBlackwellKernels=false
        cfg.FGDLSSGAdaBlackwellKernels.set_from_config(std::optional<bool>(false));
        assert(cfg.FGDLSSGAdaBlackwellKernels.has_value());
        assert(cfg.FGDLSSGAdaBlackwellKernels.value_or_default() == false);
        printf("  [PASS] 1.3 Config parsing AdaBlackwellKernels=false correctly sets value\n");

        // 1.4 Config saving serialization
        cfg.FGDLSSGAdaBlackwellKernels = true;
        bool savedAda, savedBlackwell, savedAmpere, savedExternal;
        std::string savedMode;
        SimulateConfigSave(cfg, savedAda, savedBlackwell, savedAmpere, savedExternal, savedMode);
        assert(savedBlackwell == true);
        assert(savedMode == "External"); // unset AmpereMfgMode persists the External default
        printf("  [PASS] 1.4 Config saving preserves AdaBlackwellKernels=true and defaults AmpereMfgMode to "
               "External\n");
    }

    // =========================================================================
    // Test Suite 2: Ada & Ampere MFG Strict Mutual Exclusion
    // =========================================================================
    {
        printf("\n--- Test Suite 2: Mutual Exclusion Logic ---\n");

        // 2.1 INI load when Ampere is requested (mode key absent -> External default behavior)
        {
            MockConfig cfg;
            MockState state;
            SimulateConfigLoad(state, cfg,
                               /*iniAda=*/std::optional<bool>(true),
                               /*iniBlackwell=*/std::optional<bool>(true),
                               /*iniAmpere=*/std::optional<bool>(true));

            // Ampere wins and disables Ada; absent AmpereMfgMode keeps the historic External forcing
            assert(cfg.FGDLSSGAmpereMfgUnlock.value_or_default() == true);
            assert(cfg.FGDLSSGAdaMfgUnlock.value_or_default() == false);
            assert(cfg.ExternalFrameGeneration.value_or_default() == true);
            assert(state.ampereMfgMode == AmpereMfgMode::External); // startup default
            printf("  [PASS] 2.1 Config load with both Ada and Ampere enforces Ampere exclusivity and disables Ada\n");
        }

        // 2.1b INI load with AmpereMfgMode=Internal: no External forcing, Ada still suppressed
        {
            MockConfig cfg;
            MockState state;
            SimulateConfigLoad(state, cfg,
                               /*iniAda=*/std::optional<bool>(true),
                               /*iniBlackwell=*/std::nullopt,
                               /*iniAmpere=*/std::optional<bool>(true),
                               /*iniAmpereMode=*/std::optional<std::string>("internal"));

            assert(cfg.FGDLSSGAmpereMfgMode.value_or("External") == "Internal"); // case-insensitive parse
            assert(state.ampereMfgMode == AmpereMfgMode::Internal);               // State mirror updated
            assert(!cfg.ExternalFrameGeneration.has_value());                    // forcing did NOT fire
            assert(cfg.ExternalFrameGeneration.value_or_default() == false);
            assert(cfg.FGDLSSGAdaMfgUnlock.value_or_default() == false); // suppressed in both modes
            assert(cfg.FGDLSSGAmpereMfgUnlock.value_or_default() == true);
            printf("  [PASS] 2.1b Internal mode load keeps External FG off and still suppresses Ada\n");
        }

        // 2.1c Invalid AmpereMfgMode value falls back to External (with a warning) and keeps the
        // historic external-FG forcing (Config.cpp:81-86)
        {
            MockConfig cfg;
            MockState state;
            SimulateConfigLoad(state, cfg, std::nullopt, std::nullopt, std::optional<bool>(true),
                               std::optional<std::string>("bogus"));

            assert(cfg.FGDLSSGAmpereMfgMode.value_or("External") == "External");
            assert(state.ampereMfgMode == AmpereMfgMode::External);
            assert(cfg.ExternalFrameGeneration.value_or_default() == true);
            printf("  [PASS] 2.1c Invalid AmpereMfgMode parses to External and preserves external-FG forcing\n");
        }

        // 2.1d Absent AmpereMfgMode key stays unset (behaves as External) without touching the
        // State mirror (startup-only update, Config.cpp:88-91)
        {
            MockConfig cfg;
            MockState state;
            state.ampereMfgMode = AmpereMfgMode::Internal; // sentinel proving the mirror is untouched
            SimulateConfigLoad(state, cfg, std::nullopt, std::nullopt, std::optional<bool>(true));

            assert(!cfg.FGDLSSGAmpereMfgMode.has_value()); // key stays unset
            assert(cfg.FGDLSSGAmpereMfgMode.value_or("External") == "External"); // ...but behaves as External
            assert(state.ampereMfgMode == AmpereMfgMode::Internal);              // mirror NOT touched
            assert(cfg.ExternalFrameGeneration.value_or_default() == true);      // historic forcing fires
            printf("  [PASS] 2.1d Absent AmpereMfgMode key defaults to External without touching the State "
                   "mirror\n");
        }

        // 2.2 Config save when both flags are somehow held true (External default mode)
        {
            MockConfig cfg;
            cfg.FGDLSSGAmpereMfgUnlock = true;
            cfg.FGDLSSGAdaMfgUnlock = true;

            bool savedAda, savedBlackwell, savedAmpere, savedExternal;
            std::string savedMode;
            SimulateConfigSave(cfg, savedAda, savedBlackwell, savedAmpere, savedExternal, savedMode);

            assert(savedAmpere == true);
            assert(savedAda == false); // Ada suppressed!
            assert(savedExternal == true); // External term still forced while mode != Internal
            assert(savedMode == "External");
            printf("  [PASS] 2.2 Config save with both flags held true forcibly suppresses Ada unlock\n");
        }

        // 2.2b Config save with AmpereMfgMode=Internal: the forced-External term is mode-gated OFF
        {
            MockConfig cfg;
            cfg.FGDLSSGAmpereMfgUnlock = true;
            cfg.FGDLSSGAmpereMfgMode = std::string("Internal");

            bool savedAda, savedBlackwell, savedAmpere, savedExternal;
            std::string savedMode;
            SimulateConfigSave(cfg, savedAda, savedBlackwell, savedAmpere, savedExternal, savedMode);

            assert(savedAmpere == true);
            assert(savedExternal == false); // (ampereUnlock && mode != Internal) is false; External FG unset
            assert(savedMode == "Internal"); // the mode itself is persisted
            printf("  [PASS] 2.2b Config save under Internal mode drops the forced-External term and persists "
                   "AmpereMfgMode=Internal\n");
        }

        // 2.2c An explicit External=true ini value is preserved verbatim on save under Internal
        // mode; the conflict is auto-corrected for the session only (dllmain.cpp:1899-1906)
        {
            MockConfig cfg;
            cfg.FGDLSSGAmpereMfgUnlock = true;
            cfg.FGDLSSGAmpereMfgMode = std::string("Internal");
            cfg.ExternalFrameGeneration = true; // user explicitly set External=true in the ini

            bool savedAda, savedBlackwell, savedAmpere, savedExternal;
            std::string savedMode;
            SimulateConfigSave(cfg, savedAda, savedBlackwell, savedAmpere, savedExternal, savedMode);

            assert(savedExternal == true); // the explicit user value survives the save verbatim
            assert(savedMode == "Internal");
            printf("  [PASS] 2.2c Config save preserves an explicit External=true verbatim under Internal mode "
                   "(session auto-correct handles the conflict)\n");
        }

        // 2.3 Menu UI disableAda condition evaluation
        {
            MockGpu adaGpu;
            adaGpu.vendorId = VendorId::Nvidia;
            adaGpu.nvidiaArchInfo.architecture_id = NV_GPU_ARCHITECTURE_AD100;

            MockGpu ampereGpu;
            ampereGpu.vendorId = VendorId::Nvidia;
            ampereGpu.nvidiaArchInfo.architecture_id = NV_GPU_ARCHITECTURE_GA100;

            MockState state;
            state.externalFrameGeneration = false;

            // Scenario 2.3a: Ada GPU, no Ampere active, no external FG -> Enabled
            bool isAda = adaGpu.vendorId == VendorId::Nvidia &&
                         adaGpu.nvidiaArchInfo.architecture_id == NV_GPU_ARCHITECTURE_AD100;
            bool ampereActive = false;
            bool disableAda = !isAda || ampereActive || state.externalFrameGeneration;
            assert(!disableAda);
            printf("  [PASS] 2.3a Ada GPU with no external FG has Ada MFG checkbox enabled\n");

            // Scenario 2.3b: Ampere active -> Disabled
            ampereActive = true;
            disableAda = !isAda || ampereActive || state.externalFrameGeneration;
            assert(disableAda);
            printf("  [PASS] 2.3b Ampere active disables Ada MFG checkbox\n");

            // Scenario 2.3c: External FG active -> Disabled
            ampereActive = false;
            state.externalFrameGeneration = true;
            disableAda = !isAda || ampereActive || state.externalFrameGeneration;
            assert(disableAda);
            printf("  [PASS] 2.3c External FG active disables Ada MFG checkbox\n");

            // Scenario 2.3d: Non-Ada GPU (GA100) -> Disabled
            state.externalFrameGeneration = false;
            isAda = ampereGpu.vendorId == VendorId::Nvidia &&
                    ampereGpu.nvidiaArchInfo.architecture_id == NV_GPU_ARCHITECTURE_AD100;
            disableAda = !isAda || ampereActive || state.externalFrameGeneration;
            assert(disableAda);
            printf("  [PASS] 2.3d Non-Ada GPU disables Ada MFG checkbox\n");
        }

        // 2.4 Menu UI disableAmpere when Ada is active
        {
            bool adaActive = true;
            bool supportsDlssg = true;
            bool disableAmpere = !supportsDlssg || adaActive;
            assert(disableAmpere == true);
            printf("  [PASS] 2.4 Ada MFG active disables Ampere MFG mod checkbox\n");
        }
    }

    // =========================================================================
    // Test Suite 3: Menu Status & Blackwell Kernel Display Logic
    // =========================================================================
    {
        printf("\n--- Test Suite 3: Menu UI Status & Blackwell Kernel Reporting ---\n");

        // 3.1 Pending restart notice when UI toggled
        {
            MockMfgUnlockStatus status;
            status.ModuleFound = true;
            status.AdvertiseMatched = true;
            status.ValidateMatched = true;
            status.KernelsRewritten = 2;

            std::string text = EvaluateMenuStatusText(/*adaUnlock=*/true,
                                                      /*adaEnabledForSession=*/false, status);
            assert(text == "Save Settings and restart to apply this change.");
            printf("  [PASS] 3.1 Pending restart detected when adaUnlock != adaEnabledForSession\n");
        }

        // 3.2 Waiting for DLSSG module
        {
            MockMfgUnlockStatus status;
            status.ModuleFound = false;

            std::string text = EvaluateMenuStatusText(/*adaUnlock=*/true,
                                                      /*adaEnabledForSession=*/true, status);
            assert(text == "Waiting for DLSSG to load.");
            printf("  [PASS] 3.2 Waiting message shown when DLSSG module not yet loaded\n");
        }

        // 3.3 Successful unlock with Blackwell kernels
        {
            MockMfgUnlockStatus status;
            status.ModuleFound = true;
            status.AdvertiseMatched = true;
            status.ValidateMatched = true;
            status.KernelsRewritten = 2;
            status.SnippetVersion = "310.9";

            std::string text = EvaluateMenuStatusText(/*adaUnlock=*/true,
                                                      /*adaEnabledForSession=*/true, status);
            assert(text == "DLSSG 310.9: RTX 40 MFG unlock applied with Blackwell kernels (2 containers).");
            assert(text.find("with Blackwell kernels") != std::string::npos);
            printf("  [PASS] 3.3 Menu correctly formats status with Blackwell kernels and container count\n");
        }

        // 3.4 Successful unlock with stock Ada kernels
        {
            MockMfgUnlockStatus status;
            status.ModuleFound = true;
            status.AdvertiseMatched = true;
            status.ValidateMatched = true;
            status.KernelsRewritten = 0;
            status.SnippetVersion = "310.9";

            std::string text = EvaluateMenuStatusText(/*adaUnlock=*/true,
                                                      /*adaEnabledForSession=*/true, status);
            assert(text == "DLSSG 310.9: RTX 40 MFG unlock applied (stock Ada kernels).");
            assert(text.find("stock Ada kernels") != std::string::npos);
            printf("  [PASS] 3.4 Menu correctly formats status with stock Ada kernels when KernelsRewritten == 0\n");
        }

        // 3.5 Runtime version unsupported / signature mismatch
        {
            MockMfgUnlockStatus status;
            status.ModuleFound = true;
            status.AdvertiseMatched = false;
            status.ValidateMatched = false;
            status.SnippetVersion = "310.1";

            std::string text = EvaluateMenuStatusText(/*adaUnlock=*/true,
                                                      /*adaEnabledForSession=*/true, status);
            assert(text == "DLSSG 310.1: unlock unavailable for this runtime.");
            assert(text.find("unlock unavailable") != std::string::npos);
            printf("  [PASS] 3.5 Menu correctly reports unlock unavailable for unsupported runtimes\n");
        }
    }

    // =========================================================================
    // Test Suite 4: AmpereMfgMode-aware Menu Behavior (commit 7f1c15fc)
    // =========================================================================
    {
        printf("\n--- Test Suite 4: AmpereMfgMode Menu Locks, Toggle Side-Effects & Combo ---\n");

        // 4.1 Locked External checkbox display value (menu_common.cpp:3204-3219)
        {
            MockConfig cfgExternal; // mode unset -> External
            assert(EvaluateExternalCheckboxValue(/*ampereActive=*/true, cfgExternal) == true);

            MockConfig cfgInternal;
            cfgInternal.FGDLSSGAmpereMfgMode = std::string("Internal");
            assert(EvaluateExternalCheckboxValue(/*ampereActive=*/true, cfgInternal) == false);

            MockConfig cfgFree; // unlock not active -> free (user-controlled) checkbox
            cfgFree.ExternalFrameGeneration = true;
            assert(EvaluateExternalCheckboxValue(/*ampereActive=*/false, cfgFree) == true);
            printf("  [PASS] 4.1 External checkbox locked ON in External mode, locked OFF under Internal mode\n");
        }

        // 4.2 Ampere unlock toggle side-effects (menu_common.cpp:3337-3350)
        {
            MockConfig cfg;
            ApplyAmpereToggleSideEffects(cfg, /*ampereUnlock=*/true);
            assert(cfg.ExternalFrameGeneration.value_or_default() == true); // forced on (External default)
            assert(cfg.FGDLSSGAdaMfgUnlock.value_or_default() == false);    // Ada cleared

            MockConfig cfgInt;
            cfgInt.FGDLSSGAmpereMfgMode = std::string("Internal");
            ApplyAmpereToggleSideEffects(cfgInt, /*ampereUnlock=*/true);
            assert(!cfgInt.ExternalFrameGeneration.has_value());            // Internal: External FG stays off
            assert(cfgInt.FGDLSSGAdaMfgUnlock.value_or_default() == false); // Ada still cleared
            printf("  [PASS] 4.2 Enabling Ampere forces External FG only in External mode and clears Ada\n");
        }

        // 4.3 Mode combo stores canonical strings (menu_common.cpp:3412-3421)
        {
            MockConfig cfg;
            ApplyAmpereModeCombo(cfg, 0);
            assert(cfg.FGDLSSGAmpereMfgMode.value_or("External") == "External");
            ApplyAmpereModeCombo(cfg, 1);
            assert(cfg.FGDLSSGAmpereMfgMode.value_or("External") == "Internal");
            printf("  [PASS] 4.3 Ampere MFG Mode combo persists canonical External/Internal values\n");
        }

        // 4.4 Startup-only restart hint (menu_common.cpp:3429-3432)
        {
            MockConfig cfg;
            MockState state; // session still External (started before the mode change)

            ApplyAmpereModeCombo(cfg, 1); // user switches the saved config to Internal
            assert(EvaluateAmpereModeRestartHint(cfg, state) == true);

            // "Restart": Config::Reload mirrors the parsed mode into State (Config.cpp:88-91)
            SimulateConfigLoad(state, cfg, std::nullopt, std::nullopt, std::optional<bool>(true),
                               std::optional<std::string>("internal"));
            assert(EvaluateAmpereModeRestartHint(cfg, state) == false);

            ApplyAmpereModeCombo(cfg, 0); // and back to External, again pending until restart
            assert(EvaluateAmpereModeRestartHint(cfg, state) == true);
            printf("  [PASS] 4.4 Restart hint shown only while the config mode differs from the session mode\n");
        }
    }

    printf("\n=== All Ada MFG & Blackwell Kernels Unit Tests PASSED! ===\n");
    return 0;
}

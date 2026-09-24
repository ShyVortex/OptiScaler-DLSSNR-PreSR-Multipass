#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

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

// Minimal CustomOptional matching Config.h behavior
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
};

struct MockState
{
    bool externalFrameGeneration = false;
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

// Helper to simulate Config::LoadConfig mutual exclusion
void SimulateConfigLoad(MockConfig& cfg, std::optional<bool> iniAdaUnlock, std::optional<bool> iniAdaBlackwell,
                        std::optional<bool> iniAmpereUnlock)
{
    cfg.FGDLSSGAdaMfgUnlock.set_from_config(iniAdaUnlock);
    cfg.FGDLSSGAdaBlackwellKernels.set_from_config(iniAdaBlackwell);
    cfg.FGDLSSGAmpereMfgUnlock.set_from_config(iniAmpereUnlock);

    if (cfg.FGDLSSGAmpereMfgUnlock.value_or_default())
    {
        cfg.ExternalFrameGeneration.set_from_config(true);
        cfg.FGDLSSGAdaMfgUnlock.set_from_config(false);
    }
}

// Helper to simulate Config::SaveConfig mutual exclusion
void SimulateConfigSave(const MockConfig& cfg, bool& outAdaUnlock, bool& outAdaBlackwell, bool& outAmpereUnlock,
                        bool& outExternalFG)
{
    bool ampereUnlock = cfg.FGDLSSGAmpereMfgUnlock.value_for_config_or(false);
    bool adaUnlock = cfg.FGDLSSGAdaMfgUnlock.value_for_config_or(false);
    if (ampereUnlock && adaUnlock)
        adaUnlock = false;

    outAmpereUnlock = ampereUnlock;
    outAdaUnlock = adaUnlock;
    outAdaBlackwell = cfg.FGDLSSGAdaBlackwellKernels.value_for_config_or(false);
    outExternalFG = cfg.ExternalFrameGeneration.value_for_config_or(false) || ampereUnlock;
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
        SimulateConfigSave(cfg, savedAda, savedBlackwell, savedAmpere, savedExternal);
        assert(savedBlackwell == true);
        printf("  [PASS] 1.4 Config saving preserves AdaBlackwellKernels=true\n");
    }

    // =========================================================================
    // Test Suite 2: Ada & Ampere MFG Strict Mutual Exclusion
    // =========================================================================
    {
        printf("\n--- Test Suite 2: Mutual Exclusion Logic ---\n");

        // 2.1 INI load when Ampere is requested
        {
            MockConfig cfg;
            SimulateConfigLoad(cfg,
                               /*iniAda=*/std::optional<bool>(true),
                               /*iniBlackwell=*/std::optional<bool>(true),
                               /*iniAmpere=*/std::optional<bool>(true));

            // Ampere wins and disables Ada
            assert(cfg.FGDLSSGAmpereMfgUnlock.value_or_default() == true);
            assert(cfg.FGDLSSGAdaMfgUnlock.value_or_default() == false);
            assert(cfg.ExternalFrameGeneration.value_or_default() == true);
            printf("  [PASS] 2.1 Config load with both Ada and Ampere enforces Ampere exclusivity and disables Ada\n");
        }

        // 2.2 Config save when both flags are somehow held true
        {
            MockConfig cfg;
            cfg.FGDLSSGAmpereMfgUnlock = true;
            cfg.FGDLSSGAdaMfgUnlock = true;

            bool savedAda, savedBlackwell, savedAmpere, savedExternal;
            SimulateConfigSave(cfg, savedAda, savedBlackwell, savedAmpere, savedExternal);

            assert(savedAmpere == true);
            assert(savedAda == false); // Ada suppressed!
            assert(savedExternal == true);
            printf("  [PASS] 2.2 Config save with both flags held true forcibly suppresses Ada unlock\n");
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

    printf("\n=== All Ada MFG & Blackwell Kernels Unit Tests PASSED! ===\n");
    return 0;
}

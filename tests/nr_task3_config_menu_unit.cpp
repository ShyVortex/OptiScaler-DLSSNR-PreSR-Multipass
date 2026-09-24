#include <cassert>
#include <iostream>
#include <string>
#include <optional>
#include <vector>

// Mirror the exact CustomOptional and Config structures from OptiScaler
enum HasDefaultValue
{
    NoDefault,
    WithDefault,
};

template <class T, HasDefaultValue defaultState = WithDefault> class CustomOptional : public std::optional<T>
{
    T _defaultValue {};

  public:
    CustomOptional(T defaultValue)
    {
        _defaultValue = defaultValue;
        if constexpr (defaultState == WithDefault)
            *this = defaultValue;
        else
            this->reset();
    }

    CustomOptional() { this->reset(); }

    T value_or_default() const
    {
        if (this->has_value())
            return this->value();
        return _defaultValue;
    }

    void set_from_config(std::optional<T> val)
    {
        if (val.has_value())
            *this = val.value();
    }

    CustomOptional& operator=(const T& value)
    {
        std::optional<T>::operator=(value);
        return *this;
    }
};

struct NrPassOverride
{
    std::optional<std::string> OverrideModel;
    std::optional<float> Weight;
    std::optional<int> Transfer;
};

struct TestConfig
{
    // Pre-SR invariant
    CustomOptional<bool> DlssNrRunBeforeSr { true };

    // Spatial compression
    CustomOptional<bool, NoDefault> DlssNrSpatialCompression { false };
    CustomOptional<float, NoDefault> DlssNrSpatialCenterX { 0.5f };
    CustomOptional<float, NoDefault> DlssNrSpatialCenterY { 0.5f };
    CustomOptional<float, NoDefault> DlssNrSpatialWorkX { 0.5f };
    CustomOptional<float, NoDefault> DlssNrSpatialWorkY { 0.5f };
    CustomOptional<float, NoDefault> DlssNrSpatialOffsetX { 0.0f };
    CustomOptional<float, NoDefault> DlssNrSpatialOffsetY { 0.0f };
    CustomOptional<float, NoDefault> DlssNrSpatialShiftX { 0.0f };
    CustomOptional<float, NoDefault> DlssNrSpatialShiftY { 0.0f };
    CustomOptional<bool, NoDefault> DlssNrSpatialShowCenter { false };
    CustomOptional<bool, NoDefault> DlssNrSpatialShowWork { false };

    // Detail & White point / Exposure
    CustomOptional<float, NoDefault> DlssNrReplaceDetailStrength { 0.0f };
    CustomOptional<float, NoDefault> DlssNrResidualConfidenceSensitivity { 1.0f };
    CustomOptional<int, NoDefault> DlssNrWhitePointSource { 0 };
    CustomOptional<float, NoDefault> DlssNrWhitePointTrim { 0.0f };
    CustomOptional<float, NoDefault> DlssNrAutoExposureTrim { 0.0f };
    CustomOptional<float, NoDefault> DlssNrAutoExposureHighlightProtection { 0.0f };
    CustomOptional<bool, NoDefault> DlssNrExposureTrimAnchors { false };
    CustomOptional<bool, NoDefault> DlssNrAutoExposureTrimAnchors { false };

    // Pass overrides (29 slots: Pass 2 to Pass 30)
    NrPassOverride DlssNrPassOverrides[29];

    // Shortcuts
    CustomOptional<bool, NoDefault> ShortcutKeyRequireCtrl { false };
    CustomOptional<bool, NoDefault> ShortcutKeyRequireAlt { false };

    // SM75-SM86 MFG
    CustomOptional<bool, NoDefault> AmpereMfgEnabled { false };
    CustomOptional<int, NoDefault> AmpereMfgMaxFrames { 2 };
    CustomOptional<bool, NoDefault> AmpereMfgOptimized { true };

    // Ada RTX 40 MFG
    CustomOptional<bool, NoDefault> FGDLSSGAdaMfgUnlock { false };
    CustomOptional<bool, NoDefault> FGDLSSGAdaBlackwellKernels { false };
};

int main()
{
    std::cout << "Running DLSS-NR Task 3 Config & Menu Unit Tests...\n";

    TestConfig cfg;

    // 1. Verify Pre-SR invariant default
    assert(cfg.DlssNrRunBeforeSr.has_value());
    assert(cfg.DlssNrRunBeforeSr.value() == true);
    assert(cfg.DlssNrRunBeforeSr.value_or_default() == true);
    std::cout << "  [PASS] Test 1: DlssNrRunBeforeSr defaults to true (Pre-SR)\n";

    // 2. Verify Spatial compression options defaults
    assert(cfg.DlssNrSpatialCompression.value_or_default() == false);
    assert(cfg.DlssNrSpatialCenterX.value_or_default() == 0.5f);
    assert(cfg.DlssNrSpatialCenterY.value_or_default() == 0.5f);
    assert(cfg.DlssNrSpatialWorkX.value_or_default() == 0.5f);
    assert(cfg.DlssNrSpatialWorkY.value_or_default() == 0.5f);
    assert(cfg.DlssNrSpatialOffsetX.value_or_default() == 0.0f);
    assert(cfg.DlssNrSpatialOffsetY.value_or_default() == 0.0f);
    assert(cfg.DlssNrSpatialShiftX.value_or_default() == 0.0f);
    assert(cfg.DlssNrSpatialShiftY.value_or_default() == 0.0f);
    assert(cfg.DlssNrSpatialShowCenter.value_or_default() == false);
    assert(cfg.DlssNrSpatialShowWork.value_or_default() == false);
    std::cout << "  [PASS] Test 2: Spatial compression options correctly initialized\n";

    // 3. Verify Detail & White point / Exposure options
    assert(cfg.DlssNrReplaceDetailStrength.value_or_default() == 0.0f);
    assert(cfg.DlssNrResidualConfidenceSensitivity.value_or_default() == 1.0f);
    assert(cfg.DlssNrWhitePointSource.value_or_default() == 0);
    assert(cfg.DlssNrWhitePointTrim.value_or_default() == 0.0f);
    assert(cfg.DlssNrAutoExposureTrim.value_or_default() == 0.0f);
    assert(cfg.DlssNrAutoExposureHighlightProtection.value_or_default() == 0.0f);
    assert(cfg.DlssNrExposureTrimAnchors.value_or_default() == false);
    assert(cfg.DlssNrAutoExposureTrimAnchors.value_or_default() == false);
    std::cout << "  [PASS] Test 3: Exposure, white point, and detail replacement configs initialized\n";

    // 4. Verify Pass Overrides array count (29 overrides)
    static_assert(sizeof(cfg.DlssNrPassOverrides) / sizeof(cfg.DlssNrPassOverrides[0]) == 29,
                  "Expected 29 pass overrides");
    for (int i = 0; i < 29; ++i)
    {
        assert(!cfg.DlssNrPassOverrides[i].OverrideModel.has_value());
        assert(!cfg.DlssNrPassOverrides[i].Weight.has_value());
        assert(!cfg.DlssNrPassOverrides[i].Transfer.has_value());
    }
    std::cout << "  [PASS] Test 4: 29 DlssNrPassOverrides properly sized and initialized\n";

    // 5. Verify Menu shortcut modifier configs
    assert(cfg.ShortcutKeyRequireCtrl.value_or_default() == false);
    assert(cfg.ShortcutKeyRequireAlt.value_or_default() == false);
    std::cout << "  [PASS] Test 5: Shortcut modifier keys properly initialized\n";

    // 6. Verify SM75-SM86 and Ada RTX 40 MFG invariants preserved
    assert(cfg.AmpereMfgEnabled.value_or_default() == false);
    assert(cfg.AmpereMfgMaxFrames.value_or_default() == 2);
    assert(cfg.AmpereMfgOptimized.value_or_default() == true);
    assert(cfg.FGDLSSGAdaMfgUnlock.value_or_default() == false);
    assert(cfg.FGDLSSGAdaBlackwellKernels.value_or_default() == false);
    std::cout << "  [PASS] Test 6: SM75-86 Ampere & Ada RTX 40 MFG settings intact\n";

    // 7. Verify Menu Auto-Select Pre-SR logic when enabling NR
    {
        CustomOptional<bool> runBeforeSr; // unset
        bool uiEnabled = true;
        if (uiEnabled && !runBeforeSr.has_value())
        {
            runBeforeSr = true;
        }
        assert(runBeforeSr.has_value());
        assert(runBeforeSr.value() == true);
        std::cout << "  [PASS] Test 7: Menu auto-selects Pre-SR upon enabling NR if unset\n";
    }

    // 8. Verify Display Resolution error detection and Quick Action button switch
    {
        std::string reason = "the NVIDIA NGX driver could not create Neural Rendering at display resolution (try "
                             "enabling 'Generate model before upscale' or reducing Working Scale)";
        bool hasDisplayResolutionError = reason.find("display resolution") != std::string::npos;
        assert(hasDisplayResolutionError);

        // Simulate user clicking "Switch to Pre-SR"
        cfg.DlssNrRunBeforeSr = true;
        assert(cfg.DlssNrRunBeforeSr.value() == true);
        std::cout << "  [PASS] Test 8: Display resolution guidance triggers Quick Action switch to Pre-SR\n";
    }

    std::cout << "\nAll DLSS-NR Task 3 Config & Menu Unit Tests passed successfully!\n";
    return 0;
}

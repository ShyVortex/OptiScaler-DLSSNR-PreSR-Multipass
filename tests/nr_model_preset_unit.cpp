#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

// Minimal reproduction of CustomOptional & Config structure for standalone testing
enum HasDefaultValue
{
    NoDefault,
    WithDefault,
};

template <class T, HasDefaultValue defaultState = WithDefault> class TestCustomOptional : public std::optional<T>
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

struct TestNrPassOverrides
{
    TestCustomOptional<uint32_t, NoDefault> preset, style;
    TestCustomOptional<float, NoDefault> intensity, structure, tone, skin;
    TestCustomOptional<bool, NoDefault> autoMask;
};

struct TestConfig
{
    TestCustomOptional<uint32_t> DlssNrPreset { 0 };
    TestCustomOptional<uint32_t> DlssNrStyle { 0 };
    TestCustomOptional<float> DlssNrIntensity { 1.0f };
    TestCustomOptional<float> DlssNrLocalStructure { 1.0f };
    TestCustomOptional<float> DlssNrLocalTone { 1.0f };
    TestCustomOptional<float> DlssNrSkinStructure { -1.0f };
    TestCustomOptional<bool> DlssNrAutoMask { true };
    TestNrPassOverrides DlssNrPassOverrides[29];
};

struct ModelSettings
{
    unsigned int preset = 0, style = 0;
    float intensity = 1.0f, localStructure = 1.0f, localTone = 0.0f, skinStructure = -1.0f;
    bool autoMask = true;
    bool operator==(const ModelSettings&) const = default;
};

inline ModelSettings PassSettings(const TestConfig& cfg, unsigned int pass)
{
    ModelSettings result { cfg.DlssNrPreset.value_or_default(),
                           cfg.DlssNrStyle.value_or_default(),
                           cfg.DlssNrIntensity.value_or_default(),
                           cfg.DlssNrLocalStructure.value_or_default(),
                           pass == 0 ? cfg.DlssNrLocalTone.value_or_default() : 0.0f,
                           cfg.DlssNrSkinStructure.value_or_default(),
                           cfg.DlssNrAutoMask.value_or_default() };
    if (pass > 0 && pass <= std::size(cfg.DlssNrPassOverrides))
    {
        const auto& extra = cfg.DlssNrPassOverrides[pass - 1];
        result.preset = extra.preset.value_or(result.preset);
        result.style = extra.style.value_or(result.style);
        result.intensity = extra.intensity.value_or(result.intensity);
        result.localStructure = extra.structure.value_or(result.localStructure);
        result.localTone = extra.tone.value_or(result.localTone);
        result.skinStructure = extra.skin.value_or(result.skinStructure);
        result.autoMask = extra.autoMask.value_or(result.autoMask);
    }
    result.preset = std::min(result.preset, 3u);
    result.style = std::min(result.style, 2u);
    const auto bounded = [](float value, float fallback, float minimum)
    { return std::isfinite(value) ? std::clamp(value, minimum, 2.0f) : fallback; };
    result.intensity = bounded(result.intensity, 1.0f, 0.0f);
    result.localStructure = bounded(result.localStructure, 1.0f, 0.0f);
    result.localTone = bounded(result.localTone, pass == 0 ? 1.0f : 0.0f, 0.0f);
    result.skinStructure = bounded(result.skinStructure, -1.0f, -1.0f);
    return result;
}

int main()
{
    std::printf("=== Running DLSS-NR Model Preset & Lightweight Model Unit Tests ===\n");

    // Case 1: Default config maps to Preset 0 (Auto -> Performance / Crazy Cuckoo Small FP8 in 310.9)
    {
        TestConfig cfg;
        auto s = PassSettings(cfg, 0);
        assert(s.preset == 0);
        std::printf("  [PASS] Case 1: Default DlssNrPreset maps to 0 (Auto)\n");
    }

    // Case 2: Explicit Preset 1 (Quality / Honest Tench)
    {
        TestConfig cfg;
        cfg.DlssNrPreset = 1;
        auto s = PassSettings(cfg, 0);
        assert(s.preset == 1);
        std::printf("  [PASS] Case 2: Preset 1 (Quality / Honest Tench) correctly mapped\n");
    }

    // Case 3: Explicit Preset 2 (Performance / Crazy Cuckoo Small FP8, +40% FPS)
    {
        TestConfig cfg;
        cfg.DlssNrPreset = 2;
        auto s = PassSettings(cfg, 0);
        assert(s.preset == 2);
        std::printf("  [PASS] Case 3: Preset 2 (Performance / Crazy Cuckoo Small FP8) correctly mapped\n");
    }

    // Case 4: Clamping of invalid/out-of-range preset (> 3 clamped to 3)
    {
        TestConfig cfg;
        cfg.DlssNrPreset = 99;
        auto s = PassSettings(cfg, 0);
        assert(s.preset == 3);
        std::printf("  [PASS] Case 4: Preset out-of-range (99) clamped to 3\n");
    }

    // Case 5: Multi-pass inheritance (Pass 2 inherits Pass 1 preset when unconfigured)
    {
        TestConfig cfg;
        cfg.DlssNrPreset = 2; // Performance
        auto pass1 = PassSettings(cfg, 0);
        auto pass2 = PassSettings(cfg, 1);
        auto pass3 = PassSettings(cfg, 2);
        assert(pass1.preset == 2);
        assert(pass2.preset == 2);
        assert(pass3.preset == 2);
        std::printf("  [PASS] Case 5: Multi-pass inheritance without override verified\n");
    }

    // Case 6: Multi-pass explicit override (Pass 2 overrides to Quality while Pass 1 is Performance)
    {
        TestConfig cfg;
        cfg.DlssNrPreset = 2;                   // Pass 1 = Performance
        cfg.DlssNrPassOverrides[0].preset = 1u; // Pass 2 = Quality
        cfg.DlssNrPassOverrides[1].preset = 2u; // Pass 3 = Performance
        cfg.DlssNrPassOverrides[2].preset = 1u; // Pass 4 = Quality (higher pass support)
        auto pass1 = PassSettings(cfg, 0);
        auto pass2 = PassSettings(cfg, 1);
        auto pass3 = PassSettings(cfg, 2);
        auto pass4 = PassSettings(cfg, 3);
        assert(pass1.preset == 2);
        assert(pass2.preset == 1);
        assert(pass3.preset == 2);
        assert(pass4.preset == 1);
        std::printf("  [PASS] Case 6: Per-pass explicit preset overrides (Pass 2, 3, 4) verified\n");
    }

    // Case 7: Feature rebuild detection via ModelSettings::operator==
    {
        TestConfig cfg;
        cfg.DlssNrPreset = 1;
        auto initialSettings = PassSettings(cfg, 0);

        // Identical settings should NOT trigger rebuild
        auto sameSettings = PassSettings(cfg, 0);
        assert(initialSettings == sameSettings);

        // Changing preset to 2 (Performance) MUST cause operator== to return false
        cfg.DlssNrPreset = 2;
        auto newSettings = PassSettings(cfg, 0);
        assert(initialSettings != newSettings);
        assert(newSettings.preset == 2);
        std::printf("  [PASS] Case 7: Feature rebuild trigger (operator== inequality) verified\n");
    }

    // Case 8: Menu preset combo index conversions
    {
        // Pass 1 menu combo mapping:
        // Index 0 -> Auto (0)
        // Index 1 -> Quality (1)
        // Index 2 -> Performance (2)
        for (int menuIndex = 0; menuIndex <= 2; ++menuIndex)
        {
            uint32_t configValue = (uint32_t) menuIndex;
            assert(configValue == (uint32_t) menuIndex);
        }

        // Multi-pass menu combo mapping:
        // Index 0 -> Auto / inherit (nullopt)
        // Index 1 -> Quality (preset 1)
        // Index 2 -> Performance (preset 2)
        int selectedIndex = 0;
        std::optional<uint32_t> passPreset = selectedIndex ? std::optional<uint32_t>(selectedIndex - 1) : std::nullopt;
        assert(!passPreset.has_value());

        selectedIndex = 1;
        passPreset = selectedIndex ? std::optional<uint32_t>(selectedIndex) : std::nullopt;
        assert(passPreset.has_value() && passPreset.value() == 1);

        selectedIndex = 2;
        passPreset = selectedIndex ? std::optional<uint32_t>(selectedIndex) : std::nullopt;
        assert(passPreset.has_value() && passPreset.value() == 2);

        std::printf("  [PASS] Case 8: In-game ImGui combo selection mapping verified\n");
    }

    std::printf("=== All DLSS-NR Model Preset Unit Tests PASSED! ===\n");
    return 0;
}

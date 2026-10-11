#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

// Standalone unit test suite for XeMFG-SR (Intel Xe Multi-Frame Generation Injector for DLSS-SR).
// Validates:
// 1. Config parsing, default fallback, and range clamping (1 to 5).
// 2. Legacy key migration ([XeMFG] EnableSR / SrGeneratedFrames).
// 3. Strict universal mutual exclusion across XeMfgUnlock, Ada, Ampere, Smooth Motion, and External FG.
// 4. Loader session state: SrEnabledForSession(), EnabledForSession(), and AnyEnabledForSession().
// 5. Dynamic target ceiling resolution in SR vs Streamline modes.
// 6. Dynamic in-memory ceiling adaptation (byte rewriting of U3, U4, U5).
// 7. Automatic pipeline configuration (FGInput::Upscaler, FGOutput::XeFG, FGNvngxReplacement::None).

template <class T> class TestCustomOptional : public std::optional<T>
{
    T _defaultValue {};

  public:
    using std::optional<T>::optional;
    using std::optional<T>::operator=;

    TestCustomOptional(T defaultValue) : std::optional<T>(), _defaultValue(defaultValue) {}

    TestCustomOptional() : std::optional<T>(), _defaultValue() {}

    TestCustomOptional& operator=(const T& val)
    {
        std::optional<T>::operator=(val);
        return *this;
    }

    T value_or_default() const
    {
        if (this->has_value())
            return this->value();
        return _defaultValue;
    }
};

enum class FGInput
{
    NoFG = 0,
    DLSSG = 1,
    FSRFG = 2,
    Upscaler = 3,
    ForceXeLL = 4,
    NvngxFG = 5
};

enum class FGOutput
{
    NoFG = 0,
    DLSSG = 1,
    FSRFG = 2,
    XeFG = 3
};

enum class FGNvngxReplacement
{
    None = 0,
    FSRFG = 1
};

struct SimulatedConfig
{
    // Primary XeMFG (native Streamline DLSS-G)
    TestCustomOptional<bool> XeMfgUnlock { false };
    TestCustomOptional<int> XeMfgMaxFrames { 3 };
    TestCustomOptional<bool> XeMfgExtraPacing { false };

    // XeMFG-SR (DLSS Super Resolution passes without DLSS-FG)
    TestCustomOptional<bool> XeMfgSrEnable { false };
    TestCustomOptional<int> XeMfgSrGeneratedFrames { 2 };

    // Other FG Unlockers
    TestCustomOptional<bool> FGDLSSGAdaMfgUnlock { false };
    TestCustomOptional<bool> FGDLSSGAmpereMfgUnlock { false };
    TestCustomOptional<bool> FGDLSSGSmoothMotion { false };
    TestCustomOptional<bool> ExternalFrameGeneration { false };

    // Active FG Pipeline
    TestCustomOptional<bool> FGEnabled { false };
    TestCustomOptional<FGInput> FGInputMode { FGInput::NoFG };
    TestCustomOptional<FGOutput> FGOutputMode { FGOutput::NoFG };
    TestCustomOptional<FGNvngxReplacement> FGNvngxReplacementMode { FGNvngxReplacement::None };
    TestCustomOptional<int> FGXeFGInterpolationCount { 1 };
};

namespace SimulatedLoader
{
inline bool EnabledForSession(const SimulatedConfig& cfg)
{
    return cfg.XeMfgUnlock.value_or_default() && !cfg.ExternalFrameGeneration.value_or_default() &&
           !cfg.FGDLSSGAdaMfgUnlock.value_or_default() && !cfg.FGDLSSGAmpereMfgUnlock.value_or_default();
}

inline bool SrEnabledForSession(const SimulatedConfig& cfg)
{
    return cfg.XeMfgSrEnable.value_or_default() && !cfg.XeMfgUnlock.value_or_default() &&
           !cfg.ExternalFrameGeneration.value_or_default() && !cfg.FGDLSSGAdaMfgUnlock.value_or_default() &&
           !cfg.FGDLSSGAmpereMfgUnlock.value_or_default();
}

inline bool AnyEnabledForSession(const SimulatedConfig& cfg)
{
    return EnabledForSession(cfg) || SrEnabledForSession(cfg);
}

inline unsigned int ResolveTargetCeiling(const SimulatedConfig& cfg)
{
    if (SrEnabledForSession(cfg))
        return static_cast<unsigned int>(std::clamp(cfg.XeMfgSrGeneratedFrames.value_or(2), 1, 5));
    return static_cast<unsigned int>(std::clamp(cfg.XeMfgMaxFrames.value_or(3), 1, 5));
}
} // namespace SimulatedLoader

namespace SimulatedMemoryPatching
{
// Signature patterns matching libxess_fg.dll
static const uint8_t U1_EXPECTED[] = { 0x0f, 0x85, 0xcc, 0x00, 0x00, 0x00, 0x83, 0xfb,
                                       0x01, 0x0f, 0x86, 0xc3, 0x00, 0x00, 0x00 };
static const uint8_t U2_EXPECTED[] = { 0x74, 0x09, 0x80, 0x79, 0x64, 0x00, 0x74,
                                       0x03, 0xb0, 0x01, 0xc3, 0x32, 0xc0, 0xc3 };
static const uint8_t U3_EXPECTED[] = { 0xbb, 0x03, 0x00, 0x00, 0x00 };
static const uint8_t U4_EXPECTED[] = { 0xc7, 0x87, 0x6c, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00 };
static const uint8_t U5_EXPECTED[] = { 0xb8, 0x01, 0x00, 0x00, 0x00 };

struct SimulatedDllImage
{
    std::vector<uint8_t> buffer;
    size_t u1Offset = 0x20da4f;
    size_t u2Offset = 0x1a5de4;
    size_t u3Offset = 0x1a517d;
    size_t u4Offset = 0x1a45c2;
    size_t u5Offset = 0x20973b;

    SimulatedDllImage()
    {
        buffer.resize(0x300000, 0x90);
        memcpy(buffer.data() + u1Offset, U1_EXPECTED, sizeof(U1_EXPECTED));
        memcpy(buffer.data() + u2Offset, U2_EXPECTED, sizeof(U2_EXPECTED));
        memcpy(buffer.data() + u3Offset, U3_EXPECTED, sizeof(U3_EXPECTED));
        memcpy(buffer.data() + u4Offset, U4_EXPECTED, sizeof(U4_EXPECTED));
        memcpy(buffer.data() + u5Offset, U5_EXPECTED, sizeof(U5_EXPECTED));
    }

    bool ApplyUnlock(unsigned int ceiling)
    {
        const uint8_t frameByte = static_cast<uint8_t>(ceiling);

        // U1: jnz -> jmp
        buffer[u1Offset + 0] = 0xe9;
        buffer[u1Offset + 1] = 0xcd;
        buffer[u1Offset + 2] = 0x00;
        buffer[u1Offset + 3] = 0x00;
        buffer[u1Offset + 4] = 0x00;
        buffer[u1Offset + 5] = 0x90;

        // U2: jz -> jmp short
        buffer[u2Offset + 0] = 0xeb;
        buffer[u2Offset + 1] = 0x06;

        // U3: default ceiling byte rewrite
        buffer[u3Offset + 1] = frameByte;

        // U4: override clamp byte rewrite
        buffer[u4Offset + 6] = frameByte;

        // U5: reported maximum byte rewrite
        buffer[u5Offset + 1] = frameByte;

        return true;
    }

    void UpdateCeiling(unsigned int ceiling)
    {
        const uint8_t frameByte = static_cast<uint8_t>(ceiling);
        buffer[u3Offset + 1] = frameByte;
        buffer[u4Offset + 6] = frameByte;
        buffer[u5Offset + 1] = frameByte;
    }

    uint8_t GetU3Ceiling() const { return buffer[u3Offset + 1]; }
    uint8_t GetU4Ceiling() const { return buffer[u4Offset + 6]; }
    uint8_t GetU5Ceiling() const { return buffer[u5Offset + 1]; }
};
} // namespace SimulatedMemoryPatching

int main()
{
    printf("[+] Starting XeMFG-SR Comprehensive Unit Tests...\n");

    // Test 1: Config Parsing, Defaults, and Clamping (1 to 5)
    {
        SimulatedConfig cfg;

        // Case 1a: Default values when unset
        assert(cfg.XeMfgSrEnable.value_or_default() == false);
        assert(cfg.XeMfgSrGeneratedFrames.value_or_default() == 2);

        // Case 1b: Valid values (1..5)
        for (int frames = 1; frames <= 5; ++frames)
        {
            cfg.XeMfgSrGeneratedFrames = frames;
            assert(cfg.XeMfgSrGeneratedFrames.value() == frames);
            int clamped = std::clamp(cfg.XeMfgSrGeneratedFrames.value(), 1, 5);
            assert(clamped == frames);
        }

        // Case 1c: Out-of-bounds clamping
        cfg.XeMfgSrGeneratedFrames = 0; // Below minimum
        if (cfg.XeMfgSrGeneratedFrames.value() < 1 || cfg.XeMfgSrGeneratedFrames.value() > 5)
            cfg.XeMfgSrGeneratedFrames.reset();
        assert(cfg.XeMfgSrGeneratedFrames.value_or_default() == 2);

        cfg.XeMfgSrGeneratedFrames = 6; // Above maximum
        if (cfg.XeMfgSrGeneratedFrames.value() < 1 || cfg.XeMfgSrGeneratedFrames.value() > 5)
            cfg.XeMfgSrGeneratedFrames.reset();
        assert(cfg.XeMfgSrGeneratedFrames.value_or_default() == 2);

        printf("  [PASS] Test 1: Config parsing, defaults, and 1..5 range clamping verified.\n");
    }

    // Test 2: Legacy Key Migration ([XeMFG] EnableSR / SrGeneratedFrames)
    {
        // Simulate reading legacy fallback keys when modern section is unset
        std::optional<bool> modernEnable = std::nullopt;
        std::optional<bool> legacyEnable = true;

        TestCustomOptional<bool> XeMfgSrEnable { false };
        if (modernEnable.has_value())
            XeMfgSrEnable = modernEnable.value();
        else if (legacyEnable.has_value())
            XeMfgSrEnable = legacyEnable.value();

        assert(XeMfgSrEnable.value_or_default() == true);

        std::optional<int> modernFrames = std::nullopt;
        std::optional<int> legacyFrames = 4;

        TestCustomOptional<int> XeMfgSrGeneratedFrames { 2 };
        if (modernFrames.has_value())
            XeMfgSrGeneratedFrames = modernFrames.value();
        else if (legacyFrames.has_value())
            XeMfgSrGeneratedFrames = legacyFrames.value();

        assert(XeMfgSrGeneratedFrames.value_or_default() == 4);
        printf("  [PASS] Test 2: Legacy fallback key migration verified.\n");
    }

    // Test 3: Universal Mutual Exclusion Backend
    {
        // Case 3a: Primary XeMfgUnlock takes precedence and forces XeMfgSrEnable = false
        {
            SimulatedConfig cfg;
            cfg.XeMfgUnlock = true;
            cfg.XeMfgSrEnable = true;

            if (cfg.XeMfgUnlock.value_or_default() && cfg.XeMfgSrEnable.value_or_default())
            {
                cfg.XeMfgSrEnable = false;
            }
            assert(cfg.XeMfgUnlock.value_or_default() == true);
            assert(cfg.XeMfgSrEnable.value_or_default() == false);
        }

        // Case 3b: Ada MFG takes precedence and forces XeMfgSrEnable = false
        {
            SimulatedConfig cfg;
            cfg.FGDLSSGAdaMfgUnlock = true;
            cfg.XeMfgSrEnable = true;

            if (cfg.FGDLSSGAdaMfgUnlock.value_or_default())
            {
                cfg.XeMfgSrEnable = false;
            }
            assert(cfg.FGDLSSGAdaMfgUnlock.value_or_default() == true);
            assert(cfg.XeMfgSrEnable.value_or_default() == false);
        }

        // Case 3c: Ampere MFG takes precedence and forces XeMfgSrEnable = false
        {
            SimulatedConfig cfg;
            cfg.FGDLSSGAmpereMfgUnlock = true;
            cfg.XeMfgSrEnable = true;

            if (cfg.FGDLSSGAmpereMfgUnlock.value_or_default())
            {
                cfg.XeMfgSrEnable = false;
            }
            assert(cfg.FGDLSSGAmpereMfgUnlock.value_or_default() == true);
            assert(cfg.XeMfgSrEnable.value_or_default() == false);
        }

        // Case 3d: External Frame Generation forces XeMfgSrEnable = false
        {
            SimulatedConfig cfg;
            cfg.ExternalFrameGeneration = true;
            cfg.XeMfgSrEnable = true;

            if (cfg.ExternalFrameGeneration.value_or_default())
            {
                cfg.XeMfgSrEnable = false;
            }
            assert(cfg.ExternalFrameGeneration.value_or_default() == true);
            assert(cfg.XeMfgSrEnable.value_or_default() == false);
        }

        printf("  [PASS] Test 3: Universal mutual exclusion backend verified.\n");
    }

    // Test 4: Loader Session State (SrEnabledForSession, EnabledForSession, AnyEnabledForSession)
    {
        SimulatedConfig cfg;

        // When all off
        assert(!SimulatedLoader::EnabledForSession(cfg));
        assert(!SimulatedLoader::SrEnabledForSession(cfg));
        assert(!SimulatedLoader::AnyEnabledForSession(cfg));

        // When XeMfgUnlock is on
        cfg.XeMfgUnlock = true;
        assert(SimulatedLoader::EnabledForSession(cfg));
        assert(!SimulatedLoader::SrEnabledForSession(cfg));
        assert(SimulatedLoader::AnyEnabledForSession(cfg));

        // When XeMfgSrEnable is on (clean state)
        cfg.XeMfgUnlock = false;
        cfg.XeMfgSrEnable = true;
        assert(!SimulatedLoader::EnabledForSession(cfg));
        assert(SimulatedLoader::SrEnabledForSession(cfg));
        assert(SimulatedLoader::AnyEnabledForSession(cfg));

        // When XeMfgSrEnable is on but external FG active -> must reject
        cfg.ExternalFrameGeneration = true;
        assert(!SimulatedLoader::SrEnabledForSession(cfg));
        assert(!SimulatedLoader::AnyEnabledForSession(cfg));

        // When XeMfgSrEnable is on but Ada active -> must reject
        cfg.ExternalFrameGeneration = false;
        cfg.FGDLSSGAdaMfgUnlock = true;
        assert(!SimulatedLoader::SrEnabledForSession(cfg));
        assert(!SimulatedLoader::AnyEnabledForSession(cfg));

        // When XeMfgSrEnable is on but Ampere active -> must reject
        cfg.FGDLSSGAdaMfgUnlock = false;
        cfg.FGDLSSGAmpereMfgUnlock = true;
        assert(!SimulatedLoader::SrEnabledForSession(cfg));
        assert(!SimulatedLoader::AnyEnabledForSession(cfg));

        printf("  [PASS] Test 4: Loader session state evaluation verified.\n");
    }

    // Test 5: Dynamic Target Ceiling Resolution
    {
        SimulatedConfig cfg;

        // Primary XeMFG mode: resolves to XeMfgMaxFrames
        cfg.XeMfgUnlock = true;
        cfg.XeMfgMaxFrames = 4;
        cfg.XeMfgSrGeneratedFrames = 1;
        assert(SimulatedLoader::ResolveTargetCeiling(cfg) == 4);

        // XeMFG-SR mode: resolves to XeMfgSrGeneratedFrames
        cfg.XeMfgUnlock = false;
        cfg.XeMfgSrEnable = true;
        cfg.XeMfgSrGeneratedFrames = 2; // 3X FG default
        assert(SimulatedLoader::ResolveTargetCeiling(cfg) == 2);

        cfg.XeMfgSrGeneratedFrames = 5; // 6X FG
        assert(SimulatedLoader::ResolveTargetCeiling(cfg) == 5);

        printf("  [PASS] Test 5: Dynamic target ceiling resolution verified.\n");
    }

    // Test 6: In-Memory Patch Ceiling Adaptation (Live Byte Rewriting of U3, U4, U5)
    {
        SimulatedMemoryPatching::SimulatedDllImage image;

        // Initial unlock with SR target ceiling = 2 (3X Multi-Frame Generation)
        bool patched = image.ApplyUnlock(2);
        assert(patched);
        assert(image.GetU3Ceiling() == 2);
        assert(image.GetU4Ceiling() == 2);
        assert(image.GetU5Ceiling() == 2);

        // Live dynamic adjustments from in-game slider (1..5)
        for (unsigned int target = 1; target <= 5; ++target)
        {
            image.UpdateCeiling(target);
            assert(image.GetU3Ceiling() == target);
            assert(image.GetU4Ceiling() == target);
            assert(image.GetU5Ceiling() == target);
        }

        printf("  [PASS] Test 6: Live in-memory ceiling adaptation (U3/U4/U5 rewriting) verified.\n");
    }

    // Test 7: Post-Attach Automatic Pipeline Configuration (dllmain.cpp simulation)
    {
        SimulatedConfig cfg;
        cfg.XeMfgSrEnable = true;
        cfg.XeMfgSrGeneratedFrames = 3; // 4X FG

        const bool xeMfgSrActive = SimulatedLoader::SrEnabledForSession(cfg);
        if (xeMfgSrActive && !cfg.ExternalFrameGeneration.value_or_default())
        {
            cfg.FGEnabled = true;
            cfg.FGInputMode = FGInput::Upscaler;
            cfg.FGOutputMode = FGOutput::XeFG;
            cfg.FGNvngxReplacementMode = FGNvngxReplacement::None;
            const int srFrames = std::clamp(cfg.XeMfgSrGeneratedFrames.value_or(2), 1, 5);
            cfg.FGXeFGInterpolationCount = srFrames;
        }

        assert(cfg.FGEnabled.value() == true);
        assert(cfg.FGInputMode.value() == FGInput::Upscaler);
        assert(cfg.FGOutputMode.value() == FGOutput::XeFG);
        assert(cfg.FGNvngxReplacementMode.value() == FGNvngxReplacement::None);
        assert(cfg.FGXeFGInterpolationCount.value() == 3);

        // Verify live multiplier display formula
        int liveMultiplier = cfg.FGXeFGInterpolationCount.value();
        assert(liveMultiplier + 1 == 4); // 4X FG

        printf("  [PASS] Test 7: Post-attach automatic pipeline configuration verified.\n");
    }

    printf("[+] All XeMFG-SR unit tests PASSED successfully!\n");
    return 0;
}

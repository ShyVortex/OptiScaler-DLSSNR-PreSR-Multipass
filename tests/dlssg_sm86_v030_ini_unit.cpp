#include "../OptiScaler/framegen/dlssg/AmpereMfgLoader.h"
#include <cassert>
#include <cstdio>
#include <string>

int main()
{
    std::printf("=== Running DLSSG SM86 0.3.0 INI & Configuration Unit Tests ===\n");

    using namespace AmpereMfgLoader;

    // Test 1: Full 0.3.0 INI structure verification
    {
        std::string ini030 = FormatIniContent030(5, true, "Auto", "Auto", 0, "Auto", 1);

        assert(ini030.find("[General]\nEnabled=1\n") != std::string::npos);
        assert(ini030.find("[FrameGeneration]\nOptimized=1\nMaxGeneratedFrames=5\n") != std::string::npos);
        assert(ini030.find("[Compatibility]\nPreset=Auto\nRouter=Auto\nKernelImage=Auto\nHardwareBilinear=0\n") != std::string::npos);
        assert(ini030.find("[Logging]\nLevel=1\nDirectory=dlssg_sm86\\logs\n") != std::string::npos);
        assert(ini030.find("[Runtime]\nMode=Bundled\n") != std::string::npos);

        std::printf("  [PASS] Case 1: 0.3.0 INI full section structure verified ([General], [FrameGeneration], [Compatibility], [Logging], [Runtime])\n");
    }

    // Test 2: MaxGeneratedFrames clamping up to 5 (6X Multi-Frame Generation)
    {
        // 0 and negatives clamp to 5
        assert(FormatIniContent030(0).find("MaxGeneratedFrames=5\n") != std::string::npos);
        assert(FormatIniContent030(-3).find("MaxGeneratedFrames=5\n") != std::string::npos);

        // Values exceeding 5 clamp to 5
        assert(FormatIniContent030(6).find("MaxGeneratedFrames=5\n") != std::string::npos);
        assert(FormatIniContent030(10).find("MaxGeneratedFrames=5\n") != std::string::npos);

        // Valid ranges 1 to 5 cleanly preserved
        assert(FormatIniContent030(1).find("MaxGeneratedFrames=1\n") != std::string::npos); // 2X
        assert(FormatIniContent030(2).find("MaxGeneratedFrames=2\n") != std::string::npos); // 3X
        assert(FormatIniContent030(3).find("MaxGeneratedFrames=3\n") != std::string::npos); // 4X
        assert(FormatIniContent030(4).find("MaxGeneratedFrames=4\n") != std::string::npos); // 5X
        assert(FormatIniContent030(5).find("MaxGeneratedFrames=5\n") != std::string::npos); // 6X

        std::printf("  [PASS] Case 2: MaxGeneratedFrames 1-5 (2X-6X) clamping verified\n");
    }

    // Test 3: Optimized kernels 4-tier consistency levels (0.3.2) and backward compatibility
    {
        // Boolean compatibility
        std::string optTrue = FormatIniContent030(5, true);
        assert(optTrue.find("Optimized=1\n") != std::string::npos);

        std::string optFalse = FormatIniContent030(5, false);
        assert(optFalse.find("Optimized=0\n") != std::string::npos);

        // 4 consistency tiers: 0=stock, 1=bit-identical, 2=fast lossy, 3=fastest lossy
        std::string opt0 = FormatIniContent030(5, 0);
        assert(opt0.find("Optimized=0\n") != std::string::npos);

        std::string opt1 = FormatIniContent030(5, 1);
        assert(opt1.find("Optimized=1\n") != std::string::npos);

        std::string opt2 = FormatIniContent030(5, 2);
        assert(opt2.find("Optimized=2\n") != std::string::npos);

        std::string opt3 = FormatIniContent030(5, 3);
        assert(opt3.find("Optimized=3\n") != std::string::npos);

        // Out-of-range values safely clamp to default tier 1 (Bit-identical)
        std::string optNegative = FormatIniContent030(5, -1);
        assert(optNegative.find("Optimized=1\n") != std::string::npos);

        std::string optTooLarge = FormatIniContent030(5, 4);
        assert(optTooLarge.find("Optimized=1\n") != std::string::npos);

        std::printf("  [PASS] Case 3: Optimized kernels 4-tier consistency levels (0, 1, 2, 3) and clamping verified\n");
    }

    // Test 4: UI Recomposition Preset validation (Auto, A, B)
    {
        std::string pAuto = FormatIniContent030(5, 1, "Auto");
        assert(pAuto.find("Preset=Auto\n") != std::string::npos);

        std::string pA = FormatIniContent030(5, 1, "A");
        assert(pA.find("Preset=A\n") != std::string::npos);

        std::string paLower = FormatIniContent030(5, 1, "a");
        assert(paLower.find("Preset=A\n") != std::string::npos);

        std::string pB = FormatIniContent030(5, 1, "B");
        assert(pB.find("Preset=B\n") != std::string::npos);

        std::string pbLower = FormatIniContent030(5, 1, "b");
        assert(pbLower.find("Preset=B\n") != std::string::npos);

        std::string pInvalid = FormatIniContent030(5, 1, "invalid");
        assert(pInvalid.find("Preset=Auto\n") != std::string::npos);

        std::printf("  [PASS] Case 4: UI Recomposition Preset validation (Auto, A, B) verified\n");
    }

    // Test 5: ResolveMaxGeneratedFrames with variable runtime ceiling
    {
        // 310.9 runtime ceiling (5 = 6X)
        assert(ResolveMaxGeneratedFrames(5, false, 5) == 5);
        assert(ResolveMaxGeneratedFrames(4, false, 5) == 4);
        assert(ResolveMaxGeneratedFrames(6, false, 5) == 5);
        assert(ResolveMaxGeneratedFrames(0, false, 5) == 5);

        // 310.1 runtime ceiling (3 = 4X)
        assert(ResolveMaxGeneratedFrames(4, false, 3) == 3);
        assert(ResolveMaxGeneratedFrames(5, false, 3) == 3);
        assert(ResolveMaxGeneratedFrames(3, false, 3) == 3);
        assert(ResolveMaxGeneratedFrames(0, false, 3) == 3);

        std::printf("  [PASS] Case 5: ResolveMaxGeneratedFrames respects runtime-specific ceilings (3 vs 5)\n");
    }

    // Test 6: Linux DRS multi-frame resolution with 0.3.0 6X ceiling
    {
        uint32_t val = 0;
        // Setting 0x104D6667 without explicit override must return false (preserves in-game 2X FG)
        assert(TryResolveDrsMultiFrameSetting(DRS_OVERRIDE_DLSSG_MULTI_FRAME_COUNT_ID, 5, true, true, val, 5) == false);

        // Setting 0x104D6667 with explicit user override 5 (6X) on Linux
        assert(TryResolveDrsMultiFrameSetting(DRS_OVERRIDE_DLSSG_MULTI_FRAME_COUNT_ID, 5, true, true, val, 5, 5) == true);
        assert(val == 5);

        // Clamping explicit override to 0.3.0 ceiling (5)
        assert(TryResolveDrsMultiFrameSetting(DRS_OVERRIDE_DLSSG_MULTI_FRAME_COUNT_ID, 5, true, true, val, 5, 8) == true);
        assert(val == 5);

        // Dynamic ceiling 0x10562D0F properly scales to 0.3.0 ceiling (5 frames / 6X)
        assert(TryResolveDrsMultiFrameSetting(DRS_OVERRIDE_MAX_DLSSG_DYNAMIC_MULTI_FRAME_COUNT_ID, 5, true, true, val, 5) == true);
        assert(val == 5);
        assert(TryResolveDrsMultiFrameSetting(DRS_OVERRIDE_MAX_DLSSG_DYNAMIC_MULTI_FRAME_COUNT_ID, 8, true, true, val, 5) == true);
        assert(val == 5);

        std::printf("  [PASS] Case 6: Linux DRS override properly scales to 0.3.0 ceiling (5 frames / 6X)\n");
    }

    // Test 7: Backward-compatibility: 0.2.4 FormatIniContent remains unchanged
    {
        std::string legacyIni = FormatIniContent(3, "PTX", 0, "SM86", 1);
        assert(legacyIni.find("; Native 0.2.4") != std::string::npos);
        assert(legacyIni.find("MaxGeneratedFrames=3") != std::string::npos);
        assert(legacyIni.find("Router=SM86") != std::string::npos);

        std::printf("  [PASS] Case 7: 0.2.4 legacy formatter preserved for backward compatibility\n");
    }

    // Test 8: Decoupled ceiling resolution for 0.3.1 (310.9 with SM75 support retains 6X ceiling)
    {
        // 310.9 runtime with SM75 support (0.3.1 unified build): ceiling is 5 (6X)
        const int ceiling3109 = 5;
        assert(ResolveMaxGeneratedFrames(5, false, ceiling3109) == 5);
        assert(ResolveMaxGeneratedFrames(4, false, ceiling3109) == 4);

        // 310.1 runtime with SM75 support: ceiling is 3 (4X)
        const int ceiling3101 = 3;
        assert(ResolveMaxGeneratedFrames(5, false, ceiling3101) == 3);
        assert(ResolveMaxGeneratedFrames(4, false, ceiling3101) == 3);

        std::printf("  [PASS] Case 8: Decoupled 0.3.1 ceiling: 310.9 with SM75 preserves 6X, 310.1 clamps to 4X\n");
    }

    // Test 9: Factory default 4X (3 frames) resolution in 0.3.2
    {
        // Default maxCeiling is 3 (4X)
        assert(ResolveMaxGeneratedFrames(0) == 3);
        assert(ResolveMaxGeneratedFrames(-1) == 3);
        assert(ResolveMaxGeneratedFrames(3) == 3);
        assert(ResolveMaxGeneratedFrames(1) == 1);
        assert(ResolveMaxGeneratedFrames(2) == 2);

        // Explicit ceiling 5 allows 5 (6X) when user requests it
        assert(ResolveMaxGeneratedFrames(5, false, 5) == 5);
        assert(ResolveMaxGeneratedFrames(0, false, 5) == 5);

        std::printf("  [PASS] Case 9: Factory default 4X (3 frames) resolution and override to 6X verified\n");
    }

    // Test 10: Streamline 2.8+ architecture spoofing (SpoofArchToGame in 0.3.3)
    {
        // Default "Auto": SpoofArchToGame key omitted matching factory INI convention
        std::string iniAuto = FormatIniContent030(3, 1, "Auto", "Auto", 0, "Auto", 1, "Auto");
        assert(iniAuto.find("SpoofArchToGame") == std::string::npos);

        std::string iniDefault = FormatIniContent030(3, 1);
        assert(iniDefault.find("SpoofArchToGame") == std::string::npos);

        // Explicit "1" or "true": SpoofArchToGame=1 emitted
        std::string iniEnabled1 = FormatIniContent030(3, 1, "Auto", "Auto", 0, "Auto", 1, "1");
        assert(iniEnabled1.find("SpoofArchToGame=1\n") != std::string::npos);

        std::string iniEnabledTrue = FormatIniContent030(3, 1, "Auto", "Auto", 0, "Auto", 1, "true");
        assert(iniEnabledTrue.find("SpoofArchToGame=1\n") != std::string::npos);

        // Explicit "0" or "false": SpoofArchToGame=0 emitted
        std::string iniDisabled0 = FormatIniContent030(3, 1, "Auto", "Auto", 0, "Auto", 1, "0");
        assert(iniDisabled0.find("SpoofArchToGame=0\n") != std::string::npos);

        std::string iniDisabledFalse = FormatIniContent030(3, 1, "Auto", "Auto", 0, "Auto", 1, "false");
        assert(iniDisabledFalse.find("SpoofArchToGame=0\n") != std::string::npos);

        // Invalid or empty string: treated as absent/auto (omitted)
        std::string iniOther = FormatIniContent030(3, 1, "Auto", "Auto", 0, "Auto", 1, "invalid");
        assert(iniOther.find("SpoofArchToGame") == std::string::npos);

        std::printf("  [PASS] Case 10: Streamline 2.8+ arch spoofing (SpoofArchToGame: auto omitted, 1, 0) verified\n");
    }

    // Test 11: dlssg_sm86 logging level configuration (0-3) and clamping
    {
        assert(FormatIniContent030(5, 1, "Auto", "Auto", 0, "Auto", 0).find("[Logging]\nLevel=0\n") != std::string::npos);
        assert(FormatIniContent030(5, 1, "Auto", "Auto", 0, "Auto", 1).find("[Logging]\nLevel=1\n") != std::string::npos);
        assert(FormatIniContent030(5, 1, "Auto", "Auto", 0, "Auto", 2).find("[Logging]\nLevel=2\n") != std::string::npos);
        assert(FormatIniContent030(5, 1, "Auto", "Auto", 0, "Auto", 3).find("[Logging]\nLevel=3\n") != std::string::npos);

        // Clamping invalid levels to default 1
        assert(FormatIniContent030(5, 1, "Auto", "Auto", 0, "Auto", -1).find("[Logging]\nLevel=1\n") != std::string::npos);
        assert(FormatIniContent030(5, 1, "Auto", "Auto", 0, "Auto", 4).find("[Logging]\nLevel=1\n") != std::string::npos);
        assert(FormatIniContent030(5, 1, "Auto", "Auto", 0, "Auto", 99).find("[Logging]\nLevel=1\n") != std::string::npos);

        std::printf("  [PASS] Case 11: dlssg_sm86 logging level (0-3) and clamping verified\n");
    }

    std::printf("=== All DLSSG SM86 0.3.x INI & Configuration Unit Tests PASSED! ===\n");
    return 0;
}

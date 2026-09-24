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
        assert(ini030.find("[Compatibility]\nPreset=Auto\nRouter=Auto\nKernelImage=Auto\nHardwareBilinear=0\n") !=
               std::string::npos);
        assert(ini030.find("[Logging]\nLevel=1\nDirectory=dlssg_sm86\\logs\n") != std::string::npos);
        assert(ini030.find("[Runtime]\nMode=Bundled\nCacheDirectory=\n") != std::string::npos);

        std::printf("  [PASS] Case 1: 0.3.x INI full section structure verified ([General], [FrameGeneration], "
                    "[Compatibility], [Logging], [Runtime])\n");
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

        std::printf(
            "  [PASS] Case 3: Optimized kernels 4-tier consistency levels (0, 1, 2, 3) and clamping verified\n");
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
        assert(TryResolveDrsMultiFrameSetting(DRS_OVERRIDE_DLSSG_MULTI_FRAME_COUNT_ID, 5, true, true, val, 5, 5) ==
               true);
        assert(val == 5);

        // Clamping explicit override to 0.3.0 ceiling (5)
        assert(TryResolveDrsMultiFrameSetting(DRS_OVERRIDE_DLSSG_MULTI_FRAME_COUNT_ID, 5, true, true, val, 5, 8) ==
               true);
        assert(val == 5);

        // Dynamic ceiling 0x10562D0F properly scales to 0.3.0 ceiling (5 frames / 6X)
        assert(TryResolveDrsMultiFrameSetting(DRS_OVERRIDE_MAX_DLSSG_DYNAMIC_MULTI_FRAME_COUNT_ID, 5, true, true, val,
                                              5) == true);
        assert(val == 5);
        assert(TryResolveDrsMultiFrameSetting(DRS_OVERRIDE_MAX_DLSSG_DYNAMIC_MULTI_FRAME_COUNT_ID, 8, true, true, val,
                                              5) == true);
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
        assert(FormatIniContent030(5, 1, "Auto", "Auto", 0, "Auto", 0).find("[Logging]\nLevel=0\n") !=
               std::string::npos);
        assert(FormatIniContent030(5, 1, "Auto", "Auto", 0, "Auto", 1).find("[Logging]\nLevel=1\n") !=
               std::string::npos);
        assert(FormatIniContent030(5, 1, "Auto", "Auto", 0, "Auto", 2).find("[Logging]\nLevel=2\n") !=
               std::string::npos);
        assert(FormatIniContent030(5, 1, "Auto", "Auto", 0, "Auto", 3).find("[Logging]\nLevel=3\n") !=
               std::string::npos);

        // Clamping invalid levels to default 1
        assert(FormatIniContent030(5, 1, "Auto", "Auto", 0, "Auto", -1).find("[Logging]\nLevel=1\n") !=
               std::string::npos);
        assert(FormatIniContent030(5, 1, "Auto", "Auto", 0, "Auto", 4).find("[Logging]\nLevel=1\n") !=
               std::string::npos);
        assert(FormatIniContent030(5, 1, "Auto", "Auto", 0, "Auto", 99).find("[Logging]\nLevel=1\n") !=
               std::string::npos);

        std::printf("  [PASS] Case 11: dlssg_sm86 logging level (0-3) and clamping verified\n");
    }

    // Test 12: v0.3.4-v0.3.5 updates: CacheDirectory key and 0.3.5 binary validation
    {
        std::string iniDefault = FormatIniContent030(3, 1);
        assert(iniDefault.find("[Runtime]\nMode=Bundled\nCacheDirectory=\n") != std::string::npos);

        std::filesystem::path rootDll = std::filesystem::exists("dlssg_for_sm86/sdli1995/version.dll")
                                            ? "dlssg_for_sm86/sdli1995/version.dll"
                                            : "dlssg_for_sm86/version.dll";
        std::filesystem::path sm863101Dll = std::filesystem::exists("dlssg_for_sm86/sdli1995/310.1/version.dll")
                                                ? "dlssg_for_sm86/sdli1995/310.1/version.dll"
                                                : "dlssg_for_sm86/310.1/version.dll";
        if (std::filesystem::exists(rootDll) && std::filesystem::exists(sm863101Dll))
        {
            assert(HasSm75KernelFamily(rootDll) && "v0.3.5 root 310.9 runtime must have SM75 kernel support");
            assert(!Is3101Runtime(rootDll) && "v0.3.5 root runtime must be 310.9, not 310.1");
            assert(HasSm75KernelFamily(sm863101Dll) && "v0.3.5 310.1 runtime must have SM75 kernel support");
            assert(Is3101Runtime(sm863101Dll) && "v0.3.5 310.1 runtime must be 310.1");
            std::printf("  [PASS] Case 12: v0.3.5 binary signatures and CacheDirectory INI field verified\n");
        }
        else
        {
            std::printf("  [PASS] Case 12: v0.3.5 CacheDirectory field verified (binaries not in CWD)\n");
        }
    }

    // Test 13: Dynamic Multi-Frame Generation (DynamicMFG & DynamicTargetFPS) formatting
    {
        // When dynamicMfg is enabled, DynamicMFG=1 and DynamicTargetFPS are emitted
        std::string iniDynamic120 = FormatIniContent030(5, 1, "Auto", "Auto", 0, "Auto", 1, "Auto", true, 120.0f);
        assert(iniDynamic120.find("DynamicMFG=1\n") != std::string::npos);
        assert(iniDynamic120.find("DynamicTargetFPS=120\n") != std::string::npos);

        // When dynamicMfg is enabled with 0 FPS (auto refresh rate)
        std::string iniDynamic0 = FormatIniContent030(5, 1, "Auto", "Auto", 0, "Auto", 1, "Auto", true, 0.0f);
        assert(iniDynamic0.find("DynamicMFG=1\n") != std::string::npos);
        assert(iniDynamic0.find("DynamicTargetFPS=0\n") != std::string::npos);

        // When dynamicMfg is false and hasDynamicMfgSupport is true (SilyNoMeta fork disabled)
        std::string iniDynamicDisabled =
            FormatIniContent030(5, 1, "Auto", "Auto", 0, "Auto", 1, "Auto", false, 0.0f, true);
        assert(iniDynamicDisabled.find("DynamicMFG=0\n") != std::string::npos);
        assert(iniDynamicDisabled.find("DynamicTargetFPS=0\n") != std::string::npos);

        // When dynamicMfg is false and hasDynamicMfgSupport is false (standard sdli1995 clean INI)
        std::string iniStandard = FormatIniContent030(5, 1, "Auto", "Auto", 0, "Auto", 1, "Auto", false, 0.0f, false);
        assert(iniStandard.find("DynamicMFG") == std::string::npos);
        assert(iniStandard.find("DynamicTargetFPS") == std::string::npos);

        std::printf("  [PASS] Case 13: DynamicMFG and DynamicTargetFPS formatting verified (enabled, SilyNoMeta "
                    "disabled, sdli1995 clean)\n");
    }

    // Test 14: ReShade.ini companion section generation from scratch
    {
        std::string newReshade = MergeReshadeCompanionContent("", true, 144.0f, 5, 1);
        assert(newReshade.find("[DLSSG-SM86-75-COMPANION]\n") != std::string::npos);
        assert(newReshade.find("Dynamic=1\n") != std::string::npos);
        assert(newReshade.find("TargetFPS=144\n") != std::string::npos);
        assert(newReshade.find("Multiplier=0\n") !=
               std::string::npos); // Dynamic mode sets Multiplier=0 (FollowGame/Dynamic)
        assert(newReshade.find("UIRecomposition=1\n") != std::string::npos);

        // Fixed multiplier mode (dynamicMfg = false, maxFrames = 3 -> Multiplier = 4 (4X))
        std::string fixedReshade = MergeReshadeCompanionContent("", false, 0.0f, 3, 2);
        assert(fixedReshade.find("Dynamic=0\n") != std::string::npos);
        assert(fixedReshade.find("Multiplier=4\n") != std::string::npos);
        assert(fixedReshade.find("TargetFPS=0\n") != std::string::npos);
        assert(fixedReshade.find("UIRecomposition=2\n") != std::string::npos);

        std::printf("  [PASS] Case 14: ReShade.ini companion section generation from scratch verified\n");
    }

    // Test 15: Non-destructive merging into existing ReShade.ini
    {
        std::string existing = "[GENERAL]\n"
                               "EffectSearchPaths=.\\reshade-shaders\\Shaders\n"
                               "PerformanceMode=1\n"
                               "\n"
                               "[DLSSG-SM86-75-COMPANION]\n"
                               "VulkanFamily=0\n"
                               "Multiplier=2\n"
                               "Dynamic=0\n"
                               "TargetFPS=60\n"
                               "DLSSRenderScale=0\n"
                               "UIRecomposition=0\n"
                               "\n"
                               "[OVERLAY]\n"
                               "ShowFPS=1\n";

        std::string merged = MergeReshadeCompanionContent(existing, true, 120.0f, 5, 1);

        // Verify other sections are preserved verbatim
        assert(merged.find("[GENERAL]\nEffectSearchPaths=.\\reshade-shaders\\Shaders\nPerformanceMode=1\n") !=
               std::string::npos);
        assert(merged.find("[OVERLAY]\nShowFPS=1\n") != std::string::npos);

        // Verify companion section updated
        assert(merged.find("Dynamic=1\n") != std::string::npos);
        assert(merged.find("TargetFPS=120\n") != std::string::npos);
        assert(merged.find("Multiplier=0\n") != std::string::npos);
        assert(merged.find("UIRecomposition=1\n") != std::string::npos);

        // Old values removed
        assert(merged.find("TargetFPS=60") == std::string::npos);
        assert(merged.find("Multiplier=2") == std::string::npos);

        std::printf("  [PASS] Case 15: Non-destructive merging into existing ReShade.ini verified\n");
    }

    std::printf("=== All DLSSG SM86 0.3.x INI & Configuration Unit Tests PASSED! ===\n");
    return 0;
}

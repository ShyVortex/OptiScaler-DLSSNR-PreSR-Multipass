#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <nvsdk_ngx_params.h>
#include <inputs/FG/DlssgParameterSanitizer.h>
#include <upscalers/ScopedNgxDimensionRestoration.h>
#include <framegen/dlssg/AmpereMfgLoader.h>

// Concrete Mock NVSDK_NGX_Parameter implementation for testing
class MockNgxParameters : public NVSDK_NGX_Parameter
{
  private:
    std::unordered_map<std::string, unsigned long long> _ullValues;
    std::unordered_map<std::string, float> _floatValues;
    std::unordered_map<std::string, double> _doubleValues;
    std::unordered_map<std::string, unsigned int> _uintValues;
    std::unordered_map<std::string, int> _intValues;
    std::unordered_map<std::string, void*> _voidPtrValues;
    std::unordered_map<std::string, ID3D12Resource*> _d12Values;

  public:
    void Set(const char* InName, unsigned long long InValue) override
    {
        if (InName)
            _ullValues[InName] = InValue;
    }
    void Set(const char* InName, float InValue) override
    {
        if (InName)
            _floatValues[InName] = InValue;
    }
    void Set(const char* InName, double InValue) override
    {
        if (InName)
            _doubleValues[InName] = InValue;
    }
    void Set(const char* InName, unsigned int InValue) override
    {
        if (InName)
            _uintValues[InName] = InValue;
    }
    void Set(const char* InName, int InValue) override
    {
        if (InName)
            _intValues[InName] = InValue;
    }
    void Set(const char* InName, ID3D11Resource* InValue) override
    {
        if (InName)
            _voidPtrValues[InName] = (void*) InValue;
    }
    void Set(const char* InName, ID3D12Resource* InValue) override
    {
        if (InName)
        {
            _d12Values[InName] = InValue;
            _voidPtrValues[InName] = (void*) InValue;
            _ullValues[InName] = (unsigned long long) InValue;
        }
    }
    void Set(const char* InName, void* InValue) override
    {
        if (InName)
        {
            _voidPtrValues[InName] = InValue;
            _d12Values[InName] = (ID3D12Resource*) InValue;
            _ullValues[InName] = (unsigned long long) InValue;
        }
    }

    NVSDK_NGX_Result Get(const char* InName, unsigned long long* OutValue) const override
    {
        if (!InName || !OutValue)
            return NVSDK_NGX_Result_Fail;
        auto it = _ullValues.find(InName);
        if (it != _ullValues.end())
        {
            *OutValue = it->second;
            return NVSDK_NGX_Result_Success;
        }
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    }

    NVSDK_NGX_Result Get(const char* InName, float* OutValue) const override
    {
        if (!InName || !OutValue)
            return NVSDK_NGX_Result_Fail;
        auto it = _floatValues.find(InName);
        if (it != _floatValues.end())
        {
            *OutValue = it->second;
            return NVSDK_NGX_Result_Success;
        }
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    }

    NVSDK_NGX_Result Get(const char* InName, double* OutValue) const override
    {
        if (!InName || !OutValue)
            return NVSDK_NGX_Result_Fail;
        auto it = _doubleValues.find(InName);
        if (it != _doubleValues.end())
        {
            *OutValue = it->second;
            return NVSDK_NGX_Result_Success;
        }
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    }

    NVSDK_NGX_Result Get(const char* InName, unsigned int* OutValue) const override
    {
        if (!InName || !OutValue)
            return NVSDK_NGX_Result_Fail;
        auto it = _uintValues.find(InName);
        if (it != _uintValues.end())
        {
            *OutValue = it->second;
            return NVSDK_NGX_Result_Success;
        }
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    }

    NVSDK_NGX_Result Get(const char* InName, int* OutValue) const override
    {
        if (!InName || !OutValue)
            return NVSDK_NGX_Result_Fail;
        auto it = _intValues.find(InName);
        if (it != _intValues.end())
        {
            *OutValue = it->second;
            return NVSDK_NGX_Result_Success;
        }
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    }

    NVSDK_NGX_Result Get(const char* InName, ID3D11Resource** OutValue) const override
    {
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    }

    NVSDK_NGX_Result Get(const char* InName, ID3D12Resource** OutValue) const override
    {
        if (!InName || !OutValue)
            return NVSDK_NGX_Result_Fail;
        auto it = _d12Values.find(InName);
        if (it != _d12Values.end())
        {
            *OutValue = it->second;
            return NVSDK_NGX_Result_Success;
        }
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    }

    NVSDK_NGX_Result Get(const char* InName, void** OutValue) const override
    {
        if (!InName || !OutValue)
            return NVSDK_NGX_Result_Fail;
        auto it = _voidPtrValues.find(InName);
        if (it != _voidPtrValues.end())
        {
            *OutValue = it->second;
            return NVSDK_NGX_Result_Success;
        }
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    }

    void Reset() override
    {
        _ullValues.clear();
        _floatValues.clear();
        _doubleValues.clear();
        _uintValues.clear();
        _intValues.clear();
        _voidPtrValues.clear();
        _d12Values.clear();
    }
};

// Simulation of nvngx_dlssg.dll resource ingestion and validation
// Replicates EndpointResourceManager::ReadResource and EndpointCore::ValidateApplicationResources
struct SimulatedResourceState
{
    bool hasOutputReal = false;
    uint32_t outputRealWidth = 0;
    uint32_t outputRealHeight = 0;
    uint32_t backbufferWidth = 2560;
    uint32_t backbufferHeight = 1440;

    NVSDK_NGX_Result ReadAndValidate(NVSDK_NGX_Parameter* params)
    {
        // 1. Read Backbuffer
        void* backbufferRes = nullptr;
        if (params->Get("DLSSG.Backbuffer", &backbufferRes) != NVSDK_NGX_Result_Success || !backbufferRes)
            return NVSDK_NGX_Result_FAIL_InvalidParameter;

        // 2. Read DLSSG.OutputReal (EndpointResourceManager::ReadResource at VA 0x180063249)
        void* outputRealRes = nullptr;
        NVSDK_NGX_Result getRealRes = params->Get("DLSSG.OutputReal", &outputRealRes);

        // In nvngx_dlssg.dll:
        // cmpl bashxbad00000, %eax / je 0x18006340f (skip if Get returned failure/not found)
        if ((static_cast<uint32_t>(getRealRes) & 0xFFF00000) != 0xBAD00000)
        {
            // If Get did NOT return 0xBAD00000, Tag 0x4c is registered as PRESENT!
            hasOutputReal = true;
            if (outputRealRes != nullptr)
            {
                outputRealWidth = 2560;
                outputRealHeight = 1440;
            }
            else
            {
                // Null resource has extent 0x0
                outputRealWidth = 0;
                outputRealHeight = 0;
            }
        }
        else
        {
            hasOutputReal = false;
        }

        // 3. Extent validation (EndpointCore::ValidateApplicationResources at VA 0x18003815e)
        if (hasOutputReal)
        {
            // Compare OutputReal dimensions against Backbuffer dimensions
            if (outputRealWidth != backbufferWidth || outputRealHeight != backbufferHeight)
            {
                return NVSDK_NGX_Result_FAIL_InvalidParameter; // 0xBAD00005!
            }
        }

        return NVSDK_NGX_Result_Success;
    }
};

void Test_ShouldFilterDlssgOutputReal_NullParameters()
{
    assert(!ShouldFilterDlssgOutputReal(nullptr));
    std::printf("  [PASS] Test_ShouldFilterDlssgOutputReal_NullParameters\n");
}

void Test_ShouldFilterDlssgOutputReal_KeyNotPresent()
{
    MockNgxParameters params;
    params.Set("DLSSG.Backbuffer", (void*) 0x1000);
    assert(!ShouldFilterDlssgOutputReal(&params));
    std::printf("  [PASS] Test_ShouldFilterDlssgOutputReal_KeyNotPresent\n");
}

void Test_ShouldFilterDlssgOutputReal_NonNullResource()
{
    MockNgxParameters params;
    params.Set("DLSSG.OutputReal", (ID3D12Resource*) 0x2000);
    assert(!ShouldFilterDlssgOutputReal(&params));
    std::printf("  [PASS] Test_ShouldFilterDlssgOutputReal_NonNullResource\n");
}

void Test_ShouldFilterDlssgOutputReal_NullResource()
{
    MockNgxParameters params;
    params.Set("DLSSG.OutputReal", (ID3D12Resource*) nullptr);
    assert(ShouldFilterDlssgOutputReal(&params));

    params.Reset();
    params.Set("DLSSG.OutputReal", (void*) nullptr);
    assert(ShouldFilterDlssgOutputReal(&params));
    std::printf("  [PASS] Test_ShouldFilterDlssgOutputReal_NullResource\n");
}

void Test_DlssgParameterSanitizer_FilteredGet()
{
    MockNgxParameters params;
    params.Set("DLSSG.OutputReal", (ID3D12Resource*) nullptr);
    params.Set("DLSSG.Backbuffer", (void*) 0x1000);
    params.Set("DLSSG.OutputInterpolated", (void*) 0x3000);
    params.Set("DLSSG.MultiFrameCount", 2);

    DlssgParameterSanitizer sanitizer(&params, true);
    assert(sanitizer.IsFilteringOutputReal());
    assert(sanitizer.GetInner() == &params);

    // OutputReal should return NVSDK_NGX_Result_FAIL_FeatureNotFound across all overloads
    unsigned long long ullVal = 0;
    assert(sanitizer.Get("DLSSG.OutputReal", &ullVal) == NVSDK_NGX_Result_FAIL_FeatureNotFound);

    void* voidVal = nullptr;
    assert(sanitizer.Get("DLSSG.OutputReal", &voidVal) == NVSDK_NGX_Result_FAIL_FeatureNotFound);

    ID3D12Resource* d12Val = nullptr;
    assert(sanitizer.Get("DLSSG.OutputReal", &d12Val) == NVSDK_NGX_Result_FAIL_FeatureNotFound);

    // Other parameters should pass through cleanly
    void* bbVal = nullptr;
    assert(sanitizer.Get("DLSSG.Backbuffer", &bbVal) == NVSDK_NGX_Result_Success);
    assert(bbVal == (void*) 0x1000);

    void* interpVal = nullptr;
    assert(sanitizer.Get("DLSSG.OutputInterpolated", &interpVal) == NVSDK_NGX_Result_Success);
    assert(interpVal == (void*) 0x3000);

    int mfgCount = 0;
    assert(sanitizer.Get("DLSSG.MultiFrameCount", &mfgCount) == NVSDK_NGX_Result_Success);
    assert(mfgCount == 2);

    std::printf("  [PASS] Test_DlssgParameterSanitizer_FilteredGet\n");
}

void Test_DlssgParameterSanitizer_UnfilteredGet()
{
    MockNgxParameters params;
    params.Set("DLSSG.OutputReal", (ID3D12Resource*) 0x2000);

    DlssgParameterSanitizer sanitizer(&params, false);
    assert(!sanitizer.IsFilteringOutputReal());

    void* realVal = nullptr;
    assert(sanitizer.Get("DLSSG.OutputReal", &realVal) == NVSDK_NGX_Result_Success);
    assert(realVal == (void*) 0x2000);

    std::printf("  [PASS] Test_DlssgParameterSanitizer_UnfilteredGet\n");
}

void Test_DlssgParameterSanitizer_SetForwarding()
{
    MockNgxParameters params;
    DlssgParameterSanitizer sanitizer(&params, true);

    sanitizer.Set("DLSSG.CameraNear", 0.1f);
    sanitizer.Set("DLSSG.CameraFar", 1000.0f);

    float nearVal = 0.0f;
    float farVal = 0.0f;
    assert(params.Get("DLSSG.CameraNear", &nearVal) == NVSDK_NGX_Result_Success);
    assert(params.Get("DLSSG.CameraFar", &farVal) == NVSDK_NGX_Result_Success);
    assert(nearVal == 0.1f);
    assert(farVal == 1000.0f);

    std::printf("  [PASS] Test_DlssgParameterSanitizer_SetForwarding\n");
}

void Test_Simulation_Streamline_MultiPass_MFG()
{
    // Simulate 3X FG (numFramesToGenerate = 2 -> 2 evaluation passes per present):
    // Pass 0 (intermediate): Streamline sets OutputReal = nullptr
    // Pass 1 (final): Streamline sets OutputReal = destination buffer

    MockNgxParameters params;
    params.Set("DLSSG.Backbuffer", (void*) 0x1000);
    params.Set("DLSSG.OutputInterpolated", (void*) 0x2000);

    SimulatedResourceState validator;

    // --- Subcase A: Without sanitizer (Issue #40 behavior) ---
    params.Set("DLSSG.OutputReal", (ID3D12Resource*) nullptr);
    NVSDK_NGX_Result unhandledResult = validator.ReadAndValidate(&params);
    assert(unhandledResult == NVSDK_NGX_Result_FAIL_InvalidParameter); // 0xBAD00005!

    // --- Subcase B: With sanitizer on intermediate subframe ---
    bool shouldFilter = ShouldFilterDlssgOutputReal(&params);
    assert(shouldFilter);

    DlssgParameterSanitizer sanitizerPass0(&params, shouldFilter);
    NVSDK_NGX_Result sanitizedResultPass0 = validator.ReadAndValidate(&sanitizerPass0);
    assert(sanitizedResultPass0 == NVSDK_NGX_Result_Success);
    assert(!validator.hasOutputReal);

    // --- Subcase C: Final subframe with real destination buffer ---
    params.Set("DLSSG.OutputReal", (ID3D12Resource*) 0x4000);
    shouldFilter = ShouldFilterDlssgOutputReal(&params);
    assert(!shouldFilter);

    DlssgParameterSanitizer sanitizerPass1(&params, shouldFilter);
    NVSDK_NGX_Result sanitizedResultPass1 = validator.ReadAndValidate(&sanitizerPass1);
    assert(sanitizedResultPass1 == NVSDK_NGX_Result_Success);
    assert(validator.hasOutputReal);

    std::printf("  [PASS] Test_Simulation_Streamline_MultiPass_MFG\n");
}

void Test_ResolveDlssgSanitization_UpscalerDimensionMismatch()
{
    // Shared parameters contaminated by DLSS Super Resolution (e.g., 1080p display with 720p render resolution)
    MockNgxParameters params;
    params.Set("Width", 1280u);
    params.Set("Height", 720u);
    params.Set("OutWidth", 1920u);
    params.Set("OutHeight", 1080u);

    // Resolve sanitization with no explicit fallback display extent
    DlssgSanitizationParams sanitization = ResolveDlssgSanitization(&params, 0, 0);
    assert(sanitization.NeedsSanitization());
    assert(sanitization.forcedWidth == 1920u);
    assert(sanitization.forcedHeight == 1080u);

    DlssgParameterSanitizer sanitizer(&params, sanitization.filterOutputReal, sanitization.forcedWidth,
                                      sanitization.forcedHeight);

    // Assert that queries for Width and Height return the sanitized target extent across multiple types
    unsigned int uWidth = 0, uHeight = 0;
    assert(sanitizer.Get("Width", &uWidth) == NVSDK_NGX_Result_Success && uWidth == 1920u);
    assert(sanitizer.Get("Height", &uHeight) == NVSDK_NGX_Result_Success && uHeight == 1080u);

    int iWidth = 0, iHeight = 0;
    assert(sanitizer.Get("Width", &iWidth) == NVSDK_NGX_Result_Success && iWidth == 1920);
    assert(sanitizer.Get("Height", &iHeight) == NVSDK_NGX_Result_Success && iHeight == 1080);

    float fWidth = 0.0f, fHeight = 0.0f;
    assert(sanitizer.Get("Width", &fWidth) == NVSDK_NGX_Result_Success && fWidth == 1920.0f);
    assert(sanitizer.Get("Height", &fHeight) == NVSDK_NGX_Result_Success && fHeight == 1080.0f);

    double dWidth = 0.0, dHeight = 0.0;
    assert(sanitizer.Get("Width", &dWidth) == NVSDK_NGX_Result_Success && dWidth == 1920.0);
    assert(sanitizer.Get("Height", &dHeight) == NVSDK_NGX_Result_Success && dHeight == 1080.0);

    unsigned long long ullWidth = 0, ullHeight = 0;
    assert(sanitizer.Get("Width", &ullWidth) == NVSDK_NGX_Result_Success && ullWidth == 1920ull);
    assert(sanitizer.Get("Height", &ullHeight) == NVSDK_NGX_Result_Success && ullHeight == 1080ull);

    // Assert that the underlying dictionary retains its original values without in-place mutation
    unsigned int rawWidth = 0, rawHeight = 0;
    params.Get("Width", &rawWidth);
    params.Get("Height", &rawHeight);
    assert(rawWidth == 1280u);
    assert(rawHeight == 720u);

    std::printf("  [PASS] Test_ResolveDlssgSanitization_UpscalerDimensionMismatch\n");
}

void Test_ResolveDlssgSanitization_DirectNgxBackbufferFallback()
{
    // Direct NGX Feature 11 evaluation without OutWidth/OutHeight set in parameters
    MockNgxParameters params;
    params.Set("Width", 1280u);
    params.Set("Height", 720u);

    // Fallback extent supplied from DLSSG.Backbuffer descriptor (1920x1080)
    DlssgSanitizationParams sanitization = ResolveDlssgSanitization(&params, 1920u, 1080u);
    assert(sanitization.NeedsSanitization());
    assert(sanitization.forcedWidth == 1920u);
    assert(sanitization.forcedHeight == 1080u);

    DlssgParameterSanitizer sanitizer(&params, sanitization.filterOutputReal, sanitization.forcedWidth,
                                      sanitization.forcedHeight);

    unsigned int uWidth = 0, uHeight = 0;
    assert(sanitizer.Get("Width", &uWidth) == NVSDK_NGX_Result_Success && uWidth == 1920u);
    assert(sanitizer.Get("Height", &uHeight) == NVSDK_NGX_Result_Success && uHeight == 1080u);

    std::printf("  [PASS] Test_ResolveDlssgSanitization_DirectNgxBackbufferFallback\n");
}

void Test_ResolveDlssgSanitization_DlaaNoMismatch()
{
    // Native DLAA: Render dimensions equal display dimensions (1920x1080)
    MockNgxParameters params;
    params.Set("Width", 1920u);
    params.Set("Height", 1080u);
    params.Set("OutWidth", 1920u);
    params.Set("OutHeight", 1080u);

    DlssgSanitizationParams sanitization = ResolveDlssgSanitization(&params, 1920u, 1080u);
    assert(!sanitization.NeedsSanitization());
    assert(sanitization.forcedWidth == 0);
    assert(sanitization.forcedHeight == 0);

    std::printf("  [PASS] Test_ResolveDlssgSanitization_DlaaNoMismatch\n");
}

void Test_ScopedNgxDimensionRestoration_Isolation()
{
    // Simulates shared parameter map shared between DLSS and DLSSG
    MockNgxParameters params;
    params.Set("Width", 1920u);
    params.Set("Height", 1080u);
    params.Set("OutWidth", 1920u);
    params.Set("OutHeight", 1080u);

    {
        // Entering DLSS creation / initialization scope
        ScopedNgxDimensionRestoration restorer(&params);

        // DLSSFeature mutates InParameters with render dimensions
        params.Set("Width", 1280u);
        params.Set("Height", 720u);

        unsigned int inScopeWidth = 0;
        params.Get("Width", &inScopeWidth);
        assert(inScopeWidth == 1280u);
    } // restorer destructor runs here

    // Exiting DLSS creation: original caller dimensions must be restored
    unsigned int restoredWidth = 0, restoredHeight = 0;
    params.Get("Width", &restoredWidth);
    params.Get("Height", &restoredHeight);
    assert(restoredWidth == 1920u);
    assert(restoredHeight == 1080u);

    std::printf("  [PASS] Test_ScopedNgxDimensionRestoration_Isolation\n");
}

void Test_AmpereMfgLoader_TuringSpoofArchResolution()
{
    using AmpereMfgLoader::FormatIniContent030;

    // Turing GPU (0x160): auto/absent SpoofArchToGame must resolve to 0
    std::string iniTuringAuto =
        FormatIniContent030(5, 1, "Auto", "Auto", 0, "Auto", 1, "Auto", false, 0.0f, false, false, 0x160);
    assert(iniTuringAuto.find("SpoofArchToGame=0\n") != std::string::npos);

    // Turing GPU (0x160): explicit user override "1" must be respected as an escape hatch
    std::string iniTuringExplicit =
        FormatIniContent030(5, 1, "Auto", "Auto", 0, "Auto", 1, "1", false, 0.0f, false, false, 0x160);
    assert(iniTuringExplicit.find("SpoofArchToGame=1\n") != std::string::npos);

    // Ampere GPU (0x170): auto SpoofArchToGame is omitted to preserve factory convention
    std::string iniAmpereAuto =
        FormatIniContent030(5, 1, "Auto", "Auto", 0, "Auto", 1, "Auto", false, 0.0f, false, false, 0x170);
    assert(iniAmpereAuto.find("SpoofArchToGame") == std::string::npos);

    // Ada GPU (0x190): auto SpoofArchToGame is omitted
    std::string iniAdaAuto =
        FormatIniContent030(5, 1, "Auto", "Auto", 0, "Auto", 1, "Auto", false, 0.0f, false, false, 0x190);
    assert(iniAdaAuto.find("SpoofArchToGame") == std::string::npos);

    // Internal mode active: always emits SpoofArchToGame=0 regardless of GPU
    std::string iniInternalMode =
        FormatIniContent030(5, 1, "Auto", "Auto", 0, "Auto", 1, "Auto", false, 0.0f, false, true, 0x170);
    assert(iniInternalMode.find("SpoofArchToGame=0\n") != std::string::npos);

    std::printf("  [PASS] Test_AmpereMfgLoader_TuringSpoofArchResolution\n");
}

int main()
{
    std::printf("=== Running DLSS-G Parameter Sanitization Unit Tests ===\n");

    Test_ShouldFilterDlssgOutputReal_NullParameters();
    Test_ShouldFilterDlssgOutputReal_KeyNotPresent();
    Test_ShouldFilterDlssgOutputReal_NonNullResource();
    Test_ShouldFilterDlssgOutputReal_NullResource();
    Test_DlssgParameterSanitizer_FilteredGet();
    Test_DlssgParameterSanitizer_UnfilteredGet();
    Test_DlssgParameterSanitizer_SetForwarding();
    Test_Simulation_Streamline_MultiPass_MFG();

    Test_ResolveDlssgSanitization_UpscalerDimensionMismatch();
    Test_ResolveDlssgSanitization_DirectNgxBackbufferFallback();
    Test_ResolveDlssgSanitization_DlaaNoMismatch();
    Test_ScopedNgxDimensionRestoration_Isolation();
    Test_AmpereMfgLoader_TuringSpoofArchResolution();

    std::printf("=== All DLSS-G Parameter Sanitization Unit Tests PASSED! ===\n");
    return 0;
}

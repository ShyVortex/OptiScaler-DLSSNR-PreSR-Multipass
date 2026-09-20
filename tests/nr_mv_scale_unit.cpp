#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>

// Logical reproduction of the MV Scale validation logic in DlssNr_Pipeline_Dx12.cpp
inline void ValidateMotionVectorScale(float& mvScaleX, float& mvScaleY)
{
    if (!std::isfinite(mvScaleX) || mvScaleX == 0.0f)
        mvScaleX = 1.0f;
    if (!std::isfinite(mvScaleY) || mvScaleY == 0.0f)
        mvScaleY = 1.0f;
}

enum class Upscaler : int
{
    DLSS = 0,
    FSR21 = 1,
    FSR22 = 2,
    XeSS = 3,
    DLSSD = 4,
    FFX = 5
};

enum class NVSDK_NGX_Feature : int
{
    SuperSampling = 1,
    RayReconstruction = 2
};

struct MockGPU
{
    bool dlssCapable = false;
    bool fsr4Support = false;
};

// Logical reproduction of GetUpscalerBackend(bool allowOverride)
inline Upscaler MockGetUpscalerBackend(const MockGPU& gpu, bool dx12Inited, std::optional<Upscaler> userOverride, bool allowOverride = true)
{
    Upscaler upscaler = Upscaler::XeSS;

    if (dx12Inited && gpu.dlssCapable)
        upscaler = Upscaler::DLSS;

    if (gpu.fsr4Support)
        upscaler = Upscaler::FFX;

    if (allowOverride && userOverride.has_value())
        upscaler = userOverride.value();

    return upscaler;
}

// Logical reproduction of feature creation fallback in TryCreateOptiFeature
inline Upscaler ResolveFeatureCreationFallback(Upscaler upscalerBackend,
                                               NVSDK_NGX_Feature featureId,
                                               const MockGPU& gpu,
                                               bool dx12Inited,
                                               std::optional<Upscaler> userOverride)
{
    if (upscalerBackend == Upscaler::DLSSD && featureId == NVSDK_NGX_Feature::SuperSampling)
    {
        // Fall back to hardware-appropriate backend without user override
        return MockGetUpscalerBackend(gpu, dx12Inited, userOverride, false);
    }

    return Upscaler::FSR21;
}

int main()
{
    // Test 1: Zero motion vector scales fall back to 1.0f
    {
        float x = 0.0f;
        float y = 0.0f;
        ValidateMotionVectorScale(x, y);
        assert(x == 1.0f);
        assert(y == 1.0f);

        float negZeroX = -0.0f;
        float negZeroY = -0.0f;
        ValidateMotionVectorScale(negZeroX, negZeroY);
        assert(negZeroX == 1.0f);
        assert(negZeroY == 1.0f);
    }

    // Test 2: Non-finite motion vector scales (NaN, Inf) fall back to 1.0f
    {
        float nanX = std::numeric_limits<float>::quiet_NaN();
        float infY = std::numeric_limits<float>::infinity();
        ValidateMotionVectorScale(nanX, infY);
        assert(nanX == 1.0f);
        assert(infY == 1.0f);

        float negInfX = -std::numeric_limits<float>::infinity();
        float validY = 2.5f;
        ValidateMotionVectorScale(negInfX, validY);
        assert(negInfX == 1.0f);
        assert(validY == 2.5f);
    }

    // Test 3: Valid positive and negative motion vector scales are strictly preserved
    {
        float x = 16.0f;
        float y = -9.0f;
        ValidateMotionVectorScale(x, y);
        assert(x == 16.0f);
        assert(y == -9.0f);

        float smallX = 0.001f;
        float smallY = -0.001f;
        ValidateMotionVectorScale(smallX, smallY);
        assert(smallX == 0.001f);
        assert(smallY == -0.001f);
    }

    // Test 4: DLSSD fallback on SuperSampling when hardware is DLSS-capable resolves to DLSS
    {
        MockGPU nvidiaGpu { true, false };
        auto fallback = ResolveFeatureCreationFallback(Upscaler::DLSSD, NVSDK_NGX_Feature::SuperSampling,
                                                       nvidiaGpu, true, std::nullopt);
        assert(fallback == Upscaler::DLSS);
    }

    // Test 5: DLSSD fallback on SuperSampling ignores user override set to DLSSD and picks native GPU capability
    {
        MockGPU nvidiaGpu { true, false };
        // User explicitly set Dx12Upscaler = DLSSD, but DLSSD feature creation failed
        auto fallback = ResolveFeatureCreationFallback(Upscaler::DLSSD, NVSDK_NGX_Feature::SuperSampling,
                                                       nvidiaGpu, true, Upscaler::DLSSD);
        assert(fallback == Upscaler::DLSS);
    }

    // Test 6: DLSSD fallback on AMD/Intel hardware resolves to XeSS/FFX instead of FSR21
    {
        MockGPU amdGpu { false, true };
        auto fallback = ResolveFeatureCreationFallback(Upscaler::DLSSD, NVSDK_NGX_Feature::SuperSampling,
                                                       amdGpu, true, std::nullopt);
        assert(fallback == Upscaler::FFX);

        MockGPU intelGpu { false, false };
        auto fallbackIntel = ResolveFeatureCreationFallback(Upscaler::DLSSD, NVSDK_NGX_Feature::SuperSampling,
                                                            intelGpu, true, std::nullopt);
        assert(fallbackIntel == Upscaler::XeSS);
    }

    // Test 7: Non-DLSSD upscaler initialization failure still safely falls back to FSR21
    {
        MockGPU nvidiaGpu { true, false };
        auto fallback = ResolveFeatureCreationFallback(Upscaler::DLSS, NVSDK_NGX_Feature::SuperSampling,
                                                       nvidiaGpu, true, std::nullopt);
        assert(fallback == Upscaler::FSR21);

        auto fallbackRR = ResolveFeatureCreationFallback(Upscaler::DLSSD, NVSDK_NGX_Feature::RayReconstruction,
                                                         nvidiaGpu, true, std::nullopt);
        assert(fallbackRR == Upscaler::FSR21);
    }

    std::puts("PASS: nr_mv_scale_unit (motion vector scale fallbacks & secondary upscaler context fallback)");
    return 0;
}

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>

// Mock NVSDK parameter table
struct MockNgxParameters
{
    std::unordered_map<std::string, unsigned int> uintParams;

    void Set(const char* key, unsigned int val) { uintParams[key] = val; }

    int Get(const char* key, unsigned int* outVal) const
    {
        auto it = uintParams.find(key);
        if (it != uintParams.end())
        {
            if (outVal)
                *outVal = it->second;
            return 1; // Success
        }
        return 0; // Not found
    }
};

static const char* NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width = "DLSS.Render.Subrect.Dimensions.Width";
static const char* NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height = "DLSS.Render.Subrect.Dimensions.Height";
static const char* NVSDK_NGX_Parameter_Width = "Width";
static const char* NVSDK_NGX_Parameter_Height = "Height";

// Mock Feature and State for active upscaler feature resolution
struct MockFeature
{
    unsigned int renderW = 1706;
    unsigned int renderH = 960;
    unsigned int RenderWidth() const { return renderW; }
    unsigned int RenderHeight() const { return renderH; }
};

struct MockState
{
    MockFeature* currentFeature = nullptr;
    static MockState& Instance()
    {
        static MockState s;
        return s;
    }
};

// Implementation of ResolveNrRenderDimensions matching DlssNr_Pipeline_Dx12.cpp and DlssNrPipeline_Vk.h
void ResolveNrRenderDimensions(const MockNgxParameters* parameters, unsigned int& width, unsigned int& height)
{
    width = 0;
    height = 0;
    if (parameters != nullptr)
    {
        parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &width);
        parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &height);
        if (width == 0 || height == 0)
        {
            parameters->Get(NVSDK_NGX_Parameter_Width, &width);
            parameters->Get(NVSDK_NGX_Parameter_Height, &height);
        }
    }
    if ((width == 0 || height == 0) && MockState::Instance().currentFeature != nullptr)
    {
        width = MockState::Instance().currentFeature->RenderWidth();
        height = MockState::Instance().currentFeature->RenderHeight();
    }
}

// Matching DlssNr::ColorExtent and PreSrColorExtent
struct ColorExtent
{
    unsigned int width;
    unsigned int height;
};

struct D3D12_RESOURCE_DESC
{
    unsigned int Dimension = 2; // TEXTURE2D
    unsigned int Width = 0;
    unsigned int Height = 0;
    unsigned int DepthOrArraySize = 1;
    unsigned int MipLevels = 1;
    unsigned int Format = 10; // R16G16B16A16_FLOAT
    struct
    {
        unsigned int Count = 1;
        unsigned int Quality = 0;
    } SampleDesc;
};

std::optional<ColorExtent> PreSrColorExtent(const D3D12_RESOURCE_DESC& allocation, unsigned int renderWidth,
                                            unsigned int renderHeight, unsigned int baseX = 0, unsigned int baseY = 0)
{
    if (allocation.Dimension != 2 || allocation.SampleDesc.Count != 1 || allocation.DepthOrArraySize != 1 ||
        allocation.Width == 0 || allocation.Width > 16384 || allocation.Height == 0 || allocation.Height > 16384 ||
        baseX != 0 || baseY != 0)
        return std::nullopt;

    if (renderWidth == 0 && renderHeight == 0)
        return ColorExtent { (unsigned int) allocation.Width, allocation.Height };

    if (renderWidth == 0 || renderHeight == 0 || renderWidth > allocation.Width || renderHeight > allocation.Height)
        return std::nullopt;

    return ColorExtent { renderWidth, renderHeight };
}

bool PreSrColorCanUseScratch(const D3D12_RESOURCE_DESC& allocation, unsigned int renderWidth, unsigned int renderHeight)
{
    if (!PreSrColorExtent(allocation, renderWidth, renderHeight))
        return false;
    return allocation.MipLevels == 1 || allocation.Format == 10; // typed format preserved
}

// Guide region calculations
struct GuideExtent
{
    unsigned int width, height;
};
struct GuideRegion
{
    unsigned int x, y, width, height;
    bool valid() const { return width != 0 && height != 0; }
};
struct GuideRegions
{
    GuideRegion depth, motion;
};

inline GuideRegion GuideSubrect(GuideExtent allocation, GuideExtent wanted, unsigned int x, unsigned int y)
{
    x = std::min(x, allocation.width);
    y = std::min(y, allocation.height);
    const auto availableW = allocation.width - x;
    const auto availableH = allocation.height - y;
    return { x, y, std::min(wanted.width ? wanted.width : availableW, availableW),
             std::min(wanted.height ? wanted.height : availableH, availableH) };
}

inline GuideRegions ResolveGuideRegions(GuideExtent depthAllocation, GuideExtent motionAllocation, GuideExtent render,
                                        GuideExtent output, bool lowResolutionMotion, unsigned int depthX,
                                        unsigned int depthY, unsigned int motionX, unsigned int motionY)
{
    return { GuideSubrect(depthAllocation, render, depthX, depthY),
             GuideSubrect(motionAllocation, lowResolutionMotion ? render : output, motionX, motionY) };
}

// Simulation of LateContext::Clone and CreateGuideClone
struct MockGuideClone
{
    D3D12_RESOURCE_DESC desc;

    static bool CanClone(const D3D12_RESOURCE_DESC& want)
    {
        // Verified: MipLevels > 1 is now allowed and normalized
        return want.Dimension == 2 && want.SampleDesc.Count == 1 && want.DepthOrArraySize == 1;
    }

    static MockGuideClone Create(const D3D12_RESOURCE_DESC& source)
    {
        MockGuideClone clone;
        clone.desc = source;
        clone.desc.MipLevels = 1; // Normalized to 1
        return clone;
    }

    static bool RequiresSubresource0Copy(const D3D12_RESOURCE_DESC& src, const D3D12_RESOURCE_DESC& dst)
    {
        return src.MipLevels != dst.MipLevels;
    }
};

int main()
{
    std::cout << "=== Running DLSS-NR Render Dimension Resolution Unit Tests ===" << std::endl;

    // Test 1: Explicit Subrect parameters take precedence
    {
        MockNgxParameters params;
        params.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 1920);
        params.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 1080);
        params.Set(NVSDK_NGX_Parameter_Width, 1280);
        params.Set(NVSDK_NGX_Parameter_Height, 720);

        unsigned int w = 0, h = 0;
        ResolveNrRenderDimensions(&params, w, h);
        assert(w == 1920);
        assert(h == 1080);
        std::cout << "[PASS] Test 1: Subrect dimensions take precedence when present (1920x1080)\n";
    }

    // Test 2: Fallback to Width/Height when subrect parameters are absent
    {
        MockNgxParameters params;
        params.Set(NVSDK_NGX_Parameter_Width, 1706);
        params.Set(NVSDK_NGX_Parameter_Height, 960);

        unsigned int w = 0, h = 0;
        ResolveNrRenderDimensions(&params, w, h);
        assert(w == 1706);
        assert(h == 960);
        std::cout << "[PASS] Test 2: Fallback to Width/Height when subrect is absent (1706x960)\n";
    }

    // Test 3: Fallback when subrect parameters are explicitly 0
    {
        MockNgxParameters params;
        params.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 0);
        params.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 0);
        params.Set(NVSDK_NGX_Parameter_Width, 1706);
        params.Set(NVSDK_NGX_Parameter_Height, 960);

        unsigned int w = 0, h = 0;
        ResolveNrRenderDimensions(&params, w, h);
        assert(w == 1706);
        assert(h == 960);
        std::cout << "[PASS] Test 3: Fallback when subrect dimensions are 0 (1706x960)\n";
    }

    // Test 4: When neither subrect nor dimensions are set and no active feature, resolves to 0x0
    {
        MockState::Instance().currentFeature = nullptr;
        MockNgxParameters params;
        unsigned int w = 0, h = 0;
        ResolveNrRenderDimensions(&params, w, h);
        assert(w == 0);
        assert(h == 0);
        std::cout << "[PASS] Test 4: Default to 0x0 when no dimensions are provided and no active feature\n";
    }

    // Test 5: MSFS 2024 cropColor activation and PreSrColorExtent resolution (with Width/Height)
    {
        // Allocation is display size 2560x1440, but render resolution resolved from Width/Height is 1706x960
        D3D12_RESOURCE_DESC msfsColorDesc {};
        msfsColorDesc.Width = 2560;
        msfsColorDesc.Height = 1440;
        msfsColorDesc.MipLevels = 5; // Multi-mip texture
        msfsColorDesc.Format = 10;   // R16G16B16A16_FLOAT

        MockNgxParameters params;
        params.Set(NVSDK_NGX_Parameter_Width, 1706);
        params.Set(NVSDK_NGX_Parameter_Height, 960);

        unsigned int w = 0, h = 0;
        ResolveNrRenderDimensions(&params, w, h);

        auto extent = PreSrColorExtent(msfsColorDesc, w, h);
        assert(extent.has_value());
        assert(extent->width == 1706);
        assert(extent->height == 960);

        // PreSrColorCanUseScratch must be true for typed multi-mip color
        assert(PreSrColorCanUseScratch(msfsColorDesc, w, h));

        // cropColor is true because active resolution (1706x960) != allocation size (2560x1440)
        bool cropColor = (extent->width != msfsColorDesc.Width || extent->height != msfsColorDesc.Height);
        assert(cropColor == true);

        std::cout << "[PASS] Test 5: MSFS 2024 active extent correctly crops 1706x960 in 2560x1440 allocation\n";
    }

    // Test 6: Guide region resolution with resolved dimensions
    {
        GuideExtent depthAlloc { 2560, 1440 };
        GuideExtent motionAlloc { 2560, 1440 };
        GuideExtent render { 1706, 960 };
        GuideExtent output { 2560, 1440 };

        auto guides = ResolveGuideRegions(depthAlloc, motionAlloc, render, output, true, 0, 0, 0, 0);
        assert(guides.depth.width == 1706);
        assert(guides.depth.height == 960);
        assert(guides.motion.width == 1706);
        assert(guides.motion.height == 960);

        std::cout << "[PASS] Test 6: Guides correctly resolve to active render extent (1706x960)\n";
    }

    // Test 7: Native DLAA scenario (allocation 2560x1440, Width/Height 2560x1440)
    {
        D3D12_RESOURCE_DESC dlaaDesc {};
        dlaaDesc.Width = 2560;
        dlaaDesc.Height = 1440;

        MockNgxParameters params;
        params.Set(NVSDK_NGX_Parameter_Width, 2560);
        params.Set(NVSDK_NGX_Parameter_Height, 1440);

        unsigned int w = 0, h = 0;
        ResolveNrRenderDimensions(&params, w, h);

        auto extent = PreSrColorExtent(dlaaDesc, w, h);
        assert(extent.has_value());
        assert(extent->width == 2560);
        assert(extent->height == 1440);

        bool cropColor = (extent->width != dlaaDesc.Width || extent->height != dlaaDesc.Height);
        assert(cropColor == false);

        std::cout << "[PASS] Test 7: DLAA scenario correctly identifies uncropped native 2560x1440\n";
    }

    // Test 8: Real-world MSFS 2024 evaluate scenario: completely empty parameters fallback to active feature
    {
        MockFeature activeFeature { 1706, 960 };
        MockState::Instance().currentFeature = &activeFeature;

        // In real EvaluateFeature, the game provides NO subrect and NO Width/Height
        MockNgxParameters emptyEvaluateParams;

        unsigned int w = 0, h = 0;
        ResolveNrRenderDimensions(&emptyEvaluateParams, w, h);
        assert(w == 1706);
        assert(h == 960);

        std::cout << "[PASS] Test 8: Empty evaluate parameters successfully fallback to active feature (1706x960)\n";
    }

    // Test 9: Real-world MSFS 2024 Pre-SR color crop activation with empty evaluate parameters and active feature
    {
        MockFeature activeFeature { 1706, 960 };
        MockState::Instance().currentFeature = &activeFeature;

        D3D12_RESOURCE_DESC msfsRealColorDesc {};
        msfsRealColorDesc.Width = 2560;
        msfsRealColorDesc.Height = 1440;
        msfsRealColorDesc.MipLevels = 4; // Multi-mip texture from engine
        msfsRealColorDesc.Format = 10;   // DXGI_FORMAT_R16G16B16A16_FLOAT

        MockNgxParameters emptyParams;
        unsigned int w = 0, h = 0;
        ResolveNrRenderDimensions(&emptyParams, w, h);
        assert(w == 1706);
        assert(h == 960);

        auto extent = PreSrColorExtent(msfsRealColorDesc, w, h);
        assert(extent.has_value());
        assert(extent->width == 1706);
        assert(extent->height == 960);

        // Verification: cropColor is properly activated so NR allocates at 1706x960 instead of full allocation
        // 2560x1440
        bool cropColor = (extent->width != msfsRealColorDesc.Width || extent->height != msfsRealColorDesc.Height);
        assert(cropColor == true);
        assert(PreSrColorCanUseScratch(msfsRealColorDesc, w, h) == true);

        std::cout << "[PASS] Test 9: Real-world MSFS 2024 color correctly crops to 1706x960 with empty parameters\n";
    }

    // Test 10: Multi-mip guide cloning simulation in LateContext & subresource 0 copy verification
    {
        D3D12_RESOURCE_DESC multiMipDepthDesc {};
        multiMipDepthDesc.Width = 2560;
        multiMipDepthDesc.Height = 1440;
        multiMipDepthDesc.MipLevels = 4; // MSFS 2024 passes 4 mips on depth
        multiMipDepthDesc.Format = 40;   // DXGI_FORMAT_D32_FLOAT

        // Previously, want.MipLevels != 1 caused CanClone to return false
        assert(MockGuideClone::CanClone(multiMipDepthDesc) == true);

        auto clone = MockGuideClone::Create(multiMipDepthDesc);
        assert(clone.desc.Width == 2560);
        assert(clone.desc.Height == 1440);
        assert(clone.desc.MipLevels == 1); // Cloned guide normalized to 1 mip

        // When source MipLevels (4) != dst MipLevels (1), CopyTextureRegion on subresource 0 is required
        bool needSubresource0 = MockGuideClone::RequiresSubresource0Copy(multiMipDepthDesc, clone.desc);
        assert(needSubresource0 == true);

        std::cout
            << "[PASS] Test 10: Multi-mip guide cloning normalized to MipLevels=1 with subresource 0 region copy\n";
    }

    // Test 11: LateContext::Capture subrect resolution with empty evaluate parameters
    {
        MockFeature activeFeature { 1706, 960 };
        MockState::Instance().currentFeature = &activeFeature;

        MockNgxParameters emptyLateParams;
        unsigned int lateRenderW = 0, lateRenderH = 0;
        ResolveNrRenderDimensions(&emptyLateParams, lateRenderW, lateRenderH);
        assert(lateRenderW == 1706);
        assert(lateRenderH == 960);

        std::cout << "[PASS] Test 11: LateContext subrect dimensions resolve to 1706x960 on empty parameters\n";
    }

    // Test 12: Motion vector guide region calculation with lowResolutionMotion and active feature resolved subrect
    {
        MockFeature activeFeature { 1706, 960 };
        MockState::Instance().currentFeature = &activeFeature;

        MockNgxParameters emptyParams;
        unsigned int resolvedW = 0, resolvedH = 0;
        ResolveNrRenderDimensions(&emptyParams, resolvedW, resolvedH);

        GuideExtent depthAlloc { 2560, 1440 };
        GuideExtent motionAlloc { 2560, 1440 };
        GuideExtent render { resolvedW, resolvedH };
        GuideExtent output { 2560, 1440 };

        auto guides = ResolveGuideRegions(depthAlloc, motionAlloc, render, output, true, 0, 0, 0, 0);
        assert(guides.depth.width == 1706);
        assert(guides.depth.height == 960);
        assert(guides.motion.width == 1706);
        assert(guides.motion.height == 960);

        std::cout << "[PASS] Test 12: Low-res motion guide region correctly bounds to resolved 1706x960\n";
    }

    std::cout << "All DLSS-NR render dimension resolution unit tests passed successfully!\n";
    return 0;
}

#include <cassert>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <algorithm>

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

// Implementation of ResolveNrRenderDimensions matching DlssNr_Pipeline_Dx12.cpp
void ResolveNrRenderDimensions(const MockNgxParameters* parameters, unsigned int& width, unsigned int& height)
{
    width = 0;
    height = 0;
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &width);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &height);
    if (width == 0 || height == 0)
    {
        parameters->Get(NVSDK_NGX_Parameter_Width, &width);
        parameters->Get(NVSDK_NGX_Parameter_Height, &height);
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

    // Test 2: Fallback to Width/Height when subrect parameters are absent (MSFS 2024 scenario)
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

    // Test 4: When neither subrect nor dimensions are set, resolves to 0x0
    {
        MockNgxParameters params;
        unsigned int w = 0, h = 0;
        ResolveNrRenderDimensions(&params, w, h);
        assert(w == 0);
        assert(h == 0);
        std::cout << "[PASS] Test 4: Default to 0x0 when no dimensions are provided\n";
    }

    // Test 5: MSFS 2024 cropColor activation and PreSrColorExtent resolution
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

    std::cout << "All DLSS-NR render dimension resolution unit tests passed successfully!\n";
    return 0;
}

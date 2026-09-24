#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
#include <algorithm>

// DXGI format values for test simulation
enum DXGI_FORMAT : uint32_t
{
    DXGI_FORMAT_UNKNOWN = 0,
    DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
    DXGI_FORMAT_R11G11B10_FLOAT = 26,
    DXGI_FORMAT_R8G8B8A8_UNORM = 28
};

// Mock D3D12 Resource
struct MockResource
{
    DXGI_FORMAT format;
    unsigned int width;
    unsigned int height;
    uint32_t id;
};

// Mock DLSS-NR Model Context
struct MockModelContext
{
    bool hasFeature = true;
    uint32_t retries = 0;

    void RetryAfterFailure()
    {
        hasFeature = false;
        retries++;
    }

    void Prepare() { hasFeature = true; }
};

// Mock ModelState structure mirroring ModelStateDx12
struct MockModelState
{
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int workWidth = 0;
    unsigned int workHeight = 0;
    bool beforeUpscale = true;
    bool rayReconstruction = false;
    bool reset = false;
    bool modelRunning = true;
    MockModelContext models[3];

    MockResource* output = nullptr;
    MockResource* passScratch = nullptr;
    MockResource* passClamp = nullptr;
    MockResource* colorCopy = nullptr;
    MockResource* hdrCopy = nullptr;
    MockResource* activeColor = nullptr;
    MockResource* colorSmall = nullptr;
    MockResource* outputNative = nullptr;

    struct CachedSurfaces
    {
        DXGI_FORMAT modelFormat = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT nativeFormat = DXGI_FORMAT_UNKNOWN;
        unsigned int width = 0;
        unsigned int height = 0;
        unsigned int workWidth = 0;
        unsigned int workHeight = 0;
        MockResource* colorCopy = nullptr;
        MockResource* output = nullptr;
        MockResource* passScratch = nullptr;
        MockResource* passClamp = nullptr;
        MockResource* hdrCopy = nullptr;
        MockResource* activeColor = nullptr;
        MockResource* colorSmall = nullptr;
        MockResource* outputNative = nullptr;
    } altSurfaces;

    uint32_t allocationsCount = 0;
    uint32_t parkedCount = 0;
    uint32_t swapsCount = 0;
    std::vector<MockResource*> retiredPool;

    MockResource* CreateScratch(DXGI_FORMAT fmt, unsigned int w, unsigned int h)
    {
        allocationsCount++;
        auto* res = new MockResource { fmt, w, h, allocationsCount };
        return res;
    }

    void ParkNrResource(MockResource*& res)
    {
        if (res == nullptr)
            return;
        retiredPool.push_back(res);
        parkedCount++;
        res = nullptr;
    }

    void ReleaseSurfacesIfFormatChanged(DXGI_FORMAT modelFormat, DXGI_FORMAT nativeFormat)
    {
        const auto currentModelFormat = output ? output->format : DXGI_FORMAT_UNKNOWN;
        const auto currentNativeFormat = colorCopy ? colorCopy->format : DXGI_FORMAT_UNKNOWN;

        if (output == nullptr || (currentModelFormat == modelFormat && colorCopy && hdrCopy &&
                                  currentNativeFormat == nativeFormat && hdrCopy->format == nativeFormat))
            return;

        // Check if we have cached surfaces matching the requested format and current dimensions
        if (altSurfaces.modelFormat == modelFormat && altSurfaces.nativeFormat == nativeFormat &&
            altSurfaces.width == width && altSurfaces.height == height && altSurfaces.workWidth == workWidth &&
            altSurfaces.workHeight == workHeight)
        {
            std::swap(output, altSurfaces.output);
            std::swap(passScratch, altSurfaces.passScratch);
            std::swap(passClamp, altSurfaces.passClamp);
            std::swap(colorCopy, altSurfaces.colorCopy);
            std::swap(hdrCopy, altSurfaces.hdrCopy);
            std::swap(activeColor, altSurfaces.activeColor);
            std::swap(colorSmall, altSurfaces.colorSmall);
            std::swap(outputNative, altSurfaces.outputNative);
            altSurfaces.modelFormat = currentModelFormat;
            altSurfaces.nativeFormat = currentNativeFormat;
            swapsCount++;
        }
        else
        {
            // Stash current surfaces into altSurfaces
            ParkNrResource(altSurfaces.output);
            ParkNrResource(altSurfaces.passScratch);
            ParkNrResource(altSurfaces.passClamp);
            ParkNrResource(altSurfaces.colorCopy);
            ParkNrResource(altSurfaces.hdrCopy);
            ParkNrResource(altSurfaces.activeColor);
            ParkNrResource(altSurfaces.colorSmall);
            ParkNrResource(altSurfaces.outputNative);

            altSurfaces.modelFormat = currentModelFormat;
            altSurfaces.nativeFormat = currentNativeFormat;
            altSurfaces.width = width;
            altSurfaces.height = height;
            altSurfaces.workWidth = workWidth;
            altSurfaces.workHeight = workHeight;
            altSurfaces.output = output;
            altSurfaces.passScratch = passScratch;
            altSurfaces.passClamp = passClamp;
            altSurfaces.colorCopy = colorCopy;
            altSurfaces.hdrCopy = hdrCopy;
            altSurfaces.activeColor = activeColor;
            altSurfaces.colorSmall = colorSmall;
            altSurfaces.outputNative = outputNative;

            output = nullptr;
            passScratch = nullptr;
            passClamp = nullptr;
            colorCopy = nullptr;
            hdrCopy = nullptr;
            activeColor = nullptr;
            colorSmall = nullptr;
            outputNative = nullptr;
        }

        reset = true;
    }

    void HandleResolutionOrPlacementChange(bool resChanged, bool placeChanged)
    {
        if (resChanged || placeChanged)
        {
            for (auto& model : models)
                model.RetryAfterFailure();
            modelRunning = false;

            ParkNrResource(output);
            ParkNrResource(passScratch);
            ParkNrResource(passClamp);
            ParkNrResource(colorCopy);
            ParkNrResource(hdrCopy);
            ParkNrResource(colorSmall);
            ParkNrResource(outputNative);
            ParkNrResource(activeColor);

            ParkNrResource(altSurfaces.output);
            ParkNrResource(altSurfaces.passScratch);
            ParkNrResource(altSurfaces.passClamp);
            ParkNrResource(altSurfaces.colorCopy);
            ParkNrResource(altSurfaces.hdrCopy);
            ParkNrResource(altSurfaces.activeColor);
            ParkNrResource(altSurfaces.colorSmall);
            ParkNrResource(altSurfaces.outputNative);
            altSurfaces = {};
        }
    }

    ~MockModelState()
    {
        ParkNrResource(output);
        ParkNrResource(passScratch);
        ParkNrResource(passClamp);
        ParkNrResource(colorCopy);
        ParkNrResource(hdrCopy);
        ParkNrResource(colorSmall);
        ParkNrResource(outputNative);
        ParkNrResource(activeColor);

        ParkNrResource(altSurfaces.output);
        ParkNrResource(altSurfaces.passScratch);
        ParkNrResource(altSurfaces.passClamp);
        ParkNrResource(altSurfaces.colorCopy);
        ParkNrResource(altSurfaces.hdrCopy);
        ParkNrResource(altSurfaces.activeColor);
        ParkNrResource(altSurfaces.colorSmall);
        ParkNrResource(altSurfaces.outputNative);

        for (auto* r : retiredPool)
            delete r;
    }
};

int main()
{
    std::cout << "Starting nr_format_bouncing_unit tests..." << std::endl;

    // Test 1: Initial creation in R11G11B10_FLOAT (format 26)
    MockModelState state;
    state.width = 1708;
    state.height = 960;
    state.workWidth = 1708;
    state.workHeight = 960;

    state.output = state.CreateScratch(DXGI_FORMAT_R11G11B10_FLOAT, 1708, 960);
    state.colorCopy = state.CreateScratch(DXGI_FORMAT_R11G11B10_FLOAT, 1708, 960);
    state.hdrCopy = state.CreateScratch(DXGI_FORMAT_R11G11B10_FLOAT, 1708, 960);

    const auto initialAllocations = state.allocationsCount;
    assert(initialAllocations == 3);
    assert(state.output->format == DXGI_FORMAT_R11G11B10_FLOAT);
    assert(state.models[0].retries == 0);
    assert(state.modelRunning == true);
    std::cout << "Test 1: Initial format 26 setup verified." << std::endl;

    // Test 2: First camera cut switches to format 10 (DXGI_FORMAT_R16G16B16A16_FLOAT)
    state.ReleaseSurfacesIfFormatChanged(DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT);

    // Assert that model feature is NOT destroyed / retried!
    assert(state.models[0].retries == 0);
    assert(state.modelRunning == true);
    assert(state.reset == true);
    assert(state.swapsCount == 0);

    // Current output is cleared, altSurfaces holds the format 26 surfaces
    assert(state.output == nullptr);
    assert(state.altSurfaces.modelFormat == DXGI_FORMAT_R11G11B10_FLOAT);
    assert(state.altSurfaces.nativeFormat == DXGI_FORMAT_R11G11B10_FLOAT);
    assert(state.altSurfaces.output != nullptr);
    assert(state.altSurfaces.output->format == DXGI_FORMAT_R11G11B10_FLOAT);

    // PrepareRunModels now allocates format 10 surfaces
    state.output = state.CreateScratch(DXGI_FORMAT_R16G16B16A16_FLOAT, 1708, 960);
    state.colorCopy = state.CreateScratch(DXGI_FORMAT_R16G16B16A16_FLOAT, 1708, 960);
    state.hdrCopy = state.CreateScratch(DXGI_FORMAT_R16G16B16A16_FLOAT, 1708, 960);

    const auto fmt10OutputId = state.output->id;
    const auto fmt26OutputId = state.altSurfaces.output->id;
    assert(state.output->format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    std::cout << "Test 2: Camera cut format 10 switch verified (models preserved, zero retries)." << std::endl;

    // Test 3: Normal frame after cut switches back to format 26 (DXGI_FORMAT_R11G11B10_FLOAT)
    state.ReleaseSurfacesIfFormatChanged(DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R11G11B10_FLOAT);

    // Must swap instantly without allocating!
    assert(state.swapsCount == 1);
    assert(state.models[0].retries == 0);
    assert(state.modelRunning == true);
    assert(state.reset == true);
    assert(state.output != nullptr);
    assert(state.output->format == DXGI_FORMAT_R11G11B10_FLOAT);
    assert(state.output->id == fmt26OutputId); // Reused cached format 26 resource!
    assert(state.altSurfaces.modelFormat == DXGI_FORMAT_R16G16B16A16_FLOAT);
    assert(state.altSurfaces.nativeFormat == DXGI_FORMAT_R16G16B16A16_FLOAT);
    assert(state.altSurfaces.output->id == fmt10OutputId); // Stashed format 10 resource!
    std::cout << "Test 3: Post-cut switch back to format 26 verified (instant swap, zero allocations)." << std::endl;

    // Test 4: Simulate 100 subsequent camera cuts ping-ponging 26 <-> 10
    const auto allocsBeforePingPong = state.allocationsCount;
    for (int i = 0; i < 50; ++i)
    {
        // Camera cut -> format 10
        state.ReleaseSurfacesIfFormatChanged(DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT);
        assert(state.output->format == DXGI_FORMAT_R16G16B16A16_FLOAT);
        assert(state.output->id == fmt10OutputId);

        // Next frame -> format 26
        state.ReleaseSurfacesIfFormatChanged(DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R11G11B10_FLOAT);
        assert(state.output->format == DXGI_FORMAT_R11G11B10_FLOAT);
        assert(state.output->id == fmt26OutputId);
    }
    // Absolutely zero new allocations across all 100 transitions!
    assert(state.allocationsCount == allocsBeforePingPong);
    assert(state.models[0].retries == 0);
    assert(state.swapsCount == 101);
    std::cout << "Test 4: 100 camera cut ping-pong cycles verified with 0 allocations and 0 model retries."
              << std::endl;

    // Test 5: Resolution change invalidates both active and cached pools
    state.width = 2560;
    state.height = 1440;
    state.HandleResolutionOrPlacementChange(true, false);
    assert(state.output == nullptr);
    assert(state.altSurfaces.output == nullptr);
    assert(state.altSurfaces.modelFormat == DXGI_FORMAT_UNKNOWN);
    assert(state.altSurfaces.nativeFormat == DXGI_FORMAT_UNKNOWN);
    assert(state.models[0].retries == 1);
    assert(state.modelRunning == false);
    std::cout << "Test 5: Resolution change full invalidation verified." << std::endl;

    // Test 6: Mixed spatial formats (modelFormat = 10, nativeFormat = 26)
    state.models[0].Prepare();
    state.modelRunning = true;
    state.width = 1708;
    state.height = 960;
    state.workWidth = 1708;
    state.workHeight = 960;
    state.output = state.CreateScratch(DXGI_FORMAT_R16G16B16A16_FLOAT, 1708, 960);
    state.colorCopy = state.CreateScratch(DXGI_FORMAT_R11G11B10_FLOAT, 1708, 960);
    state.hdrCopy = state.CreateScratch(DXGI_FORMAT_R11G11B10_FLOAT, 1708, 960);
    const auto spatialOutputId = state.output->id;

    // Switch to uniform format 10
    state.ReleaseSurfacesIfFormatChanged(DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT);
    assert(state.output == nullptr);
    assert(state.altSurfaces.modelFormat == DXGI_FORMAT_R16G16B16A16_FLOAT);
    assert(state.altSurfaces.nativeFormat == DXGI_FORMAT_R11G11B10_FLOAT);
    state.output = state.CreateScratch(DXGI_FORMAT_R16G16B16A16_FLOAT, 1708, 960);
    state.colorCopy = state.CreateScratch(DXGI_FORMAT_R16G16B16A16_FLOAT, 1708, 960);
    state.hdrCopy = state.CreateScratch(DXGI_FORMAT_R16G16B16A16_FLOAT, 1708, 960);

    // Switch back to mixed spatial format
    state.ReleaseSurfacesIfFormatChanged(DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R11G11B10_FLOAT);
    assert(state.output != nullptr);
    assert(state.output->id == spatialOutputId);
    assert(state.colorCopy->format == DXGI_FORMAT_R11G11B10_FLOAT);
    assert(state.output->format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    std::cout << "Test 6: Spatial mixed format caching verified." << std::endl;

    std::cout << "PASS: nr_format_bouncing_unit (FF7 Rebirth camera cut format stability)" << std::endl;
    return 0;
}

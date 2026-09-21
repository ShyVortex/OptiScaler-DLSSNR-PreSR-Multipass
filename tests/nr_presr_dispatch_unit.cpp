#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

// DXGI format declarations
enum DXGI_FORMAT : uint32_t
{
    DXGI_FORMAT_UNKNOWN = 0,
    DXGI_FORMAT_R32G32B32A32_TYPELESS = 1,
    DXGI_FORMAT_R32G32B32A32_FLOAT = 2,
    DXGI_FORMAT_R16G16B16A16_TYPELESS = 9,
    DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
    DXGI_FORMAT_R16G16B16A16_UNORM = 11,
    DXGI_FORMAT_R32G32_TYPELESS = 15,
    DXGI_FORMAT_R32G32_FLOAT = 16,
    DXGI_FORMAT_R10G10B10A2_TYPELESS = 23,
    DXGI_FORMAT_R10G10B10A2_UNORM = 24,
    DXGI_FORMAT_R8G8B8A8_TYPELESS = 27,
    DXGI_FORMAT_R8G8B8A8_UNORM = 28,
    DXGI_FORMAT_R16G16_TYPELESS = 33,
    DXGI_FORMAT_R16G16_FLOAT = 34,
    DXGI_FORMAT_R32_TYPELESS = 39,
    DXGI_FORMAT_R32_FLOAT = 40,
    DXGI_FORMAT_R24G8_TYPELESS = 44,
    DXGI_FORMAT_R24_UNORM_X8_TYPELESS = 46,
    DXGI_FORMAT_R16_TYPELESS = 53,
    DXGI_FORMAT_R16_UNORM = 56,
    DXGI_FORMAT_R32G8X24_TYPELESS = 19,
    DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS = 20,
};

// Simulation of DlssNr_Dx12::State::TypedGuideFormat
DXGI_FORMAT TypedGuideFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default:
        return f;
    }
}

struct ResourceDesc
{
    uint32_t Width = 0;
    uint32_t Height = 0;
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
};

// Mock ID3D12Resource
struct MockResource
{
    ResourceDesc desc;
    const ResourceDesc& GetDesc() const { return desc; }
};

// Mock DlssNr status publisher
struct MockNrState
{
    bool failed = false;
    std::string reason;
    bool modelRunning = false;
    uint64_t successfulDispatches = 0;
    bool exposureOfferedNow = false;
    bool exposureEverOffered = false;
    uint64_t exposureFrames = 0;
    float gameExposure = 0.0f;
    float gamePreExposure = 1.0f;

    void ReportPipelineSkip(const char* r)
    {
        if (reason.empty() && r)
            reason = r;
    }

    std::string PublishedReason() const { return reason; }
};

// Simulation of DlssNr_Dx12::Dispatch check
bool TestDlssNrDispatch(MockResource* colour, MockResource* output,
                        MockResource* depth, MockResource* motion,
                        void* exposureTexture, MockNrState& state)
{
    if (!colour || !depth || !motion || !output)
        return false;

    if (colour != output)
    {
        const auto source = colour->GetDesc(), target = output->GetDesc();
        if (source.Width != target.Width || source.Height != target.Height ||
            TypedGuideFormat(source.Format) != TypedGuideFormat(target.Format))
        {
            state.ReportPipelineSkip("dimension or format mismatch between pre-SR colour and intermediate buffer");
            return false;
        }
    }

    state.exposureOfferedNow = exposureTexture != nullptr;
    state.exposureEverOffered |= state.exposureOfferedNow;
    ++state.exposureFrames;

    // Simulate successful execution
    ++state.successfulDispatches;
    return true;
}

// Simulation of RestoreInputs in MakeDlssNrPass
struct TestRestoreInputs
{
    std::vector<void*> trackedResources;

    void Read(void* resource)
    {
        if (resource == nullptr)
            return;
        if (std::find(trackedResources.begin(), trackedResources.end(), resource) != trackedResources.end())
            return;
        trackedResources.push_back(resource);
    }
};

// Simulation of DispatchPass SRV setup
struct TestSrvTable
{
    void* srvs[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };

    void Build(void* source, void* model, void* original, void* motion, void* prevEdit)
    {
        srvs[0] = source;
        srvs[1] = model != nullptr ? model : source;
        srvs[2] = original != nullptr ? original : source;
        srvs[3] = motion != nullptr ? motion : source;
        srvs[4] = prevEdit != nullptr ? prevEdit : source;
    }
};

// Simulation of ResolveWhitePoint with null exposure texture
float TestResolveWhitePoint(int whitePointSource, float paperWhite,
                            void* exposureTexture, float gamePreExposure, float gameExposure)
{
    // Mode 1: Game exposure texture
    if (whitePointSource == 1 && exposureTexture != nullptr && gameExposure > 1e-6f)
    {
        return std::clamp(gamePreExposure / gameExposure, 0.01f, 4096.0f);
    }
    // Fallback to paper white slider
    return paperWhite > 0.0f ? paperWhite : 200.0f;
}

// Simulation of PrepareDlssNrInput failure handling
bool TestPrepareDlssNrInput(bool pipelineDispatchSuccess, MockNrState& state)
{
    if (pipelineDispatchSuccess)
        return true;

    state.ReportPipelineSkip("pre-SR input dispatch failed in pipeline");
    return false;
}

int main()
{
    std::cout << "=== Running DLSS-NR Pre-SR Dispatch & Null Exposure Unit Tests ===" << std::endl;

    // Test 1: Typeless source matches normalized typed target in Dispatch
    {
        MockNrState state;
        MockResource colorTypeless { { 2560, 1440, DXGI_FORMAT_R16G16B16A16_TYPELESS } };
        MockResource bufferTyped { { 2560, 1440, DXGI_FORMAT_R16G16B16A16_FLOAT } };
        MockResource depth { { 2560, 1440, DXGI_FORMAT_R32_FLOAT } };
        MockResource motion { { 2560, 1440, DXGI_FORMAT_R16G16_FLOAT } };

        // Even with exposureTexture == nullptr, typeless source should match typed buffer
        bool ok = TestDlssNrDispatch(&colorTypeless, &bufferTyped, &depth, &motion, nullptr, state);
        assert(ok);
        assert(state.successfulDispatches == 1);
        assert(!state.exposureOfferedNow);
        assert(state.reason.empty());
        std::cout << "[PASS] Test 1: Typeless R16G16B16A16 source matches typed FLOAT buffer with null exposure" << std::endl;
    }

    // Test 2: Other typeless formats (R32G32B32A32, R8G8B8A8, R10G10B10A2) match typed buffers
    {
        MockNrState state;
        MockResource depth { { 3840, 2160, DXGI_FORMAT_R32_FLOAT } };
        MockResource motion { { 3840, 2160, DXGI_FORMAT_R16G16_FLOAT } };

        // R32G32B32A32
        MockResource c32 { { 3840, 2160, DXGI_FORMAT_R32G32B32A32_TYPELESS } };
        MockResource b32 { { 3840, 2160, DXGI_FORMAT_R32G32B32A32_FLOAT } };
        assert(TestDlssNrDispatch(&c32, &b32, &depth, &motion, nullptr, state));

        // R8G8B8A8
        MockResource c8 { { 1920, 1080, DXGI_FORMAT_R8G8B8A8_TYPELESS } };
        MockResource b8 { { 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM } };
        assert(TestDlssNrDispatch(&c8, &b8, &depth, &motion, nullptr, state));

        // R10G10B10A2
        MockResource c10 { { 1920, 1080, DXGI_FORMAT_R10G10B10A2_TYPELESS } };
        MockResource b10 { { 1920, 1080, DXGI_FORMAT_R10G10B10A2_UNORM } };
        assert(TestDlssNrDispatch(&c10, &b10, &depth, &motion, nullptr, state));

        assert(state.successfulDispatches == 3);
        std::cout << "[PASS] Test 2: R32G32B32A32, R8G8B8A8, and R10G10B10A2 typeless normalization succeeds" << std::endl;
    }

    // Test 3: Genuine format mismatch is rejected and reports skip reason
    {
        MockNrState state;
        MockResource colorR8 { { 2560, 1440, DXGI_FORMAT_R8G8B8A8_UNORM } };
        MockResource bufferR16 { { 2560, 1440, DXGI_FORMAT_R16G16B16A16_FLOAT } };
        MockResource depth { { 2560, 1440, DXGI_FORMAT_R32_FLOAT } };
        MockResource motion { { 2560, 1440, DXGI_FORMAT_R16G16_FLOAT } };

        bool ok = TestDlssNrDispatch(&colorR8, &bufferR16, &depth, &motion, nullptr, state);
        assert(!ok);
        assert(state.successfulDispatches == 0);
        assert(state.PublishedReason() == "dimension or format mismatch between pre-SR colour and intermediate buffer");
        std::cout << "[PASS] Test 3: Incompatible format mismatch rejected with published skip reason" << std::endl;
    }

    // Test 4: Dimension mismatch is rejected and reports skip reason
    {
        MockNrState state;
        MockResource colorSmall { { 1920, 1080, DXGI_FORMAT_R16G16B16A16_FLOAT } };
        MockResource bufferLarge { { 2560, 1440, DXGI_FORMAT_R16G16B16A16_FLOAT } };
        MockResource depth { { 2560, 1440, DXGI_FORMAT_R32_FLOAT } };
        MockResource motion { { 2560, 1440, DXGI_FORMAT_R16G16_FLOAT } };

        bool ok = TestDlssNrDispatch(&colorSmall, &bufferLarge, &depth, &motion, nullptr, state);
        assert(!ok);
        assert(state.successfulDispatches == 0);
        assert(state.PublishedReason() == "dimension or format mismatch between pre-SR colour and intermediate buffer");
        std::cout << "[PASS] Test 4: Dimension mismatch rejected with published skip reason" << std::endl;
    }

    // Test 5: RestoreInputs gracefully skips null exposure resource
    {
        TestRestoreInputs restore;
        int dummyDepth = 1;
        int dummyMotion = 2;
        void* dummyExposure = nullptr;

        restore.Read(&dummyDepth);
        restore.Read(&dummyMotion);
        restore.Read(dummyExposure);

        assert(restore.trackedResources.size() == 2);
        assert(restore.trackedResources[0] == &dummyDepth);
        assert(restore.trackedResources[1] == &dummyMotion);
        std::cout << "[PASS] Test 5: RestoreInputs ignores null exposure resource without registering barrier" << std::endl;
    }

    // Test 6: DispatchPass srvs descriptor table substitutes InSource when prevEdit/exposureTex is null
    {
        TestSrvTable srvTable;
        int dummySource = 1;
        int dummyModel = 2;
        int dummyOriginal = 3;
        int dummyMotion = 4;
        void* dummyPrevEdit = nullptr; // exposureTex is null

        srvTable.Build(&dummySource, &dummyModel, &dummyOriginal, &dummyMotion, dummyPrevEdit);
        assert(srvTable.srvs[0] == &dummySource);
        assert(srvTable.srvs[1] == &dummyModel);
        assert(srvTable.srvs[2] == &dummyOriginal);
        assert(srvTable.srvs[3] == &dummyMotion);
        assert(srvTable.srvs[4] == &dummySource); // Falls back to InSource
        std::cout << "[PASS] Test 6: SrvTable substitutes InSource for slot 4 when exposure texture is null" << std::endl;
    }

    // Test 7: White point fallback when game exposure texture is null
    {
        const float defaultPaperWhite = 200.0f;

        // User selected Mode 1 (Game Exposure), but game has no exposure texture
        float wpMode1NoExposure = TestResolveWhitePoint(1, defaultPaperWhite, nullptr, 1.0f, 0.0f);
        assert(std::abs(wpMode1NoExposure - 200.0f) < 1e-4f);

        // User selected Mode 0 (Manual Paper White)
        float wpMode0 = TestResolveWhitePoint(0, 350.0f, nullptr, 1.0f, 0.0f);
        assert(std::abs(wpMode0 - 350.0f) < 1e-4f);

        // Game with valid exposure texture in Mode 1
        int validExposure = 1;
        float wpWithExposure = TestResolveWhitePoint(1, defaultPaperWhite, &validExposure, 1.0f, 0.01f);
        assert(std::abs(wpWithExposure - 100.0f) < 1e-4f);

        std::cout << "[PASS] Test 7: White point safely falls back to paper white when exposure texture is null" << std::endl;
    }

    // Test 8: PrepareDlssNrInput reports pipeline skip reason on dispatch failure
    {
        MockNrState state;
        bool ok = TestPrepareDlssNrInput(false, state);
        assert(!ok);
        assert(state.PublishedReason() == "pre-SR input dispatch failed in pipeline");
        std::cout << "[PASS] Test 8: PrepareDlssNrInput publishes non-empty skip reason on pipeline dispatch failure" << std::endl;
    }

    std::cout << "\nALL 8 UNIT TESTS PASSED SUCCESSFULLY!" << std::endl;
    return 0;
}

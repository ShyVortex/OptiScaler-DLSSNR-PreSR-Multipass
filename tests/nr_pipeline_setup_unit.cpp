#include <cassert>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>

// Mock NVSDK parameter table for subrect tests
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

static const char* NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X = "DLSS.Output.Subrect.Base.X";
static const char* NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y = "DLSS.Output.Subrect.Base.Y";
static const char* NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X = "DLSS.Input.Color.Subrect.Base.X";
static const char* NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y = "DLSS.Input.Color.Subrect.Base.Y";

bool TestHasSupportedNrSubrects(const MockNgxParameters& parameters, bool beforeUpscale)
{
    const char* offsets[] { NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X,
                            NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y };
    for (const auto* name : offsets)
    {
        unsigned int offset = 0;
        if (parameters.Get(name, &offset) == 1 && offset != 0)
            return false;
    }
    if (beforeUpscale)
    {
        unsigned int x = 0, y = 0;
        parameters.Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, &x);
        parameters.Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, &y);
        if (x != 0 || y != 0)
            return false;
    }
    return true;
}

// Mock DlssNr status publisher
struct MockNrState
{
    bool failed = false;
    const char* reason = "";
    bool modelRunning = false;

    void ReportPipelineSkip(const char* r)
    {
        if ((reason == nullptr || reason[0] == '\0') && r)
            reason = r;
    }

    std::string PublishedReason() const
    {
        return (reason != nullptr && reason[0] != '\0') ? reason : "";
    }
};

// Simulation of MakeDlssNrPass Setup logic
void* TestDlssNrPassSetup(bool enabled, bool shaderInit, bool supportedSubrects, void* depth, void* motion,
                          void* nextOutput, bool beforeUpscale, void* color, bool bufferAllocSuccess,
                          void* allocatedBuffer, MockNrState& state)
{
    if (!enabled)
        return nullptr;
    if (!shaderInit)
    {
        state.ReportPipelineSkip("the Neural Rendering shader is not initialized");
        return nullptr;
    }
    if (!supportedSubrects)
    {
        state.ReportPipelineSkip("subrect offsets are unsupported by DLSS-NR");
        return nullptr;
    }
    if (depth == nullptr)
    {
        state.ReportPipelineSkip("depth resource is missing");
        return nullptr;
    }
    if (motion == nullptr)
    {
        state.ReportPipelineSkip("motion-vector resource is missing");
        return nullptr;
    }
    if (nextOutput == nullptr)
    {
        state.ReportPipelineSkip("output target resource is null");
        return nullptr;
    }
    if (beforeUpscale)
        return color;
    if (!bufferAllocSuccess)
    {
        state.ReportPipelineSkip("the post-upscale intermediate buffer could not be allocated");
        return nullptr;
    }
    return allocatedBuffer;
}

int main()
{
    // 1. Test subrect validation
    {
        MockNgxParameters p;
        assert(TestHasSupportedNrSubrects(p, true));
        assert(TestHasSupportedNrSubrects(p, false));

        // Output offset non-zero -> invalid for both pre and post
        p.Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 16);
        assert(!TestHasSupportedNrSubrects(p, true));
        assert(!TestHasSupportedNrSubrects(p, false));
        p.Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0);

        // Input color offset non-zero -> invalid for pre, but valid for post
        p.Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, 8);
        assert(!TestHasSupportedNrSubrects(p, true));
        assert(TestHasSupportedNrSubrects(p, false));
    }

    // 2. Test Setup failure reason propagation
    void* fakeColor = (void*) 0x10;
    void* fakeDepth = (void*) 0x20;
    void* fakeMotion = (void*) 0x30;
    void* fakeOutput = (void*) 0x40;
    void* fakeAllocated = (void*) 0x50;

    // Missing depth
    {
        MockNrState state;
        void* res = TestDlssNrPassSetup(true, true, true, nullptr, fakeMotion, fakeOutput, false, fakeColor, true,
                                        fakeAllocated, state);
        assert(res == nullptr);
        assert(state.PublishedReason() == "depth resource is missing");
    }

    // Missing motion
    {
        MockNrState state;
        void* res = TestDlssNrPassSetup(true, true, true, fakeDepth, nullptr, fakeOutput, false, fakeColor, true,
                                        fakeAllocated, state);
        assert(res == nullptr);
        assert(state.PublishedReason() == "motion-vector resource is missing");
    }

    // Uninitialized shader
    {
        MockNrState state;
        void* res = TestDlssNrPassSetup(true, false, true, fakeDepth, fakeMotion, fakeOutput, false, fakeColor, true,
                                        fakeAllocated, state);
        assert(res == nullptr);
        assert(state.PublishedReason() == "the Neural Rendering shader is not initialized");
    }

    // Post-upscale buffer allocation failure
    {
        MockNrState state;
        void* res = TestDlssNrPassSetup(true, true, true, fakeDepth, fakeMotion, fakeOutput, false, fakeColor, false,
                                        fakeAllocated, state);
        assert(res == nullptr);
        assert(state.PublishedReason() == "the post-upscale intermediate buffer could not be allocated");
    }

    // Successful post-upscale setup
    {
        MockNrState state;
        void* res = TestDlssNrPassSetup(true, true, true, fakeDepth, fakeMotion, fakeOutput, false, fakeColor, true,
                                        fakeAllocated, state);
        assert(res == fakeAllocated);
        assert(state.PublishedReason().empty());
    }

    // Successful pre-upscale setup
    {
        MockNrState state;
        void* res = TestDlssNrPassSetup(true, true, true, fakeDepth, fakeMotion, fakeOutput, true, fakeColor, true,
                                        fakeAllocated, state);
        assert(res == fakeColor);
        assert(state.PublishedReason().empty());
    }

    std::cout << "All NR pipeline setup and diagnostic unit tests passed successfully!\n";
    return 0;
}

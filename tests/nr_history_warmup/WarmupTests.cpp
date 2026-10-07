#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>
struct ID3D12GraphicsCommandList
{
};
struct ID3D12Resource
{
};
struct NVSDK_NGX_Parameter
{
};
struct FrameInfo
{
    bool BeforeUpscale = false, RayReconstruction = false;
    unsigned epoch = 0;
};
template <class T> struct Option
{
    T value {};
    T value_or_default() const { return value; }
};
struct Config
{
    Option<bool> DlssNrEnabled { true }, DlssNrDenoiseFirst { true }, DlssNrRunBeforeSr { false };
    static Config* Instance()
    {
        static Config c;
        return &c;
    }
};
namespace DlssNr
{
bool CanRunBeforeUpscale_Dx12(NVSDK_NGX_Parameter*) { return true; }
} // namespace DlssNr
bool TuningMatchesFeature(const Config&, unsigned) { return true; }
struct ModelBoundary
{
    bool created = false;
    unsigned createEpoch = 0, evaluations = 0, retirements = 0;
    bool HasFeature() const { return created; }
    void RetryAfterFailure()
    {
        created = false;
        ++retirements;
    }
    bool Run(unsigned epoch)
    {
        if (!created)
        {
            created = true;
            createEpoch = epoch;
            return false;
        }
        if (epoch == createEpoch)
            return false;
        ++evaluations;
        return true;
    }
};
struct State
{
    struct Surfaces
    {
        ID3D12Resource* output = nullptr;
        ID3D12Resource* passScratch = nullptr;
        ID3D12Resource* passClamp = nullptr;
        ID3D12Resource* colorCopy = nullptr;
        ID3D12Resource* hdrCopy = nullptr;
        ID3D12Resource* colorSmall = nullptr;
        ID3D12Resource* outputNative = nullptr;
        ID3D12Resource* activeColor = nullptr;
    };
    struct Nr : Surfaces
    {
        unsigned width = 0, height = 0, workWidth = 0, workHeight = 0, successfulDispatches = 0;
        bool beforeUpscale = false, rayReconstruction = false, reset = false, passScratchFailed = false;
        ModelBoundary models[1];
        bool passCreateFailed[1] {};
        Surfaces altSurfaces;
    } nr;
    bool modelRunning = false;
    bool sameDimensions = true;
    void ParkNrResource(ID3D12Resource*& resource) { resource = nullptr; }
    ID3D12Resource* CreateScratch(void*, int, unsigned, unsigned)
    {
        static ID3D12Resource marker;
        return &marker;
    }
    void Run(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*,
             const FrameInfo& frame, void*, bool* evaluatedModel = nullptr)
    {
        const auto& cfg = *Config::Instance();
        void* device = nullptr;
        const unsigned requestedPasses = 1;
        const unsigned width = frame.BeforeUpscale || sameDimensions ? 960 : 1920;
        const unsigned height = frame.BeforeUpscale || sameDimensions ? 540 : 1080;
        const unsigned workWidth = width, workHeight = height;
        const int modelFormat = 1;
        struct
        {
            unsigned Width, Height;
            int Format;
        } desc { width, height, modelFormat };
#include "production-model-preparation.inc"
        bool evaluated = nr.models[0].Run(frame.epoch);
        if (evaluatedModel)
            *evaluatedModel = evaluated;
        if (evaluated)
            ++nr.successfulDispatches;
    }
};
struct Handoff
{
    ID3D12Resource* color = nullptr;
    bool replaceOutput = false, modelEvaluated = false, modelAttempted = false, ownsNrHistory = false;
};
Handoff PrivateBefore(State& owner, unsigned epoch)
{
    Handoff handoff;
    auto& nr = owner.nr;
    (void) nr;
    struct
    {
        ID3D12Resource* edited = nullptr;
        bool reset = false, failed = false;
    } g;
    ID3D12GraphicsCommandList command;
    auto* cmd = &command;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    FrameInfo frame { true, false, epoch };
    void* queue = nullptr;
    const auto before = owner.nr.successfulDispatches;
    auto Say = [](const std::string&) {};
#include "production-private-invocation.inc"
#include "production-nr-success.inc"
    static ID3D12Resource edited;
    handoff.color = &edited;
    return handoff;
}
int main()
{
    unsigned failures = 0;
    for (bool sameDimensions : { true, false })
    {
        State owner;
        owner.sameDimensions = sameDimensions;
        NVSDK_NGX_Parameter parameters;
        auto* InParameters = &parameters;
        void* NeuralRendering = &owner;
        bool specializedNr = false;
        [[maybe_unused]] bool interop = false;
        [[maybe_unused]] bool useOutputScaling = false;
        unsigned firstReady = 0;
        for (unsigned epoch = 1; epoch <= 4; ++epoch)
        {
            auto denoiseHandoff = PrivateBefore(owner, epoch);
#include "production-routing.inc"
            std::printf("TRACE %s frame %u successful %u own %d before %d after %d\n",
                        sameDimensions ? "same dimensions" : "different dimensions", epoch,
                        owner.nr.successfulDispatches, denoiseFirst, nrBeforeUpscale, nrAfterUpscale);
            if (denoiseHandoff.color && !firstReady)
                firstReady = epoch;
            if (nrBeforeUpscale || nrAfterUpscale)
            {
                // Different display extent strengthens the retry trigger; identical
                // dimensions must still expose the actual placementChanged decision.
                FrameInfo fallback { false, false, epoch };
                owner.Run(nullptr, nullptr, nullptr, nullptr, nullptr, fallback, nullptr);
            }
        }
        const bool ready = firstReady == 2 && owner.nr.models[0].evaluations == 3;
        std::printf("%s private model warmup completes on second epoch with %s\n", ready ? "PASS" : "FAIL",
                    sameDimensions ? "same dimensions" : "different dimensions");
        if (!ready)
            ++failures;
    }
    std::printf("%u assertion failure(s)\n", failures);
    return failures ? 1 : 0;
}

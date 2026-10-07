// Execute extracted production methods. Stand-ins model only command/resource
// boundaries and deferred GPU completion; no D3D device or game is invoked.
#include <cstdio>
#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <stdexcept>

enum D3D12_RESOURCE_STATES
{
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
    D3D12_RESOURCE_STATE_COPY_SOURCE,
    D3D12_RESOURCE_STATE_COPY_DEST
};
struct ID3D12GraphicsCommandList
{
};
struct NVSDK_NGX_Parameter
{
};
struct DlssNrFrameInfo
{
    unsigned long long SubmissionEpoch = 1;
};
constexpr int NVSDK_NGX_Result_Success = 0;
struct ResourceDesc
{
    unsigned Width = 1920, Height = 1080;
    int Format = 1;
};
struct ID3D12Resource
{
    ResourceDesc desc;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    int copies = 0;
    ResourceDesc GetDesc() const { return desc; }
};
template <class T> struct Option : std::optional<T>
{
    using std::optional<T>::operator=;
    T value_or_default() const { return this->value_or(T {}); }
};
struct Config
{
    Option<bool> RestoreComputeSignature, RestoreGraphicSignature;
    Option<int> OutputResourceBarrier;
    Option<bool> DlssNrEnabled, DlssNrRunBeforeSr, DlssNrDenoiseFirst;
    static Config* Instance()
    {
        static Config c;
        return &c;
    }
};
namespace D3D12Hooks
// The model is the GPU boundary; the caller's flag propagation remains production code.
{
static bool restorable = true;
bool CanRestoreRootSignature(ID3D12GraphicsCommandList*) { return restorable; }
} // namespace D3D12Hooks
namespace DlssNr
{
static bool inputSupported = true;
bool CanRunBeforeUpscale_Dx12(NVSDK_NGX_Parameter*) { return inputSupported; }
struct Extent
{
    unsigned width, height;
};
void CopyActiveColor(ID3D12GraphicsCommandList*, ID3D12Resource* out, ID3D12Resource* in, Extent e)
{
    if (!in || !out || in->state != D3D12_RESOURCE_STATE_COPY_SOURCE || out->state != D3D12_RESOURCE_STATE_COPY_DEST ||
        e.width != 1920 || e.height != 1080)
        throw std::runtime_error("invalid copy boundary");
    ++out->copies;
}
} // namespace DlssNr
struct BoundaryModelFrame
{
    ID3D12Resource* color = nullptr;
    ID3D12Resource* output = nullptr;
};
int PassSettings(const Config&, unsigned) { return 0; }
struct ModelBoundary
{
    inline static unsigned evaluations = 0;
    int Run(ID3D12GraphicsCommandList*, void*, BoundaryModelFrame&, int, unsigned long long, bool* evaluated)
    {
        *evaluated = true;
        ++evaluations;
        return NVSDK_NGX_Result_Success;
    }
};
// Completion is explicitly advanced by tests. Collect cannot prematurely destroy
// a generation whose command list is still in flight.
struct Lifetime
{
    bool complete = false;
    std::vector<std::function<void()>> retired;
    ~Lifetime()
    {
        for (auto& f : retired)
            f();
    }
    void Record(ID3D12GraphicsCommandList*) {}
    void Retire(std::function<void()> f) { retired.push_back(std::move(f)); }
    void BeginGeneration() {}
    void Collect()
    {
        if (complete)
        {
            for (auto& f : retired)
                f();
            retired.clear();
        }
    }
};
struct DlssNr_Dx12
{
    struct State
    {
        struct Generation
        {
            struct EnlargerBoundary
            {
                std::string Error() const { return "injected GPU evaluation failure"; }
            };
            bool reset = false, failed = false;
            unsigned outW = 1920, outH = 1080;
            int outputFormat = 1;
            ID3D12Resource* upscaled = nullptr;
            ID3D12Resource* composite = nullptr;
            ID3D12Resource* edited = nullptr;
            std::unique_ptr<EnlargerBoundary> enlarger = std::make_unique<EnlargerBoundary>();
        };
        struct Pending
        {
            ID3D12GraphicsCommandList* cmd = nullptr;
            NVSDK_NGX_Parameter* caller = nullptr;
            ID3D12Resource* handedOff = nullptr;
            D3D12_RESOURCE_STATES handedState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            bool replaceOutput = false;
        };
        struct DenoiseFirstContext
        {
            State& owner;
            std::unique_ptr<Generation> current;
            Pending pending;
            unsigned retiredCount = 0;
            std::vector<Generation*> retiredGenerations;
            Lifetime lifetime;
            std::string status;
            explicit DenoiseFirstContext(State& s) : owner(s) {}
            void Say(const std::string& s) { status = s; }
            void After(ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*, ID3D12Resource*, bool, D3D12_RESOURCE_STATES);
            void Cancel();
            void RetireCurrent();
            void ReleaseResources();
            void AbandonPendingPrefix()
            {
#include "production-abandoned-pending.inc"
            }
        } denoiseFirst;
        struct DeferredSrContext
        {
            std::unique_ptr<Generation> current;
            Pending pending;
            unsigned retiredCount = 0;
            std::vector<Generation*> retiredGenerations;
            Lifetime lifetime;
            void Cancel();
            void RetireCurrent();
            void ReleaseResources();
        } deferredSr;
        struct Nr
        {
            std::vector<ModelBoundary> models { ModelBoundary {} };
            unsigned successfulDispatches = 0;
            bool failed = false, reset = false, spatialFallback = false, spatialActive = false;
            const char* reason = "";
            std::string spatialFallbackReason;
        } nr;
        Lifetime lifetime;
        std::unique_ptr<Generation> enlarger;
        std::vector<std::unique_ptr<Generation>> retiredEnlargers;
        std::string enlargementStatus;
        State() : denoiseFirst(*this) {}
        void ReleaseEnlarger()
        {
            if (enlarger)
                retiredEnlargers.push_back(std::move(enlarger));
        }
        void RetryAfterFailure();
        void Run(ID3D12GraphicsCommandList* cmdList, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*,
                 const DlssNrFrameInfo& frame, void*, bool* evaluatedModel = nullptr)
        {
            auto& cfg = *Config::Instance();
            void* device = nullptr;
            BoundaryModelFrame modelFrame;
            ID3D12Resource* passInput = nullptr;
            ID3D12Resource* passOutput = nullptr;
            unsigned pass = 0;
            int result = NVSDK_NGX_Result_Success;
            [[maybe_unused]] bool modelRunning = false;
            [[maybe_unused]] auto* modelConsumption = evaluatedModel;
            do
            {
#include "production-model-evaluated.inc"
            } while (false);
            // Inject failed final NR composition: successfulDispatches remains unchanged
            // although the actual model evaluation above consumed its history.
        }
        void Barrier(ID3D12GraphicsCommandList*, ID3D12Resource* r, D3D12_RESOURCE_STATES from,
                     D3D12_RESOURCE_STATES to)
        {
            if (!r || r->state != from)
                throw std::runtime_error("invalid resource transition");
            r->state = to;
        }
    };
};
#include "production-methods.inc"

struct TraceHandoff
{
    ID3D12Resource* color = nullptr;
    bool replaceOutput = false;
    bool modelEvaluated = false;
};
TraceHandoff LateFailureBefore(DlssNr_Dx12::State& owner, bool composition)
{
    TraceHandoff handoff;
    auto& g = *owner.denoiseFirst.current;
    auto Say = [&](const std::string& text) { owner.denoiseFirst.Say(text); };
    ID3D12GraphicsCommandList command;
    auto* cmd = &command;
#include "production-failed-gate.inc"
    const auto before = owner.nr.successfulDispatches;
    // The private NR GPU evaluation succeeds, advancing its persistent history.
    ++owner.nr.successfulDispatches;
#include "production-nr-success.inc"
    const bool ok = false; // fail only the subsequent GPU composition/enlargement
    if (composition)
    {
        // The composition dispatch boundary has transitioned its output to UAV.
        g.composite->state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
#include "production-composition-failure.inc"
    }
    else
    {
#include "production-enlarge-failure.inc"
    }
    return handoff;
}

static int failures = 0;
void Check(bool value, const char* label)
{
    std::printf("%s %s\n", value ? "PASS" : "FAIL", label);
    if (!value)
        ++failures;
}
void AfterCase(bool replace, bool success, bool mismatch)
{
    DlssNr_Dx12::State s;
    auto& ctx = s.denoiseFirst;
    ctx.current = std::make_unique<DlssNr_Dx12::State::Generation>();
    ID3D12GraphicsCommandList cmd, other;
    NVSDK_NGX_Parameter caller;
    ID3D12Resource handed, output, privateOutput;
    privateOutput.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ctx.current->upscaled = &privateOutput;
    auto* initialGeneration = ctx.current.get();
    ctx.pending = { &cmd, &caller, &handed, handed.state, replace };
    ctx.After(mismatch ? &other : &cmd, &caller, &output, success, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (mismatch)
    {
        Check(!ctx.current && ctx.retiredCount == 1 && ctx.retiredGenerations == std::vector { initialGeneration } &&
                  initialGeneration->reset,
              "mismatched pair retires generation until its GPU work completes");
        Check(handed.state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
              "mismatched command list never transitions a previously handed-off resource");
        ctx.lifetime.complete = true;
        ctx.lifetime.Collect();
        Check(ctx.retiredCount == 0 && ctx.retiredGenerations.empty(),
              "mismatched generation is reclaimed after GPU completion");
    }
    else
    {
        Check(handed.state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
              "matching pair restores handed-off resource state");
        if (!replace && !success)
            Check(ctx.current->reset, "failed default game upscale invalidates private history");
        else if (!replace)
            Check(!ctx.current->reset, "successful default game upscale preserves private history");
        else if (!success)
            Check(ctx.current->reset, "failed replacement upscale invalidates private history");
        else
        {
            Check(!ctx.current->reset && output.copies == 1, "successful replacement copies the private result");
            Check(output.state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS &&
                      privateOutput.state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                  "replacement copy restores both resource states");
        }
    }
    Check(!ctx.pending.cmd && !ctx.pending.caller && !ctx.pending.handedOff, "After consumes the pending handoff");
}
void IntermediateOutputCase()
{
    DlssNr_Dx12::State owner;
    auto& context = owner.denoiseFirst;
    context.current = std::make_unique<DlssNr_Dx12::State::Generation>();
    ID3D12GraphicsCommandList command;
    NVSDK_NGX_Parameter parameters;
    ID3D12Resource intermediate, privateOutput;
    privateOutput.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    context.current->upscaled = &privateOutput;
    context.pending.cmd = &command;
    context.pending.caller = &parameters;
    context.pending.replaceOutput = true;
    // Game's final output override does not describe the pipeline-owned UAV target.
    Config::Instance()->OutputResourceBarrier = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bool correctBoundary = true;
    try
    {
        context.After(&command, &parameters, &intermediate, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    catch (const std::runtime_error&)
    {
        correctBoundary = false;
    }
    Config::Instance()->OutputResourceBarrier.reset();
    Check(correctBoundary && intermediate.copies == 1 && intermediate.state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          "replacement uses actual intermediate output arrival state despite final-output override");
}
void RetryCase()
{
    using Generation = DlssNr_Dx12::State::Generation;
    DlssNr_Dx12::State s;
    s.nr.failed = s.nr.spatialFallback = s.nr.spatialActive = true;
    s.nr.reason = "failed";
    s.nr.spatialFallbackReason = "failed";
    s.enlargementStatus = "failed";
    s.enlarger = std::make_unique<Generation>();
    s.deferredSr.current = std::make_unique<Generation>();
    s.denoiseFirst.current = std::make_unique<Generation>();
    auto* deferred = s.deferredSr.current.get();
    auto* denoise = s.denoiseFirst.current.get();
    deferred->failed = denoise->failed = true;
    ID3D12GraphicsCommandList cmd;
    s.deferredSr.pending.cmd = s.denoiseFirst.pending.cmd = &cmd;
    s.RetryAfterFailure();
    Check(!s.nr.failed && s.nr.reset && !s.nr.spatialFallback && !s.nr.spatialActive &&
              std::string(s.nr.reason).empty() && s.nr.spatialFallbackReason.empty(),
          "retry resets ordinary NR failure and fallback state");
    Check(!s.enlarger && s.enlargementStatus.empty(), "retry releases the current enlarger");
    Check(!s.deferredSr.current && !s.deferredSr.pending.cmd && deferred->reset,
          "retry retires failed DeferredSr history and cancels its pending handoff");
    Check(!s.denoiseFirst.current && !s.denoiseFirst.pending.cmd && denoise->reset,
          "retry retires failed DenoiseFirst history and cancels its pending handoff");
    // Simulate the next Before's allocation gate: only absence of current permits
    // fresh allocation; a still-current failed generation remains disabled.
    if (!s.deferredSr.current)
        s.deferredSr.current = std::make_unique<Generation>();
    if (!s.denoiseFirst.current)
        s.denoiseFirst.current = std::make_unique<Generation>();
    Check(!s.deferredSr.current->failed, "DeferredSr can resume after explicit retry");
    Check(!s.denoiseFirst.current->failed, "DenoiseFirst can resume after explicit retry");
    s.deferredSr.lifetime.complete = s.denoiseFirst.lifetime.complete = true;
    s.deferredSr.lifetime.Collect();
    s.denoiseFirst.lifetime.Collect();
}
void RoutingCases()
{
    struct RouteCase
    {
        const char* name;
        bool shader, enabled, specialized, before, supported, interop, color, replacement;
        bool wantOwn, wantBefore, wantAfter;
    };
    const RouteCase cases[] { { "warmup empty handoff retains ordinary pre-SR", true, true, false, true, true, false,
                                false, false, false, true, false },
                              { "warmup empty handoff retains ordinary post-SR", true, true, false, false, true, false,
                                false, false, false, false, true },
                              { "unsupported private input retains ordinary post-SR", true, true, false, true, false,
                                false, false, false, false, false, true },
                              { "interop skips private handoff but retains ordinary NR", true, true, false, true, true,
                                true, false, false, false, true, false },
                              { "successful color handoff owns both seams", true, true, false, true, true, false, true,
                                false, true, false, false },
                              { "successful replacement handoff owns both seams", true, true, false, false, true, false,
                                false, true, true, false, false },
                              { "disabled NR schedules neither ordinary seam", true, false, false, true, true, false,
                                false, false, false, false, false },
                              { "specialized route retains priority", true, true, true, true, true, false, false, false,
                                false, false, false },
                              { "absent shader schedules neither seam", false, true, false, true, true, false, false,
                                false, false, false, false } };
    for (const auto& test : cases)
    {
        auto& cfg = *Config::Instance();
        cfg.DlssNrEnabled = test.enabled;
        cfg.DlssNrRunBeforeSr = test.before;
        cfg.DlssNrDenoiseFirst = true; // request alone must not own either seam
        DlssNr::inputSupported = test.supported;
        void* NeuralRendering = test.shader ? &cfg : nullptr;
        bool specializedNr = test.specialized;
        [[maybe_unused]] bool interop = test.interop;
        [[maybe_unused]] bool useOutputScaling = false;
        NVSDK_NGX_Parameter parameters;
        auto* InParameters = &parameters;
        ID3D12Resource handedColor;
        TraceHandoff denoiseHandoff { test.color ? &handedColor : nullptr, test.replacement };
#include "production-routing.inc"
        Check(denoiseFirst == test.wantOwn && nrBeforeUpscale == test.wantBefore && nrAfterUpscale == test.wantAfter,
              test.name);
    }
}
void LateFailureCases()
{
    for (bool composition : { true, false })
    {
        for (bool beforePlacement : { true, false })
        {
            DlssNr_Dx12::State owner;
            owner.denoiseFirst.current = std::make_unique<DlssNr_Dx12::State::Generation>();
            ID3D12Resource composite;
            owner.denoiseFirst.current->composite = &composite;
            auto& cfg = *Config::Instance();
            cfg.DlssNrEnabled = cfg.DlssNrDenoiseFirst = true;
            cfg.DlssNrRunBeforeSr = beforePlacement;
            DlssNr::inputSupported = true;
            void* NeuralRendering = &owner;
            bool specializedNr = false;
            [[maybe_unused]] bool interop = false;
            [[maybe_unused]] bool useOutputScaling = false;
            NVSDK_NGX_Parameter parameters;
            auto* InParameters = &parameters;
            {
                auto denoiseHandoff = LateFailureBefore(owner, composition);
#include "production-routing.inc"
                if (nrBeforeUpscale || nrAfterUpscale)
                    ++owner.nr.successfulDispatches;
                Check(owner.nr.successfulDispatches == 1 && denoiseFirst,
                      composition ? "composition failure does not evaluate model history twice in one epoch"
                                  : "private upscale failure does not evaluate model history twice in one epoch");
                if (composition)
                {
                    Check(owner.denoiseFirst.current->failed && owner.nr.reset,
                          "late composition failure parks private context and resets ordinary NR history");
                    Check(composite.state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          "failed composition restores its scratch resource");
                }
            }
            // On the next epoch a failed private context must exit before model evaluation,
            // leaving the ordinary route available again.
            const auto previousDispatches = owner.nr.successfulDispatches;
            {
                auto denoiseHandoff = LateFailureBefore(owner, composition);
#include "production-routing.inc"
                Check(owner.nr.successfulDispatches == previousDispatches && !denoiseFirst &&
                          (nrBeforeUpscale || nrAfterUpscale),
                      "subsequent failed-private frame resumes ordinary NR without a private evaluation");
            }
        }
    }
}
void AbandonedPendingCase()
{
    DlssNr_Dx12::State owner;
    auto& context = owner.denoiseFirst;
    context.current = std::make_unique<DlssNr_Dx12::State::Generation>();
    auto* generation = context.current.get();
    ID3D12GraphicsCommandList command;
    ID3D12Resource handed;
    context.pending.cmd = &command;
    context.pending.handedOff = &handed;
    context.AbandonPendingPrefix();
    Check(!context.current && context.retiredCount == 1 && context.retiredGenerations == std::vector { generation } &&
              generation->reset && !context.pending.cmd,
          "abandoned Before handoff retires generation instead of reusing an unknown resource state");
    Check(handed.state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          "abandoned Before never restores a resource on a new command list");
    context.lifetime.complete = true;
    context.lifetime.Collect();
    Check(context.retiredCount == 0 && context.retiredGenerations.empty(),
          "abandoned generation is reclaimed after completion");
}
TraceHandoff InternalCompositionFailureBefore(DlssNr_Dx12::State& owner)
{
    TraceHandoff handoff;
    auto& g = *owner.denoiseFirst.current;
    auto Say = [&](const std::string& text) { owner.denoiseFirst.Say(text); };
    ID3D12GraphicsCommandList command;
    auto* cmd = &command;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    DlssNrFrameInfo frame;
    void* queue = nullptr;
    const auto before = owner.nr.successfulDispatches;
#include "production-before-run-call.inc"
#include "production-nr-success.inc"
    return handoff;
}
void InternalCompositionFailureCases()
{
    DlssNr_Dx12::State propagationOwner;
    ID3D12GraphicsCommandList command;
    DlssNrFrameInfo frame;
    bool consumed = false;
    propagationOwner.Run(&command, nullptr, nullptr, nullptr, nullptr, frame, nullptr, &consumed);
    Check(consumed && propagationOwner.nr.successfulDispatches == 0,
          "actual Run reports consumed model history even when final composition fails");
    for (bool beforePlacement : { true, false })
    {
        DlssNr_Dx12::State owner;
        owner.denoiseFirst.current = std::make_unique<DlssNr_Dx12::State::Generation>();
        auto& cfg = *Config::Instance();
        cfg.DlssNrEnabled = cfg.DlssNrDenoiseFirst = true;
        cfg.DlssNrRunBeforeSr = beforePlacement;
        DlssNr::inputSupported = true;
        void* NeuralRendering = &owner;
        bool specializedNr = false;
        [[maybe_unused]] bool interop = false;
        [[maybe_unused]] bool useOutputScaling = false;
        NVSDK_NGX_Parameter parameters;
        auto* InParameters = &parameters;
        const auto previousEvaluations = ModelBoundary::evaluations;
        auto denoiseHandoff = InternalCompositionFailureBefore(owner);
#include "production-routing.inc"
        if (nrBeforeUpscale || nrAfterUpscale)
        {
            bool ordinaryConsumed = false;
            owner.Run(&command, nullptr, nullptr, nullptr, nullptr, frame, nullptr, &ordinaryConsumed);
        }
        Check(ModelBoundary::evaluations == previousEvaluations + 1 && denoiseFirst && denoiseHandoff.modelEvaluated &&
                  owner.nr.successfulDispatches == 0,
              "NR internal composition failure does not evaluate model history twice in one epoch");
    }
}
int main()
{
    try
    {
        AfterCase(false, false, false);
        AfterCase(false, true, false);
        AfterCase(true, false, false);
        AfterCase(true, true, false);
        AfterCase(false, true, true);
        RetryCase();
        RoutingCases();
        LateFailureCases();
        AbandonedPendingCase();
        InternalCompositionFailureCases();
        IntermediateOutputCase();
    }
    catch (const std::exception& e)
    {
        std::printf("ERROR boundary fixture: %s\n", e.what());
        return 2;
    }
    std::printf("%d assertion failure(s)\n", failures);
    return failures ? 1 : 0;
}

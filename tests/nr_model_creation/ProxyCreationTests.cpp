// Compile the complete production proxy and GPU lifetime tracker. Only NGX and
// external COM/fence operations are substituted; no GPU/runtime DLL is loaded.
#define main ExistingSubmissionAuditMain
#include "../nr_cpu_submission_audit/SubmissionResetTests.cpp"
#undef main
#define NR_PROXY_REAL_LIFETIME
#include "../dlssnr_proxy/MockNgx.h"
#ifdef NR_PROXY_BASELINE
#include "ProxyBaseline.cpp"
#else
#include "../../OptiScaler/dlssnr/DlssNr_Proxy.cpp"
#endif

namespace DlssNr::NgxDiagnostics
{
Scope::Scope() {}
Scope::~Scope() {}
void RuntimeReport(ID3D12GraphicsCommandList*, ID3D12Device*, const char*) {}
} // namespace DlssNr::NgxDiagnostics

namespace DlssNr
{
std::vector<std::filesystem::path> CompatibilityRuntime::CandidatePaths() { return {}; }
std::shared_ptr<CompatibilityRuntime> CompatibilityRuntime::TryOpen(ID3D12Device*) { return {}; }
std::shared_ptr<CompatibilityRuntime> CompatibilityRuntime::TryOpen(const std::filesystem::path&, ID3D12Device*)
{
    return {};
}
CompatibilityRuntime::~CompatibilityRuntime() = default;
NVSDK_NGX_Result CompatibilityRuntime::Create(ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**)
{
    return NVSDK_NGX_Result_Fail;
}
NVSDK_NGX_Result CompatibilityRuntime::Evaluate(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                                NVSDK_NGX_Parameter*)
{
    return NVSDK_NGX_Result_Fail;
}
NVSDK_NGX_Result CompatibilityRuntime::Release(NVSDK_NGX_Handle*) { return NVSDK_NGX_Result_Fail; }
} // namespace DlssNr

// D3D12 private data is keyed by GUID, and queues can signal several independent
// tracker fences. The old single-tracker fixture intentionally models only one.
struct ModelCommands final : CpuObject<ID3D12GraphicsCommandList>
{
    std::vector<std::pair<GUID, ComPtr<IUnknown>>> watches;
    HRESULT SetPrivateDataInterface(REFGUID key, const IUnknown* value) override
    {
        for (auto& [stored, watch] : watches)
            if (stored == key)
            {
                watch = const_cast<IUnknown*>(value);
                return S_OK;
            }
        watches.emplace_back(key, ComPtr<IUnknown>(const_cast<IUnknown*>(value)));
        return S_OK;
    }
};
struct ModelQueue final : CpuObject<ID3D12CommandQueue>
{
    ComPtr<ID3D12Device> device;
    std::vector<std::pair<ComPtr<ID3D12Fence>, UINT64>> signals;
    bool failSignal = false, removed = false;
    explicit ModelQueue(ID3D12Device* d) : device(d) {}
    HRESULT GetDevice(REFIID, void** out) override
    {
        *out = device.Get();
        device->AddRef();
        return S_OK;
    }
    HRESULT Signal(ID3D12Fence* fence, UINT64 value) override
    {
        if (failSignal)
            return E_FAIL;
        for (auto& [stored, target] : signals)
            if (stored.Get() == fence)
            {
                target = value;
                return S_OK;
            }
        signals.emplace_back(ComPtr<ID3D12Fence>(fence), value);
        return S_OK;
    }
    void Execute() {}
    void Complete()
    {
        for (auto& [fence, target] : signals)
            static_cast<CpuFence*>(fence.Get())->completed = removed ? UINT64_MAX : target;
    }
    void RemoveDevice()
    {
        removed = true;
        Complete();
    }
};

struct ProxyFixture
{
    ComPtr<ID3D12Device> device;
    ComPtr<ModelQueue> queue;
    ComPtr<ModelCommands> commands, other;
    DlssNr::Proxy::Context context;
    DlssNr::ModelSettings settings;
    ID3D12CommandList* lists[1];
    ProxyFixture()
    {
        device.Attach(new CpuDevice);
        queue.Attach(new ModelQueue(device.Get()));
        commands.Attach(new ModelCommands);
        other.Attach(new ModelCommands);
        lists[0] = commands.Get();
    }
    ~ProxyFixture()
    {
        queue->Complete();
        context.ResetRecording(commands.Get());
        context.ResetRecording(other.Get());
        context.Release();
        context.Collect();
    }
    bool Create(uint64_t epoch = 10, ID3D12GraphicsCommandList* cmd = nullptr)
    {
        bool ready = true;
        const auto result =
            context.Prepare(cmd ? cmd : commands.Get(), device.Get(), 1280, 720, settings, epoch, &ready);
        return result == NVSDK_NGX_Result_Success && !ready && context.HasFeature();
    }
    auto Submit(ID3D12CommandList* cmd = nullptr)
    {
        ID3D12CommandList* submitted[] { cmd ? cmd : commands.Get() };
        auto token = context.BeginSubmission(1, submitted);
        queue->Execute();
        return token;
    }
};

bool epochCannotSubmitCreation()
{
    ProxyFixture f;
    return f.Create() && !f.context.Ready(11) && !f.context.Ready(9999);
}
bool otherListCannotCompleteCreation()
{
    ProxyFixture f;
    if (!f.Create())
        return false;
    auto token = f.Submit(f.other.Get());
    token.Complete(f.queue.Get());
    f.queue->Complete();
    return !f.context.Ready(11);
}
bool completionDoesNotRequireEpochChange()
{
    ProxyFixture f;
    if (!f.Create())
        return false;
    auto token = f.Submit();
    token.Complete(f.queue.Get());
    if (f.context.Ready(10))
        return false;
    f.queue->Complete();
    return f.context.Ready(10) && f.context.Ready(10);
}
bool pendingFenceIsNotReady()
{
    ProxyFixture f;
    if (!f.Create())
        return false;
    auto token = f.Submit();
    token.Complete(f.queue.Get());
    return !f.context.Ready(11);
}
bool discardedCreationRebuildsOnce()
{
    ProxyFixture f;
    if (!f.Create())
        return false;
    const auto creations = Mock::creations;
    f.context.ResetRecording(f.commands.Get());
    if (!f.Create(11) || Mock::creations != creations + 1)
        return false;
    bool ready = true;
    f.context.Prepare(f.commands.Get(), f.device.Get(), 1280, 720, f.settings, 12, &ready);
    return !ready && Mock::creations == creations + 1;
}
bool submittedResetRetainsFeature()
{
    ProxyFixture f;
    if (!f.Create())
        return false;
    const auto creations = Mock::creations;
    auto token = f.Submit();
    f.context.ResetRecording(f.commands.Get());
    token.Complete(f.queue.Get());
    bool ready = true;
    f.context.Prepare(f.other.Get(), f.device.Get(), 1280, 720, f.settings, 11, &ready);
    if (ready || Mock::creations != creations)
        return false;
    f.queue->Complete();
    f.context.Prepare(f.other.Get(), f.device.Get(), 1280, 720, f.settings, 11, &ready);
    return ready && Mock::creations == creations;
}
bool failedSignalCannotEnableOrRecreate()
{
    ProxyFixture f;
    if (!f.Create())
        return false;
    const auto creations = Mock::creations;
    f.queue->failSignal = true;
    auto token = f.Submit();
    token.Complete(f.queue.Get());
    f.context.ResetRecording(f.commands.Get());
    bool ready = true;
    f.context.Prepare(f.other.Get(), f.device.Get(), 1280, 720, f.settings, 11, &ready);
    return !ready && Mock::creations == creations;
}
bool abandonedTokenCannotEnableOrRecreate()
{
    ProxyFixture f;
    if (!f.Create())
        return false;
    const auto creations = Mock::creations;
    {
        auto token = f.Submit();
    }
    f.context.ResetRecording(f.commands.Get());
    bool ready = true;
    f.context.Prepare(f.other.Get(), f.device.Get(), 1280, 720, f.settings, 11, &ready);
    return !ready && Mock::creations == creations;
}
bool removedDeviceCannotEnableOrRecreate()
{
    ProxyFixture f;
    if (!f.Create())
        return false;
    const auto creations = Mock::creations;
    auto token = f.Submit();
    token.Complete(f.queue.Get());
    f.queue->RemoveDevice();
    f.context.ResetRecording(f.commands.Get());
    bool ready = true;
    f.context.Prepare(f.other.Get(), f.device.Get(), 1280, 720, f.settings, 11, &ready);
    return !ready && Mock::creations == creations;
}
bool readyFeatureKeepsHistoryAndSurvivesNormalReset()
{
    ProxyFixture f;
    if (!f.Create())
        return false;
    auto token = f.Submit();
    token.Complete(f.queue.Get());
    f.queue->Complete();
    if (!f.context.Ready(10))
        return false;
    f.context.ResetRecording(f.commands.Get());
    const auto creations = Mock::creations;
    bool ready = false;
    f.context.Prepare(f.other.Get(), f.device.Get(), 1280, 720, f.settings, 200, &ready);
    return ready && Mock::creations == creations;
}
bool retiredAndCurrentCreationReceiveNotifications()
{
    ProxyFixture f;
    if (!f.Create())
        return false;
    f.context.RetryAfterFailure();
    if (!f.Create(11, f.other.Get()))
        return false;
    ID3D12CommandList* both[] { f.commands.Get(), f.other.Get() };
    auto token = f.context.BeginSubmission(2, both);
    f.queue->Execute();
    f.context.ResetRecording(f.commands.Get());
    f.context.ResetRecording(f.other.Get());
    token.Complete(f.queue.Get());
    if (f.context.Ready(11))
        return false;
    f.queue->Complete();
    f.context.Collect();
    return f.context.Ready(11) && f.context.Idle();
}
bool prepareCannotEvaluateUnsubmittedModel()
{
    ProxyFixture f;
    DlssNr::Proxy::Frame frame;
    frame.color = frame.depth = frame.motion = frame.output = reinterpret_cast<ID3D12Resource*>(uintptr_t(1));
    frame.size = { 1280, 720 };
    frame.guides = { { 0, 0, 1280, 720 }, { 0, 0, 1280, 720 } };
    const auto evaluations = Mock::evaluations;
    bool evaluated = true;
    f.context.Run(f.commands.Get(), f.device.Get(), frame, f.settings, 10, &evaluated);
    f.context.Run(f.commands.Get(), f.device.Get(), frame, f.settings, 11, &evaluated);
    return !evaluated && Mock::evaluations == evaluations;
}

bool independentOwnersDoNotShareReadiness()
{
    ProxyFixture first, second;
    if (!first.Create() || !second.Create())
        return false;
    auto token = first.Submit();
    token.Complete(first.queue.Get());
    first.queue->Complete();
    return first.context.Ready(10) && !second.context.Ready(200);
}

bool destroyedRecordingRecreatesOnAnotherList()
{
    ProxyFixture f;
    if (!f.Create())
        return false;
    const auto creations = Mock::creations;
    f.commands.Reset();
    return f.Create(10, f.other.Get()) && Mock::creations == creations + 1;
}

bool quarantinedRecordingDoesNotRecreate()
{
    ProxyFixture f;
    if (!f.Create())
        return false;
    const auto creations = Mock::creations;
    f.context.QuarantineSubmission(1, f.lists);
    f.context.ResetRecording(f.commands.Get());
    bool ready = true;
    f.context.Prepare(f.other.Get(), f.device.Get(), 1280, 720, f.settings, 99, &ready);
    return !ready && Mock::creations == creations;
}

int main()
{
    struct Case
    {
        const char* name;
        bool (*run)();
    };
    const Case cases[] {
        { "CPU epoch cannot submit creation", epochCannotSubmitCreation },
        { "other list cannot complete creation", otherListCannotCompleteCreation },
        { "GPU completion works with constant epoch", completionDoesNotRequireEpochChange },
        { "pending creation fence is not ready", pendingFenceIsNotReady },
        { "discarded creation rebuilds only once", discardedCreationRebuildsOnce },
        { "reset before delayed Complete retains feature", submittedResetRetainsFeature },
        { "failed Signal cannot enable or recreate", failedSignalCannotEnableOrRecreate },
        { "abandoned token cannot enable or recreate", abandonedTokenCannotEnableOrRecreate },
        { "removed device cannot enable or recreate", removedDeviceCannotEnableOrRecreate },
        { "ready feature survives normal reset", readyFeatureKeepsHistoryAndSurvivesNormalReset },
        { "independent owners keep independent readiness", independentOwnersDoNotShareReadiness },
        { "destroyed unsubmitted recording recreates", destroyedRecordingRecreatesOnAnotherList },
        { "quarantined recording cannot enable or recreate", quarantinedRecordingDoesNotRecreate },
        { "retired/current creation notifications", retiredAndCurrentCreationReceiveNotifications },
        { "Run cannot evaluate unsubmitted model", prepareCannotEvaluateUnsubmittedModel },
    };
    unsigned failures = 0;
    for (const auto& test : cases)
    {
        const bool pass = test.run();
        std::printf("%s %s\n", pass ? "PASS" : "FAIL", test.name);
        failures += !pass;
    }
    std::printf("Model creation: %zu cases, %u failures\n", std::size(cases), failures);
    return failures ? 1 : 0;
}

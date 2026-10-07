#define main ExistingSubmissionAuditMain
#include "../nr_cpu_submission_audit/SubmissionResetTests.cpp"
#undef main
#include "../../OptiScaler/dlssnr/DlssNr_PrivateCreation.h"
#include <cstring>

// Real command lists retain private interfaces independently by GUID.
struct WatchedCommands final : CpuObject<ID3D12GraphicsCommandList>
{
    std::vector<std::pair<GUID, ComPtr<IUnknown>>> watches;
    HRESULT SetPrivateDataInterface(REFGUID key, const IUnknown* value) override
    {
        for (auto& [stored, watch] : watches)
            if (!std::memcmp(&stored, &key, sizeof(GUID)))
            {
                watch = const_cast<IUnknown*>(value);
                return S_OK;
            }
        watches.emplace_back(key, ComPtr<IUnknown>(const_cast<IUnknown*>(value)));
        return S_OK;
    }
};

struct DlssNr_Dx12
{
    struct State
    {
        struct DeferredSrContext
        {
            struct Generation
            {
                DlssNr::PrivateFeatureCreation creation;
                unsigned* deleted = nullptr;
                ~Generation() { ++*deleted; }
            };
            std::unique_ptr<Generation> current;
            std::vector<Generation*> retiredGenerations;
            unsigned retiredCount = 0;
            DlssNr::GpuLifetime lifetime;
            void RetireCurrent();
        };
    };
};
#include "deferred-retire-under-test.inc"

bool registryRetainsOldCreationUntilResourceCompletion()
{
    DlssNr_Dx12::State::DeferredSrContext context;
    unsigned deleted = 0;
    ComPtr<WatchedCommands> commands;
    commands.Attach(new WatchedCommands);
    ComPtr<ID3D12Device> device;
    device.Attach(new CpuDevice);
    ComPtr<CpuQueue> queue;
    queue.Attach(new CpuQueue(device.Get()));
    context.current = std::make_unique<DlssNr_Dx12::State::DeferredSrContext::Generation>();
    auto* generation = context.current.get();
    generation->deleted = &deleted;
    context.lifetime.Record(commands.Get());
    generation->creation.Record(commands.Get());
    ID3D12CommandList* lists[] { commands.Get() };
    auto creation = generation->creation.BeginSubmission(1, lists);
    auto resource = context.lifetime.BeginSubmission(1, lists);
    queue->Execute();
    context.RetireCurrent();
    if (context.current || context.retiredCount != 1 || context.retiredGenerations.size() != 1 || deleted)
        return false;
    // Delayed Reset/Complete must still reach the exact retired creation helper.
    context.retiredGenerations.front()->creation.ResetRecording(commands.Get());
    context.lifetime.ResetRecording(commands.Get());
    creation.Complete(queue.Get());
    queue->Complete();
    if (!generation->creation.Ready() || deleted)
        return false;
    resource.Complete(queue.Get());
    if (deleted)
        return false;
    queue->Complete();
    context.lifetime.Collect();
    return deleted == 1 && context.retiredCount == 0 && context.retiredGenerations.empty();
}

bool quarantinedRetirementRemainsRegistered()
{
    DlssNr_Dx12::State::DeferredSrContext context;
    unsigned deleted = 0;
    ComPtr<WatchedCommands> commands;
    commands.Attach(new WatchedCommands);
    context.current = std::make_unique<DlssNr_Dx12::State::DeferredSrContext::Generation>();
    context.current->deleted = &deleted;
    context.lifetime.Record(commands.Get());
    context.current->creation.Record(commands.Get());
    context.RetireCurrent();
    if (context.retiredGenerations.size() != 1)
        return false;
    ID3D12CommandList* lists[] { commands.Get() };
    auto* old = context.retiredGenerations.front();
    old->creation.QuarantineSubmission(1, lists);
    context.lifetime.QuarantineSubmission(1, lists);
    old->creation.ResetRecording(commands.Get());
    context.lifetime.ResetRecording(commands.Get());
    context.lifetime.Collect();
    return deleted == 0 && context.retiredCount == 1 && context.retiredGenerations.size() == 1 &&
           !old->creation.Ready() && !old->creation.Idle();
}

int main()
{
    const bool completion = registryRetainsOldCreationUntilResourceCompletion();
    const bool quarantine = quarantinedRetirementRemainsRegistered();
    std::printf("%s: retired creation registry survives delayed completion then clears\n",
                completion ? "PASS" : "FAIL");
    std::printf("%s: quarantined retired generation remains registered\n", quarantine ? "PASS" : "FAIL");
    return completion && quarantine ? 0 : 1;
}

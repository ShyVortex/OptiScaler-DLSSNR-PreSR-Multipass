// Reuse the existing owner/COM fixture and its mechanically extracted production
// ReadyToDestroy. The destructor predicate is extracted independently by run.ps1.
#define main ExistingStateAuditMain
#include "../nr_cpu_submission_audit/StateSubmissionTests.cpp"
#undef main

struct DestructorDecision
{
    DlssNr_Dx12::State* _state;
    DlssNr_Dx12::DescriptorOwner _descriptorSlots;
    bool ReadyToDestroy()
    {
        DlssNr_Dx12 owner;
        owner._state = _state;
        return owner.ReadyToDestroy();
    }
    bool RetainsState(bool finished)
    {
#include "destructor-predicate-under-test.inc"
        {
            return true;
        }
        return false;
    }
};

bool parentIdleDoesNotOverridePrivateFailure(unsigned kind)
{
    DlssNr_Dx12::State state;
    ComPtr<CpuDevice> device;
    device.Attach(new CpuDevice);
    ComPtr<CpuQueue> parentQueue, privateQueue;
    parentQueue.Attach(new CpuQueue(device.Get()));
    privateQueue.Attach(new CpuQueue(device.Get()));
    ComPtr<CpuCommands> commands;
    commands.Attach(new CpuCommands);
    state.lifetime.Record(commands.Get());
    ID3D12CommandList* lists[] { commands.Get() };
    auto parent = state.lifetime.BeginSubmission(1, lists);
    DlssNr::GpuSubmission privateWork;
    if (kind < 2)
    {
        auto& lifetime = kind == 0 ? state.deferredSr.lifetime : state.denoiseFirst.lifetime;
        lifetime.Record(commands.Get());
        privateWork = lifetime.BeginSubmission(1, lists);
        lifetime.ResetRecording(commands.Get());
    }
    else
    {
        state.deferredSr.current = std::make_unique<DlssNr_Dx12::State::DeferredSr::Generation>();
        auto& creation = state.deferredSr.current->creation;
        creation.Record(commands.Get());
        privateWork = creation.BeginSubmission(1, lists);
        creation.ResetRecording(commands.Get());
    }
    state.lifetime.ResetRecording(commands.Get());
    parent.Complete(parentQueue.Get());
    parentQueue->Complete();
    privateQueue->failSignal = true;
    privateWork.Complete(privateQueue.Get());
    DestructorDecision decision { &state };
    const bool parentIdle = state.lifetime.Idle();
    const bool fullGuardReady = decision.ReadyToDestroy();
    const bool retains = decision.RetainsState(true);
    const char* names[] { "Deferred resource tracker", "Denoise resource tracker", "private creation tracker" };
    const bool pass = parentIdle && !fullGuardReady && retains;
    std::printf(
        "%s: destructor retains %s failure despite completed parent (parent-idle=%u, full-ready=%u, retains=%u)\n",
        pass ? "PASS" : "FAIL", names[kind], unsigned(parentIdle), unsigned(fullGuardReady), unsigned(retains));
    return pass;
}

bool registryPreventsDestructorCleanup()
{
    DlssNr_Dx12::State state;
    auto retired = std::make_unique<DlssNr_Dx12::State::DeferredSr::Generation>();
    state.deferredSr.retiredGenerations.push_back(retired.get());
    DestructorDecision decision { &state };
    const bool pass = state.lifetime.Idle() && !decision.ReadyToDestroy() && decision.RetainsState(true);
    std::printf("%s: destructor retains registered retired private generation\n", pass ? "PASS" : "FAIL");
    return pass;
}

int main()
{
    unsigned failures = 0;
    for (unsigned kind = 0; kind < 3; ++kind)
        failures += !parentIdleDoesNotOverridePrivateFailure(kind);
    failures += !registryPreventsDestructorCleanup();
    DlssNr_Dx12::State idle;
    DestructorDecision decision { &idle };
    const bool controls = !decision.RetainsState(true) && decision.RetainsState(false);
    std::printf("%s: idle ownership permits cleanup and failed picture wait retains it\n", controls ? "PASS" : "FAIL");
    return failures || !controls ? 1 : 0;
}

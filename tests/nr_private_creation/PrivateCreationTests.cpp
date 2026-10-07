// Compile the real tracker and reuse its existing CPU-only COM boundary fixtures.
#define main ExistingSubmissionAuditMain
#include "../nr_cpu_submission_audit/SubmissionResetTests.cpp"
#undef main
#include "../../OptiScaler/dlssnr/DlssNr_PrivateCreation.h"
#include <new>
#include <thread>

struct CreationFixture
{
    ComPtr<ID3D12Device> device;
    ComPtr<CpuQueue> queue, otherQueue;
    ComPtr<CpuCommands> commands, other;
    DlssNr::PrivateFeatureCreation creation;
    ID3D12CommandList* lists[1];
    CreationFixture()
    {
        device.Attach(new CpuDevice);
        queue.Attach(new CpuQueue(device.Get()));
        otherQueue.Attach(new CpuQueue(device.Get()));
        commands.Attach(new CpuCommands);
        other.Attach(new CpuCommands);
        lists[0] = commands.Get();
        creation.Record(commands.Get());
    }
    auto Submit()
    {
        auto submission = creation.BeginSubmission(1, lists);
        queue->Execute();
        return submission;
    }
};

// These tests catch replacing exact submitted-fence proof with frame progress,
// a different list's progress, open-recording state, or an always-ready default.
bool onlyCreationFenceEnablesEvaluation()
{
    CreationFixture f;
    if (f.creation.Ready() || f.creation.Discarded() || f.creation.Idle())
        return false;
    ID3D12CommandList* wrongLists[] { f.other.Get() };
    auto wrong = f.creation.BeginSubmission(1, wrongLists);
    wrong.Complete(f.otherQueue.Get());
    f.otherQueue->Complete();
    if (wrong || f.creation.Ready())
        return false;
    auto submission = f.Submit();
    if (!submission || f.creation.Ready())
        return false;
    submission.Complete(f.queue.Get());
    f.otherQueue->Complete();
    if (f.creation.Ready())
        return false;
    f.queue->Complete();
    return f.creation.Ready() && !f.creation.Discarded() && !f.creation.Idle();
}

bool resetUnsubmittedDiscardsCreation()
{
    CreationFixture f;
    f.creation.ResetRecording(f.other.Get());
    if (f.creation.Discarded())
        return false;
    f.creation.ResetRecording(f.commands.Get());
    return !f.creation.Ready() && f.creation.Discarded() && f.creation.Idle();
}

bool destructionUnsubmittedDiscardsCreation()
{
    CreationFixture f;
    f.commands.Reset();
    return !f.creation.Ready() && f.creation.Discarded() && f.creation.Idle();
}

// Reset between Execute and delayed Complete must not erase the captured recording.
bool delayedNotificationSurvivesReset()
{
    CreationFixture f;
    auto submission = f.Submit();
    f.creation.ResetRecording(f.commands.Get());
    if (f.creation.Ready() || f.creation.Discarded() || f.creation.Idle())
        return false;
    submission.Complete(f.queue.Get());
    if (f.creation.Ready() || f.creation.Discarded())
        return false;
    f.queue->Complete();
    return f.creation.Ready() && !f.creation.Discarded() && f.creation.Idle();
}

bool delayedNotificationSurvivesCommandDestruction()
{
    CreationFixture f;
    auto submission = f.Submit();
    f.commands.Reset();
    submission.Complete(f.queue.Get());
    if (f.creation.Ready() || f.creation.Discarded())
        return false;
    f.queue->Complete();
    return f.creation.Ready() && f.creation.Idle();
}

bool commandAddressReuseCannotCompleteDiscardedCreation()
{
    CreationFixture f;
    alignas(CpuCommands) unsigned char storage[sizeof(CpuCommands)];
    auto* first = new (storage) CpuCommands;
    DlssNr::PrivateFeatureCreation creation;
    creation.Record(first);
    first->~CpuCommands();
    auto* replacement = new (storage) CpuCommands;
    ID3D12CommandList* lists[] { replacement };
    auto submission = creation.BeginSubmission(1, lists);
    submission.Complete(f.queue.Get());
    f.queue->Complete();
    const bool pass = !creation.Ready() && creation.Discarded() && !submission;
    replacement->~CpuCommands();
    return pass;
}

bool ownerAddressReuseCannotRedirectOldCallback()
{
    CreationFixture f;
    alignas(DlssNr::PrivateFeatureCreation) unsigned char storage[sizeof(DlssNr::PrivateFeatureCreation)];
    auto* first = new (storage) DlssNr::PrivateFeatureCreation;
    first->Record(f.commands.Get());
    auto old = first->BeginSubmission(1, f.lists);
    first->~PrivateFeatureCreation();
    auto* replacement = new (storage) DlssNr::PrivateFeatureCreation;
    replacement->Record(f.other.Get());
    old.Complete(f.queue.Get());
    f.queue->Complete();
    const bool pass = !replacement->Ready() && !replacement->Discarded();
    replacement->ResetRecording(f.other.Get());
    replacement->~PrivateFeatureCreation();
    return pass;
}

bool capturedExecutionsRequireEveryQueue()
{
    CreationFixture f;
    auto outer = f.Submit();
    auto inner = f.creation.BeginSubmission(1, f.lists);
    f.creation.ResetRecording(f.commands.Get());
    inner.Complete(f.queue.Get());
    f.queue->Complete();
    if (f.creation.Ready())
        return false;
    outer.Complete(f.otherQueue.Get());
    if (f.creation.Ready())
        return false;
    f.otherQueue->Complete();
    return f.creation.Ready();
}

bool replayMustCompleteItsActualQueue()
{
    CreationFixture f;
    auto first = f.Submit();
    first.Complete(f.queue.Get());
    f.queue->Complete();
    if (!f.creation.Ready())
        return false;
    auto replay = f.creation.BeginSubmission(1, f.lists);
    f.creation.ResetRecording(f.commands.Get());
    replay.Complete(f.otherQueue.Get());
    if (f.creation.Ready())
        return false;
    f.otherQueue->Complete();
    return f.creation.Ready();
}

struct FenceFailureDevice final : CpuObject<ID3D12Device>
{
    HRESULT CreateFence(UINT64, D3D12_FENCE_FLAGS, REFIID, void** out) override
    {
        *out = nullptr;
        return E_FAIL;
    }
};

bool fenceCreationFailureRemainsQuarantined()
{
    CreationFixture f;
    ComPtr<ID3D12Device> failedDevice;
    failedDevice.Attach(new FenceFailureDevice);
    f.queue->device = failedDevice;
    auto submission = f.Submit();
    f.creation.ResetRecording(f.commands.Get());
    submission.Complete(f.queue.Get());
    f.queue->Complete();
    return !f.creation.Ready() && !f.creation.Discarded() && !f.creation.Idle();
}

bool completionFailuresRemainQuarantined(unsigned failure)
{
    CreationFixture f;
    {
        auto submission = f.Submit();
        f.creation.ResetRecording(f.commands.Get());
        if (failure != 2)
        {
            f.queue->failSignal = failure == 0;
            submission.Complete(f.queue.Get());
            if (failure == 1)
                static_cast<CpuFence*>(f.queue->fence.Get())->completed = UINT64_MAX;
        }
    }
    f.creation.FinishSubmitted();
    return !f.creation.Ready() && !f.creation.Discarded() && !f.creation.Idle();
}

bool quarantinedSubmissionCannotBecomeReady()
{
    CreationFixture f;
    ID3D12CommandList* wrongLists[] { f.other.Get() };
    f.creation.QuarantineSubmission(1, wrongLists);
    auto submission = f.Submit();
    f.creation.QuarantineSubmission(1, f.lists);
    f.creation.ResetRecording(f.commands.Get());
    submission.Complete(f.queue.Get());
    f.queue->Complete();
    return !f.creation.Ready() && !f.creation.Discarded() && !f.creation.Idle();
}

bool laterRecordCannotReplaceCreationIdentity()
{
    CreationFixture f;
    f.creation.Record(f.other.Get());
    ID3D12CommandList* lists[] { f.other.Get() };
    auto unrelated = f.creation.BeginSubmission(1, lists);
    unrelated.Complete(f.queue.Get());
    f.queue->Complete();
    if (unrelated || f.creation.Ready())
        return false;
    f.creation.ResetRecording(f.commands.Get());
    f.creation.Record(f.other.Get());
    return f.creation.Discarded() && !f.creation.Ready();
}

bool retirementClosesOnlyCompletedSubmissions()
{
    CreationFixture f;
    f.creation.FinishSubmitted();
    if (f.creation.Idle() || f.creation.Discarded())
        return false;
    auto submission = f.Submit();
    submission.Complete(f.queue.Get());
    f.creation.FinishSubmitted();
    if (f.creation.Idle())
        return false;
    f.queue->Complete();
    f.creation.FinishSubmitted();
    return f.creation.Ready() && f.creation.Idle() && !f.creation.Discarded();
}

bool emptyHelperCannotClaimCreationCompletion()
{
    DlssNr::PrivateFeatureCreation creation;
    creation.Record(nullptr);
    creation.ResetRecording(nullptr);
    creation.QuarantineSubmission(0, nullptr);
    auto submission = creation.BeginSubmission(0, nullptr);
    creation.FinishSubmitted();
    return !submission && !creation.Ready() && !creation.Discarded() && creation.Idle();
}

bool concurrentResetAndDelayedCompleteRetainIdentity()
{
    for (unsigned i = 0; i < 100; ++i)
    {
        CreationFixture f;
        auto submission = f.Submit();
        std::thread reset([&] { f.creation.ResetRecording(f.commands.Get()); });
        submission.Complete(f.queue.Get());
        reset.join();
        if (f.creation.Ready() || f.creation.Discarded())
            return false;
        f.queue->Complete();
        if (!f.creation.Ready())
            return false;
    }
    return true;
}

// The GPU can complete between any two fence polls. The boundary returns the
// observed pending value once, then exposes completion to subsequent polls.
struct ProgressFence final : CpuObject<ID3D12Fence>
{
    UINT64 completed = 0;
    bool completeAfterRead = false;
    UINT64 GetCompletedValue() override
    {
        const auto observed = completed;
        if (completeAfterRead)
            completed = 1;
        return observed;
    }
};
struct ProgressDevice final : CpuObject<ID3D12Device>
{
    ComPtr<ProgressFence> fence;
    HRESULT CreateFence(UINT64, D3D12_FENCE_FLAGS, REFIID, void** out) override
    {
        fence.Attach(new ProgressFence);
        *out = static_cast<ID3D12Fence*>(fence.Get());
        fence->AddRef();
        return S_OK;
    }
};

bool gpuProgressBetweenPollsCannotDiscardSubmittedCreation()
{
    CreationFixture f;
    ComPtr<ProgressDevice> device;
    device.Attach(new ProgressDevice);
    f.queue->device = device.Get();
    auto submission = f.Submit();
    f.creation.ResetRecording(f.commands.Get());
    submission.Complete(f.queue.Get());
    device->fence->completeAfterRead = true;
    return !f.creation.Discarded() && f.creation.Ready();
}

int main()
{
    struct Case
    {
        const char* name;
        bool (*run)();
    } cases[] {
        { "only the creation fence enables evaluation", onlyCreationFenceEnablesEvaluation },
        { "unsubmitted reset discards creation", resetUnsubmittedDiscardsCreation },
        { "unsubmitted destruction discards creation", destructionUnsubmittedDiscardsCreation },
        { "delayed notification survives reset", delayedNotificationSurvivesReset },
        { "delayed notification survives command destruction", delayedNotificationSurvivesCommandDestruction },
        { "command address reuse cannot complete discarded creation",
          commandAddressReuseCannotCompleteDiscardedCreation },
        { "owner address reuse cannot redirect old callback", ownerAddressReuseCannotRedirectOldCallback },
        { "all captured queues must complete", capturedExecutionsRequireEveryQueue },
        { "replay must complete its actual queue", replayMustCompleteItsActualQueue },
        { "fence creation failure remains quarantined", fenceCreationFailureRemainsQuarantined },
        { "failed signal remains quarantined", [] { return completionFailuresRemainQuarantined(0); } },
        { "device removal remains quarantined", [] { return completionFailuresRemainQuarantined(1); } },
        { "abandoned submission remains quarantined", [] { return completionFailuresRemainQuarantined(2); } },
        { "quarantined submission cannot become ready", quarantinedSubmissionCannotBecomeReady },
        { "later recording cannot replace creation identity", laterRecordCannotReplaceCreationIdentity },
        { "retirement closes only completed submissions", retirementClosesOnlyCompletedSubmissions },
        { "empty helper cannot claim completion", emptyHelperCannotClaimCreationCompletion },
        { "concurrent reset and Complete retain identity", concurrentResetAndDelayedCompleteRetainIdentity },
        { "GPU progress between polls cannot discard submitted creation",
          gpuProgressBetweenPollsCannotDiscardSubmittedCreation },
    };
    unsigned failures = 0;
    for (const auto& test : cases)
    {
        const bool pass = test.run();
        std::printf("%s: %s\n", pass ? "PASS" : "FAIL", test.name);
        failures += !pass;
    }
    return failures ? 1 : 0;
}

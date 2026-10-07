#define main ExistingSubmissionAuditMain
#include "../nr_cpu_submission_audit/SubmissionResetTests.cpp"
#undef main
#include "../../OptiScaler/dlssnr/DlssNr_PrivateCreation.h"
#include <string>

struct GateOwner
{
    struct Generation
    {
        DlssNr::PrivateFeatureCreation creation;
        bool failed = false;
    } g;
    unsigned evaluations = 0;
    void Say(const std::string&) {}
    void Evaluate()
    {
#include "deferred-gate-under-test.inc"
        ++evaluations;
    }
};

int main()
{
    const char* names[] { "no submission",          "other list submitted",     "creation discarded",
                          "creation fence pending", "creation fence completed", "Reset before delayed Complete",
                          "failed Signal" };
    unsigned failures = 0;
    for (unsigned index = 0; index < 7; ++index)
    {
        GateOwner owner;
        ComPtr<ID3D12Device> device;
        device.Attach(new CpuDevice);
        ComPtr<CpuQueue> queue;
        queue.Attach(new CpuQueue(device.Get()));
        ComPtr<CpuCommands> commands, other;
        commands.Attach(new CpuCommands);
        other.Attach(new CpuCommands);
        owner.g.creation.Record(commands.Get());
        ID3D12CommandList* lists[] { index == 1 ? other.Get() : commands.Get() };
        DlssNr::GpuSubmission pending;
        if (index == 1 || index >= 3)
        {
            pending = owner.g.creation.BeginSubmission(1, lists);
            queue->Execute();
            if (index == 5)
                owner.g.creation.ResetRecording(commands.Get());
            queue->failSignal = index == 6;
            pending.Complete(queue.Get());
            if (index == 1 || index == 4)
                queue->Complete();
        }
        else if (index == 2)
            owner.g.creation.ResetRecording(commands.Get());
        owner.Evaluate();
        const bool pass = owner.evaluations == (index == 4 ? 1u : 0u) && owner.g.failed == (index == 2);
        std::printf("%s: extracted DeferredSr gate: %s\n", pass ? "PASS" : "FAIL", names[index]);
        failures += !pass;
    }
    return failures ? 1 : 0;
}

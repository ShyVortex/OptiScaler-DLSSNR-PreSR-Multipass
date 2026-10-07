// The runner extracts the actual DeferredSr gate from the audited revision.
// No copy of its decision logic is maintained in this fixture.
#include <cstdio>
#include <cstdint>
#define main ExistingSubmissionAuditMain
#include "../nr_cpu_submission_audit/SubmissionResetTests.cpp"
#undef main
#define LOG_DEBUG(...) ((void) 0)

struct BaselineOwner
{
    struct Generation
    {
        uint64_t createEpoch = 10;
    } g;
    uint64_t submittedEpoch = 10;
    unsigned evaluations = 0;
    void Evaluate()
    {
#include "baseline-epoch-under-test.inc"
        ++evaluations;
    }
};

int main()
{
    const char* cases[] { "no creation submission", "only another list submitted", "creation recording discarded" };
    unsigned failures = 0;
    for (unsigned index = 0; index < 3; ++index)
    {
        Fixture gpu;
        auto creationCompleted = gpu.tracker.CompletionProbe(gpu.commands.Get());
        ComPtr<CpuCommands> other;
        other.Attach(new CpuCommands);
        if (index == 1)
        {
            ID3D12CommandList* lists[] { other.Get() };
            auto submission = gpu.tracker.BeginSubmission(1, lists);
            gpu.queue->Execute();
            submission.Complete(gpu.queue.Get());
            gpu.queue->Complete();
        }
        else if (index == 2)
            gpu.tracker.ResetRecording(gpu.commands.Get());
        BaselineOwner owner;
        // Frame/submission epoch advanced, but none of these cases submitted the
        // feature's creation recording. Evaluation must remain unavailable.
        owner.submittedEpoch = 11;
        owner.Evaluate();
        const bool pass = owner.evaluations == 0 && !creationCompleted();
        std::printf("%s: %s (evaluations=%u, creation-completed=%u)\n", pass ? "PASS" : "FAIL", cases[index],
                    owner.evaluations, unsigned(creationCompleted()));
        failures += !pass;
    }
    return failures ? 1 : 0;
}

// The production tracker and submission token are the code under test.
#include "../../OptiScaler/dlssnr/DlssNr_GpuLifetime.cpp"
#include <barrier>
#include <cstdio>
#include <cstring>
#include <thread>

using Microsoft::WRL::ComPtr;

template <class Interface> class CpuObject : public Interface
{
    std::atomic<ULONG> references { 1 };

  public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override
    {
        if (!out)
            return E_POINTER;
        *out = static_cast<Interface*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto remaining = --references;
        if (!remaining)
            delete this;
        return remaining;
    }
    virtual ~CpuObject() = default;
};
struct CpuFence final : CpuObject<ID3D12Fence>
{
    std::atomic<UINT64> completed { 0 };
    UINT64 GetCompletedValue() override { return completed.load(); }
};
struct CpuDevice final : CpuObject<ID3D12Device>
{
    HRESULT CreateFence(UINT64 value, D3D12_FENCE_FLAGS, REFIID, void** out) override
    {
        auto* fence = new CpuFence;
        fence->completed = value;
        *out = static_cast<ID3D12Fence*>(fence);
        return S_OK;
    }
};
struct CpuQueue final : CpuObject<ID3D12CommandQueue>
{
    ComPtr<ID3D12Device> device; // Initialized before either thread starts; immutable thereafter.
    std::mutex mutex;
    ComPtr<ID3D12Fence> fence;
    UINT64 signalValue = 0;
    explicit CpuQueue(ID3D12Device* value) : device(value) {}
    HRESULT GetDevice(REFIID, void** out) override
    {
        *out = device.Get();
        device->AddRef();
        return S_OK;
    }
    HRESULT Signal(ID3D12Fence* value, UINT64 target) override
    {
        std::lock_guard lock(mutex);
        fence = value;
        signalValue = target;
        return S_OK;
    }
    void CompleteGpu()
    {
        std::lock_guard lock(mutex);
        if (fence)
            static_cast<CpuFence*>(fence.Get())->completed = signalValue;
    }
    UINT64 Signalled()
    {
        std::lock_guard lock(mutex);
        return signalValue;
    }
};
struct CpuCommands final : CpuObject<ID3D12GraphicsCommandList>
{
    std::mutex mutex;
    ComPtr<IUnknown> watch; // This fixture has one tracker and therefore one private-data key.
    HRESULT SetPrivateDataInterface(REFGUID, const IUnknown* value) override
    {
        std::lock_guard lock(mutex);
        watch = const_cast<IUnknown*>(value);
        return S_OK;
    }
};

int main(int argc, char** argv)
{
    // Test-local negative control emulates a call site that omits pre-submit capture.
    // It changes no production code and must expose early retirement deterministically.
    const bool postOnly = argc == 2 && std::strcmp(argv[1], "--post-only") == 0;
    ComPtr<CpuDevice> device;
    device.Attach(new CpuDevice);
    ComPtr<CpuQueue> oldQueue, newQueue;
    oldQueue.Attach(new CpuQueue(device.Get()));
    newQueue.Attach(new CpuQueue(device.Get()));
    ComPtr<CpuCommands> commands;
    commands.Attach(new CpuCommands);
    DlssNr::GpuLifetime tracker;
    ID3D12CommandList* lists[] { commands.Get() };
    std::atomic<unsigned> oldReleased { 0 }, newReleased { 0 };
    std::atomic_bool oldGpuPending { true }, newGpuPending { false }, earlyRelease { false };
    std::barrier rendezvous(2);
    tracker.Record(commands.Get());
    tracker.Retire(
        [&]
        {
            if (oldGpuPending.load())
                earlyRelease = true;
            ++oldReleased;
        });
    auto oldSubmission = postOnly ? DlssNr::GpuSubmission {} : tracker.BeginSubmission(1, lists);

    std::thread recorder(
        [&]
        {
            rendezvous.arrive_and_wait(); // A: the original execution is captured and pending.
            tracker.ResetRecording(commands.Get());
            tracker.Record(commands.Get()); // Same object, a distinct recording generation.
            tracker.Retire(
                [&]
                {
                    if (newGpuPending.load())
                        earlyRelease = true;
                    ++newReleased;
                });
            auto newSubmission = tracker.BeginSubmission(1, lists);
            newGpuPending = true;
            tracker.ResetRecording(commands.Get()); // Close it while its own submission is pinned.
            rendezvous.arrive_and_wait();           // B: both recording generations are closed.
            rendezvous.arrive_and_wait();           // C: old notification and old GPU completion occurred.
            newSubmission.Complete(newQueue.Get()); // Notification arrives on the recorder thread.
            rendezvous.arrive_and_wait();           // D: new work has a fence but is still pending.
            rendezvous.arrive_and_wait();           // E: the main thread advanced the new GPU fence.
            tracker.Collect();
            rendezvous.arrive_and_wait(); // F: final collection is observable by the main thread.
        });

    rendezvous.arrive_and_wait(); // A
    rendezvous.arrive_and_wait(); // B
    const bool retainedBeforeNotification = oldReleased == 0 && newReleased == 0;
    if (postOnly)
        tracker.Submitted(oldQueue.Get(), 1, lists);
    else
        oldSubmission.Complete(oldQueue.Get());
    const bool retainedBeforeFence = oldReleased == 0 && newReleased == 0;
    oldGpuPending = false;
    oldQueue->CompleteGpu();
    tracker.Collect();
    const bool oldOnlyCompleted = oldReleased == 1 && newReleased == 0 && !tracker.Idle();
    rendezvous.arrive_and_wait(); // C
    rendezvous.arrive_and_wait(); // D
    tracker.Collect();
    const bool newStillPending = newReleased == 0 && !tracker.Idle();
    newGpuPending = false;
    newQueue->CompleteGpu();
    rendezvous.arrive_and_wait(); // E
    rendezvous.arrive_and_wait(); // F
    recorder.join();
    const bool allCompleted = oldReleased == 1 && newReleased == 1 && tracker.Idle();
    const bool signalledBoth = oldQueue->Signalled() == 1 && newQueue->Signalled() == 1;
    const bool passed = retainedBeforeNotification && retainedBeforeFence && oldOnlyCompleted && newStillPending &&
                        allCompleted && signalledBoth && !earlyRelease;
    std::printf("mode=%s, retained_before_notification=%d, retained_before_fence=%d\n",
                postOnly ? "post-only negative control" : "captured submission", retainedBeforeNotification,
                retainedBeforeFence);
    std::printf("old_only_completed=%d, new_pending_after_old_completion=%d, all_completed=%d\n", oldOnlyCompleted,
                newStillPending, allCompleted);
    std::printf("old_releases=%u, new_releases=%u, old_signals=%llu, new_signals=%llu, early_release=%d\n",
                oldReleased.load(), newReleased.load(), oldQueue->Signalled(), newQueue->Signalled(),
                earlyRelease.load());
    std::puts(passed ? "PASS: threaded handoff retains each generation through its own completion"
                     : "FAIL: threaded handoff violated generation ownership");
    return passed ? 0 : 1;
}

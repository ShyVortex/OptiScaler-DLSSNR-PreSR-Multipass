// Compile the unmodified production tracker; only the external COM/GPU boundary is fake.
#include "../../OptiScaler/dlssnr/DlssNr_GpuLifetime.cpp"
#include <cstdio>

using Microsoft::WRL::ComPtr;

template <class Interface> class CpuObject : public Interface
{
    ULONG references = 1;

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
    UINT64 completed = 0;
    UINT64 GetCompletedValue() override { return completed; }
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
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Fence> fence;
    UINT64 signalled = 0;
    bool workPending = false;
    bool failSignal = false;
    explicit CpuQueue(ID3D12Device* value) : device(value) {}
    HRESULT GetDevice(REFIID, void** out) override
    {
        *out = device.Get();
        device->AddRef();
        return S_OK;
    }
    HRESULT Signal(ID3D12Fence* value, UINT64 target) override
    {
        fence = value;
        signalled = target;
        return failSignal ? E_FAIL : S_OK;
    }
    // Represents the real ExecuteCommandLists call returning, not completion.
    void Execute() { workPending = true; }
    void Complete()
    {
        workPending = false;
        if (fence)
            static_cast<CpuFence*>(fence.Get())->completed = signalled;
    }
};
struct CpuCommands final : CpuObject<ID3D12GraphicsCommandList>
{
    ComPtr<IUnknown> watch;
    HRESULT SetPrivateDataInterface(REFGUID, const IUnknown* value) override
    {
        watch = const_cast<IUnknown*>(value);
        return S_OK;
    }
};

struct Result
{
    bool early;
    bool eventually;
    UINT64 signals;
};
template <class Tracker> auto prepare(Tracker& tracker, UINT count, ID3D12CommandList* const* lists)
{
    if constexpr (requires { tracker.BeginSubmission(count, lists); })
        return tracker.BeginSubmission(count, lists);
    else
    {
        struct LegacySubmission
        {
            Tracker& tracker;
            UINT count;
            ID3D12CommandList* const* lists;
            void Complete(ID3D12CommandQueue* queue) { tracker.Submitted(queue, count, lists); }
        };
        return LegacySubmission { tracker, count, lists };
    }
}
Result exercise(bool resetBeforeNotification)
{
    ComPtr<ID3D12Device> device;
    device.Attach(new CpuDevice);
    ComPtr<CpuQueue> queue;
    queue.Attach(new CpuQueue(device.Get()));
    ComPtr<CpuCommands> commands;
    commands.Attach(new CpuCommands);
    DlssNr::GpuLifetime tracker;
    unsigned released = 0;
    bool releasedDuringPendingWork = false;
    tracker.Record(commands.Get());
    tracker.Retire(
        [&]
        {
            ++released;
            releasedDuringPendingWork |= queue->workPending;
        });
    ID3D12CommandList* lists[] { commands.Get() };
    auto submission = prepare(tracker, 1, lists);
    queue->Execute();
    if (resetBeforeNotification)
        tracker.ResetRecording(commands.Get());
    submission.Complete(queue.Get());
    if (!resetBeforeNotification)
        tracker.ResetRecording(commands.Get());
    const auto signals = queue->signalled;
    queue->Complete();
    tracker.Collect();
    return { releasedDuringPendingWork, released == 1 && tracker.Idle(), signals };
}

struct Fixture
{
    ComPtr<ID3D12Device> device;
    ComPtr<CpuQueue> queue, otherQueue;
    ComPtr<CpuCommands> commands;
    DlssNr::GpuLifetime tracker;
    ID3D12CommandList* lists[1];
    unsigned released = 0;
    Fixture()
    {
        device.Attach(new CpuDevice);
        queue.Attach(new CpuQueue(device.Get()));
        otherQueue.Attach(new CpuQueue(device.Get()));
        commands.Attach(new CpuCommands);
        lists[0] = commands.Get();
        tracker.Record(commands.Get());
        tracker.Retire([this] { ++released; });
    }
};

bool nestedSubmissionsRetainBothQueues()
{
    Fixture f;
    auto outer = prepare(f.tracker, 1, f.lists);
    auto inner = prepare(f.tracker, 1, f.lists);
    f.tracker.ResetRecording(f.commands.Get());
    inner.Complete(f.queue.Get());
    f.queue->Complete();
    f.tracker.Collect();
    if (f.released)
        return false; // still pinned by the outer Execute
    outer.Complete(f.otherQueue.Get());
    f.tracker.Collect();
    if (f.released)
        return false; // outer queue has not completed
    f.otherQueue->Complete();
    f.tracker.Collect();
    return f.released == 1 && f.tracker.Idle();
}

bool newGenerationIsNotCompletedByOldNotification()
{
    Fixture f;
    auto old = prepare(f.tracker, 1, f.lists);
    f.tracker.ResetRecording(f.commands.Get());
    f.tracker.Record(f.commands.Get());
    unsigned newReleased = 0;
    f.tracker.Retire([&] { ++newReleased; });
    old.Complete(f.queue.Get());
    f.queue->Complete();
    f.tracker.Collect();
    if (f.released != 1 || newReleased != 0 || f.tracker.Idle())
        return false;
    auto newer = prepare(f.tracker, 1, f.lists);
    f.tracker.ResetRecording(f.commands.Get());
    newer.Complete(f.otherQueue.Get());
    f.tracker.Collect();
    if (newReleased)
        return false;
    f.otherQueue->Complete();
    f.tracker.Collect();
    return newReleased == 1 && f.tracker.Idle();
}

bool replayRetainsActualQueue()
{
    Fixture f;
    prepare(f.tracker, 1, f.lists).Complete(f.queue.Get());
    f.queue->Complete();
    f.tracker.Collect();
    if (f.released)
        return false; // still replayable
    auto replay = prepare(f.tracker, 1, f.lists);
    f.tracker.ResetRecording(f.commands.Get());
    replay.Complete(f.otherQueue.Get());
    f.tracker.Collect();
    if (f.released)
        return false;
    f.otherQueue->Complete();
    f.tracker.Collect();
    return f.released == 1 && f.tracker.Idle();
}

bool unprovableCompletionIsQuarantined(unsigned failure)
{
    Fixture f;
    {
        auto pending = prepare(f.tracker, 1, f.lists);
        f.tracker.ResetRecording(f.commands.Get());
        if (failure != 2)
        {
            f.queue->failSignal = failure == 0;
            pending.Complete(f.queue.Get());
            if (failure == 1)
                static_cast<CpuFence*>(f.queue->fence.Get())->completed = UINT64_MAX;
        } // case 2 intentionally abandons the transaction after Execute
    }
    f.tracker.Collect();
    return !f.released && !f.tracker.Idle();
}

bool completedTokenIsIdempotent()
{
    Fixture f;
    auto pending = prepare(f.tracker, 1, f.lists);
    pending.Complete(f.queue.Get());
    pending.Complete(f.otherQueue.Get());
    f.tracker.ResetRecording(f.commands.Get());
    f.queue->Complete();
    f.tracker.Collect();
    return f.released == 1 && f.otherQueue->signalled == 0;
}

int main()
{
    const auto control = exercise(false);
    const auto interleaved = exercise(true);
    std::printf("normal order: early_release=%d, eventual_release=%d, fence_signals=%llu\n", control.early,
                control.eventually, control.signals);
    std::printf("reset before submission notification: early_release=%d, eventual_release=%d, fence_signals=%llu\n",
                interleaved.early, interleaved.eventually, interleaved.signals);
    if (control.early || !control.eventually || control.signals != 1)
    {
        std::puts("FAIL: control did not retain ownership until fence completion");
        return 2;
    }
    if (interleaved.early)
    {
        std::puts("FAIL: production tracker released retired ownership before pending work completed");
        return 1;
    }
    bool passed = true;
    auto check = [&](const char* name, bool result)
    {
        std::printf("%s: %s\n", name, result ? "PASS" : "FAIL");
        passed &= result;
    };
    check("nested submissions retain both queues", nestedSubmissionsRetainBothQueues());
    check("new generation is not completed by old notification", newGenerationIsNotCompletedByOldNotification());
    check("replay retains actual queue", replayRetainsActualQueue());
    check("failed Signal quarantines ownership", unprovableCompletionIsQuarantined(0));
    check("device removal quarantines ownership", unprovableCompletionIsQuarantined(1));
    check("abandoned token quarantines ownership", unprovableCompletionIsQuarantined(2));
    check("completed token is idempotent", completedTokenIsIdempotent());
    return passed ? 0 : 1;
}

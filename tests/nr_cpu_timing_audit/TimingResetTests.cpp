#include "CpuD3d12.h"
// Actual timer implementation, not a copy of its slot/fence logic.
#include "../../OptiScaler/dlssnr/DlssNr_GpuLifetime.cpp"
#include "../../OptiScaler/shaders/dlssnr/DlssNr_GpuTime.h"
#include <cstdio>
#include <utility>

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
struct CpuQueryHeap final : CpuObject<ID3D12QueryHeap>
{
};
struct CpuReadback final : CpuObject<ID3D12Resource>
{
    std::vector<UINT64> data;
    unsigned maps = 0, unmaps = 0;
    explicit CpuReadback(UINT64 bytes) : data(static_cast<size_t>(bytes / sizeof(UINT64)), 0) {}
    HRESULT Map(UINT, const D3D12_RANGE*, void** out) override
    {
        ++maps;
        *out = data.data();
        return S_OK;
    }
    void Unmap(UINT, const D3D12_RANGE*) override { ++unmaps; }
};
struct CpuDevice final : CpuObject<ID3D12Device>
{
    ComPtr<CpuReadback> readback;
    HRESULT CreateQueryHeap(const D3D12_QUERY_HEAP_DESC*, REFIID, void** out) override
    {
        *out = static_cast<ID3D12QueryHeap*>(new CpuQueryHeap);
        return S_OK;
    }
    HRESULT CreateCommittedResource(const CD3DX12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const CD3DX12_RESOURCE_DESC* desc,
                                    D3D12_RESOURCE_STATES, const void*, REFIID, void** out) override
    {
        readback.Attach(new CpuReadback(desc->Width));
        *out = static_cast<ID3D12Resource*>(readback.Get());
        readback->AddRef();
        return S_OK;
    }
    HRESULT CreateFence(UINT64 value, D3D12_FENCE_FLAGS, REFIID, void** out) override
    {
        auto* fence = new CpuFence;
        fence->completed = value;
        *out = static_cast<ID3D12Fence*>(fence);
        return S_OK;
    }
};
struct CpuCommands final : CpuObject<ID3D12GraphicsCommandList>
{
    std::vector<UINT> queries;
    std::vector<std::pair<GUID, ComPtr<IUnknown>>> watches;
    HRESULT SetPrivateDataInterface(REFGUID key, const IUnknown* value) override
    {
        for (auto& entry : watches)
            if (std::memcmp(&entry.first, &key, sizeof(GUID)) == 0)
            {
                entry.second = const_cast<IUnknown*>(value);
                return S_OK;
            }
        watches.push_back({ key, const_cast<IUnknown*>(value) });
        return S_OK;
    }
    void EndQuery(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT index) override { queries.push_back(index); }
    void ResolveQueryData(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT, UINT, ID3D12Resource* target,
                          UINT64 offset) override
    {
        // Hand-authored timestamp payload. The production fence must gate reads;
        // this boundary fixture does not model GPU execution of ResolveQueryData.
        auto& data = static_cast<CpuReadback*>(target)->data;
        data[static_cast<size_t>(offset / sizeof(UINT64))] = 1000;
        data[static_cast<size_t>(offset / sizeof(UINT64)) + 1] = 2000;
    }
};
struct CpuQueue final : CpuObject<ID3D12CommandQueue>
{
    struct SignalPoint
    {
        ComPtr<ID3D12Fence> fence;
        UINT64 value;
    };
    std::vector<SignalPoint> signals;
    ComPtr<ID3D12Device> device;
    bool failSignal = false;
    explicit CpuQueue(ID3D12Device* value) : device(value) {}
    HRESULT GetDevice(REFIID, void** out) override
    {
        *out = device.Get();
        device->AddRef();
        return S_OK;
    }
    HRESULT GetTimestampFrequency(UINT64* out) override
    {
        *out = 1000000;
        return S_OK;
    }
    HRESULT Signal(ID3D12Fence* fence, UINT64 value) override
    {
        if (failSignal)
            return E_FAIL;
        signals.push_back({ fence, value });
        return S_OK;
    }
    void Complete()
    {
        for (const auto& signal : signals)
            static_cast<CpuFence*>(signal.fence.Get())->completed = signal.value;
    }
    void RemoveDevice()
    {
        for (const auto& signal : signals)
            static_cast<CpuFence*>(signal.fence.Get())->completed = UINT64_MAX;
    }
};

struct Fixture
{
    ComPtr<CpuDevice> device;
    ComPtr<CpuQueue> queue;
    ComPtr<CpuCommands> first, next;
    Fixture()
    {
        device.Attach(new CpuDevice);
        queue.Attach(new CpuQueue(device.Get()));
        first.Attach(new CpuCommands);
        next.Attach(new CpuCommands);
    }
};

// This compatibility adapter lets the same behavioral tests run against the original
// post-submit-only header and the transaction implementation. It does no bookkeeping.
template <class Timer> struct LegacyNotification
{
    Timer& timer;
    UINT count;
    ID3D12CommandList* const* lists;
    void Complete(ID3D12CommandQueue* queue) { timer.Submitted(queue, count, lists); }
};
template <class Timer> auto BeginSubmission(Timer& timer, UINT count, ID3D12CommandList* const* lists)
{
    if constexpr (requires { timer.BeginSubmission(count, lists); })
        return timer.BeginSubmission(count, lists);
    else
        return LegacyNotification<Timer> { timer, count, lists };
}

UINT Record(DlssNrGpuTime& timer, CpuCommands* commands)
{
    const auto previous = commands->queries.size();
    timer.Start(commands);
    timer.End(commands);
    return commands->queries.size() == previous ? UINT_MAX : commands->queries[previous];
}

bool NormalOrderPreservesPendingQuery()
{
    Fixture f;
    DlssNrGpuTime timer(f.device.Get());
    const auto firstIndex = Record(timer, f.first.Get());
    ID3D12CommandList* lists[] { f.first.Get() };
    auto submission = BeginSubmission(timer, 1, lists);
    // The real queue call has returned; the simulated GPU has not completed anything.
    submission.Complete(f.queue.Get());
    timer.ResetRecording(f.first.Get());
    const auto nextIndex = Record(timer, f.next.Get());
    const bool ok = firstIndex != UINT_MAX && nextIndex != UINT_MAX && firstIndex != nextIndex;
    std::printf("normal order: first_query=%u, pending_next_query=%u: %s\n", firstIndex, nextIndex,
                ok ? "PASS" : "FAIL");
    return ok;
}

bool ResetBeforeNotificationPreservesPendingQuery()
{
    Fixture f;
    DlssNrGpuTime timer(f.device.Get());
    const auto firstIndex = Record(timer, f.first.Get());
    ID3D12CommandList* lists[] { f.first.Get() };
    auto submission = BeginSubmission(timer, 1, lists);
    // Legal command-list Reset does not imply GPU completion. Interleave it after the
    // actual Execute call but before the NR hook's post-submit notification.
    timer.ResetRecording(f.first.Get());
    submission.Complete(f.queue.Get());
    const auto nextIndex = Record(timer, f.next.Get());
    const bool ok = firstIndex != UINT_MAX && nextIndex != UINT_MAX && firstIndex != nextIndex;
    std::printf("reset before notification: first_query=%u, pending_next_query=%u: %s\n", firstIndex, nextIndex,
                ok ? "PASS" : "FAIL");
    return ok;
}

bool FailedSignalQuarantinesPendingQuery()
{
    Fixture f;
    DlssNrGpuTime timer(f.device.Get());
    const auto firstIndex = Record(timer, f.first.Get());
    ID3D12CommandList* lists[] { f.first.Get() };
    auto submission = BeginSubmission(timer, 1, lists);
    f.queue->failSignal = true;
    submission.Complete(f.queue.Get());
    timer.ResetRecording(f.first.Get());
    const auto nextIndex = Record(timer, f.next.Get());
    const bool ok = firstIndex != UINT_MAX && nextIndex != UINT_MAX && firstIndex != nextIndex;
    std::printf("signal failure: first_query=%u, next_query=%u: %s\n", firstIndex, nextIndex, ok ? "PASS" : "FAIL");
    return ok;
}

bool RemovedDeviceDoesNotCountAsCompletion()
{
    Fixture f;
    DlssNrGpuTime timer(f.device.Get());
    const auto firstIndex = Record(timer, f.first.Get());
    ID3D12CommandList* lists[] { f.first.Get() };
    auto submission = BeginSubmission(timer, 1, lists);
    submission.Complete(f.queue.Get());
    timer.ResetRecording(f.first.Get());
    f.queue->RemoveDevice();
    const auto nextIndex = Record(timer, f.next.Get());
    const bool ok = firstIndex != UINT_MAX && nextIndex != UINT_MAX && firstIndex != nextIndex;
    std::printf("device removal: first_query=%u, next_query=%u: %s\n", firstIndex, nextIndex, ok ? "PASS" : "FAIL");
    return ok;
}

bool DelayedNotificationCannotAttachToNewRecording()
{
    Fixture f;
    DlssNrGpuTime timer(f.device.Get());
    const auto firstIndex = Record(timer, f.first.Get());
    ID3D12CommandList* lists[] { f.first.Get() };
    auto submission = BeginSubmission(timer, 1, lists);
    timer.ResetRecording(f.first.Get());
    // The same command-list object now records a new generation before the old
    // submission notification finishes. Completion of the old work cannot free it.
    const auto rerecordedIndex = Record(timer, f.first.Get());
    submission.Complete(f.queue.Get());
    f.queue->Complete();
    const auto nextIndex = Record(timer, f.next.Get());
    const bool ok = firstIndex != UINT_MAX && rerecordedIndex != UINT_MAX && nextIndex != UINT_MAX &&
                    firstIndex != rerecordedIndex && nextIndex != rerecordedIndex;
    std::printf("same object, new recording: old_query=%u, new_query=%u, next_query=%u: %s\n", firstIndex,
                rerecordedIndex, nextIndex, ok ? "PASS" : "FAIL");
    return ok;
}

bool ReplayWaitsForItsActualQueue()
{
    Fixture f;
    DlssNrGpuTime timer(f.device.Get());
    ComPtr<CpuQueue> replayQueue;
    replayQueue.Attach(new CpuQueue(f.device.Get()));
    const auto firstIndex = Record(timer, f.first.Get());
    ID3D12CommandList* lists[] { f.first.Get() };
    auto first = BeginSubmission(timer, 1, lists);
    first.Complete(f.queue.Get());
    f.queue->Complete();
    // Replaying a completed, closed command list is legal. Its later submission
    // remains pending on a distinct queue when Reset closes the recording.
    auto replay = BeginSubmission(timer, 1, lists);
    replay.Complete(replayQueue.Get());
    timer.ResetRecording(f.first.Get());
    const auto nextIndex = Record(timer, f.next.Get());
    const bool ok = firstIndex != UINT_MAX && nextIndex != UINT_MAX && firstIndex != nextIndex;
    std::printf("replay on another queue: first_query=%u, pending_next_query=%u: %s\n", firstIndex, nextIndex,
                ok ? "PASS" : "FAIL");
    return ok;
}

bool ExhaustedPoolSkipsUntilCompleted()
{
    Fixture f;
    DlssNrGpuTime timer(f.device.Get());
    std::vector<UINT> pendingIndices;
    std::vector<ComPtr<CpuCommands>> recordings;
    bool distinct = true;
    bool exhausted = false;
    // Bound the fixture without depending on the production pool-size constant.
    for (unsigned i = 0; i < 64; ++i)
    {
        ComPtr<CpuCommands> commands;
        commands.Attach(new CpuCommands);
        const auto index = Record(timer, commands.Get());
        if (index == UINT_MAX)
        {
            exhausted = true;
            break;
        }
        for (auto previous : pendingIndices)
            distinct &= index != previous;
        pendingIndices.push_back(index);
        ID3D12CommandList* lists[] { commands.Get() };
        auto submission = BeginSubmission(timer, 1, lists);
        submission.Complete(f.queue.Get());
        timer.ResetRecording(commands.Get());
        recordings.push_back(std::move(commands));
    }
    f.queue->Complete();
    const auto afterCompletion = Record(timer, f.next.Get());
    const bool ok = distinct && exhausted && !pendingIndices.empty() && afterCompletion != UINT_MAX;
    std::printf("pool exhaustion: pending=%zu, distinct=%d, skipped=%d, resumed=%d: %s\n", pendingIndices.size(),
                distinct, exhausted, afterCompletion != UINT_MAX, ok ? "PASS" : "FAIL");
    return ok;
}

bool CompletedTimingIsReadableBeforeResetButSlotIsNotReusable()
{
    Fixture f;
    DlssNrGpuTime timer(f.device.Get());
    const auto index = Record(timer, f.first.Get());
    ID3D12CommandList* lists[] { f.first.Get() };
    auto submission = BeginSubmission(timer, 1, lists);
    submission.Complete(f.queue.Get());
    if (timer.ReadGpuTime())
        return false;
    f.queue->Complete();
    const auto duration = timer.ReadGpuTime();
    const auto next = Record(timer, f.next.Get());
    const bool ok = duration && *duration == 1.0 && next != index;
    std::printf("completed timing readable before reset; replayable slot retained: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

bool CompletedReadbackIsConsumedOnceAndReplayRearmsIt()
{
    Fixture f;
    DlssNrGpuTime timer(f.device.Get());
    const auto firstIndex = Record(timer, f.first.Get());
    ID3D12CommandList* lists[] { f.first.Get() };
    BeginSubmission(timer, 1, lists).Complete(f.queue.Get());
    f.queue->Complete();
    bool ok = true;
    for (unsigned i = 0; i < 12; ++i)
        ok &= timer.ReadGpuTime() == 1.0;
    auto& readback = *f.device->readback.Get();
    const auto initialReads = readback.maps;
    ok &= initialReads == 1 && readback.unmaps == 1;

    ComPtr<CpuQueue> replayQueue;
    replayQueue.Attach(new CpuQueue(f.device.Get()));
    auto replay = BeginSubmission(timer, 1, lists);
    for (unsigned i = 0; i < 5; ++i)
        timer.ReadGpuTime();
    ok &= readback.maps == 1; // pending transaction must not read old completion
    replay.Complete(replayQueue.Get());
    for (unsigned i = 0; i < 5; ++i)
        timer.ReadGpuTime();
    ok &= readback.maps == 1; // replay's own queue has not completed
    replayQueue->Complete();
    for (unsigned i = 0; i < 12; ++i)
        timer.ReadGpuTime();
    const auto replayReads = readback.maps;
    ok &= replayReads == 2 && readback.unmaps == 2;

    timer.ResetRecording(f.first.Get());
    const auto recycled = Record(timer, f.next.Get());
    ok &= recycled == firstIndex && readback.maps == 2;
    readback.data[recycled] = 1000;
    readback.data[recycled + 1] = 4000;
    ID3D12CommandList* newLists[] { f.next.Get() };
    BeginSubmission(timer, 1, newLists).Complete(f.queue.Get());
    f.queue->Complete();
    for (unsigned i = 0; i < 12; ++i)
        ok &= timer.ReadGpuTime() == 3.0;
    ok &= readback.maps == 3 && readback.unmaps == 3;
    timer.ClearLast();
    for (unsigned i = 0; i < 5; ++i)
        ok &= !timer.ReadGpuTime();
    ok &= readback.maps == 3;
    std::printf("readback once/replay/recycle: first=%u, replay=%u, total=%u: %s\n", initialReads, replayReads,
                readback.maps, ok ? "PASS" : "FAIL");
    return ok;
}

// FinishSubmitted is an owner-retirement notification, not GPU completion.
// Removing its completion guard would read/reuse an in-flight timestamp pair;
// making it a no-op would leave the completed replayable pair unreclaimable.
bool RetiredTimingRequiresFenceBeforeReadAndReuse(bool resetBeforeNotification)
{
    Fixture f;
    DlssNrGpuTime timer(f.device.Get());
    const auto first = Record(timer, f.first.Get());
    ID3D12CommandList* lists[] { f.first.Get() };
    auto submission = BeginSubmission(timer, 1, lists);
    if (resetBeforeNotification)
    {
        timer.ResetRecording(f.first.Get());
        timer.FinishSubmitted(); // the captured token has not attached its fence yet
    }
    else
        submission.Complete(f.queue.Get());
    timer.FinishSubmitted();
    const bool unread = !timer.ReadGpuTime();
    const auto whilePending = Record(timer, f.next.Get());
    if (resetBeforeNotification)
    {
        submission.Complete(f.queue.Get());
        timer.FinishSubmitted();
    }
    const bool stillUnread = !timer.ReadGpuTime();
    f.queue->Complete();
    timer.FinishSubmitted();
    const auto duration = timer.ReadGpuTime();
    const auto reclaimed = Record(timer, f.next.Get());
    const bool ok = first != UINT_MAX && whilePending != UINT_MAX && whilePending != first && unread && stillUnread &&
                    duration == 1.0 && reclaimed == first;
    std::printf("retired timer %s waits for fence before read/reuse, then reclaims: %s\n",
                resetBeforeNotification ? "Reset/Finish before Complete" : "Finish while submitted in flight",
                ok ? "PASS" : "FAIL");
    return ok;
}

bool RetirementDoesNotDiscardUnsubmittedTiming()
{
    Fixture f;
    DlssNrGpuTime timer(f.device.Get());
    const auto first = Record(timer, f.first.Get());
    timer.FinishSubmitted();
    const auto next = Record(timer, f.next.Get());
    const bool ok = first != UINT_MAX && next != UINT_MAX && next != first && !timer.ReadGpuTime();
    std::printf("retired timer Finish preserves unsubmitted recording: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

bool RetiredTimingFailureRemainsQuarantined(bool removed)
{
    Fixture f;
    DlssNrGpuTime timer(f.device.Get());
    const auto first = Record(timer, f.first.Get());
    ID3D12CommandList* lists[] { f.first.Get() };
    auto submission = BeginSubmission(timer, 1, lists);
    f.queue->failSignal = !removed;
    submission.Complete(f.queue.Get());
    if (removed)
        f.queue->RemoveDevice();
    timer.ResetRecording(f.first.Get());
    timer.FinishSubmitted();
    const auto next = Record(timer, f.next.Get());
    timer.FinishSubmitted();
    const bool ok = first != UINT_MAX && next != UINT_MAX && next != first && !timer.ReadGpuTime();
    std::printf("retired timer %s stays unreadable/unreusable after Finish: %s\n",
                removed ? "removed-device fence" : "failed Signal", ok ? "PASS" : "FAIL");
    return ok;
}

int main()
{
    const bool normal = NormalOrderPreservesPendingQuery();
    const bool interleaved = ResetBeforeNotificationPreservesPendingQuery();
    const bool failure = FailedSignalQuarantinesPendingQuery();
    const bool removed = RemovedDeviceDoesNotCountAsCompletion();
    const bool rerecorded = DelayedNotificationCannotAttachToNewRecording();
    const bool replay = ReplayWaitsForItsActualQueue();
    const bool bounded = ExhaustedPoolSkipsUntilCompleted();
    const bool readable = CompletedTimingIsReadableBeforeResetButSlotIsNotReusable();
    const bool readOnce = CompletedReadbackIsConsumedOnceAndReplayRearmsIt();
    const bool retiredInFlight = RetiredTimingRequiresFenceBeforeReadAndReuse(false);
    const bool retiredPending = RetiredTimingRequiresFenceBeforeReadAndReuse(true);
    const bool retiredUnsubmitted = RetirementDoesNotDiscardUnsubmittedTiming();
    const bool retiredSignalFailure = RetiredTimingFailureRemainsQuarantined(false);
    const bool retiredDeviceRemoval = RetiredTimingFailureRemainsQuarantined(true);
    return normal && interleaved && failure && removed && rerecorded && replay && bounded && readable && readOnce &&
                   retiredInFlight && retiredPending && retiredUnsubmitted && retiredSignalFailure &&
                   retiredDeviceRemoval
               ? 0
               : 1;
}

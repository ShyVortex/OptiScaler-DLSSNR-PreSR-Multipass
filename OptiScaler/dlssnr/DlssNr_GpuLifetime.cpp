#include "pch.h"
#include "DlssNr_GpuLifetime.h"
#include <Util.h>
#include <algorithm>
#include <vector>
#include <map>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <objbase.h>

namespace DlssNr
{
namespace
{
template <typename T> T* Identity(T* object)
{
    T* real = nullptr;
    return object && Util::CheckForRealObject(__FUNCTION__, object, (IUnknown**) &real) ? real : object;
}
} // namespace

struct GpuLifetime::Impl
{
    // Reset notifications may arrive on a different engine thread from Record/Retire.
    // Recursive because collection/destruction can re-enter the tracker on this thread.
    std::recursive_mutex mutex;
    struct Timeline
    {
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        UINT64 value = 0;
        bool failed = false;
    };
    struct Recording
    {
        ID3D12CommandList* commands = nullptr; // identity only, never dereferenced
        std::atomic_bool open { true };
        bool signalFailed = false;
        unsigned pendingSubmissions = 0;
        // Only the latest value on each queue is needed, including when the list is replayed.
        std::map<std::shared_ptr<Timeline>, UINT64> completions;
        bool Complete() const { return !open && Finished(); }
        bool Finished() const
        {
            return !signalFailed && !pendingSubmissions &&
                   std::all_of(completions.begin(), completions.end(),
                               [](const auto& c)
                               {
                                   const auto& [timeline, value] = c;
                                   const auto completed = timeline->fence->GetCompletedValue();
                                   return completed != UINT64_MAX && completed >= value;
                               });
        }
    };
    // Command lists can be released instead of Reset. A private IUnknown notification
    // closes that recording without retaining/dereferencing the command list itself.
    struct RecordingWatch final : IUnknown
    {
        std::atomic<ULONG> references { 1 };
        std::weak_ptr<Recording> recording;
        explicit RecordingWatch(const std::shared_ptr<Recording>& use) : recording(use) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
        {
            if (!out)
                return E_POINTER;
            *out = nullptr;
            if (iid != __uuidof(IUnknown))
                return E_NOINTERFACE;
            *out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const auto remaining = --references;
            if (!remaining)
            {
                if (auto use = recording.lock())
                    use->open = false;
                delete this;
            }
            return remaining;
        }
    };
    GUID watchKey {};
    bool watchKeyValid = SUCCEEDED(CoCreateGuid(&watchKey));
    using Uses = std::vector<std::shared_ptr<Recording>>;
    struct Retired
    {
        Uses uses;
        std::function<void()> destroy;
    };
    Uses recordings;
    Uses currentGeneration;
    std::vector<Retired> retired;
    std::vector<std::shared_ptr<Timeline>> timelines;
    bool collecting = false;
    std::shared_ptr<Impl> abandoned;
    void Collect();
    static bool Complete(const Uses& uses)
    {
        return std::all_of(uses.begin(), uses.end(), [](const auto& use) { return use->Complete(); });
    }
    std::shared_ptr<Timeline> QueueTimeline(ID3D12CommandQueue* queue)
    {
        if (!queue)
            return {};
        for (const auto& timeline : timelines)
            if (timeline->queue.Get() == queue)
                return timeline;
        auto timeline = std::make_shared<Timeline>();
        timeline->queue = queue; // keep queue identity stable for the lifetime of its fence
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&timeline->fence))))
            timeline->failed = true;
        timelines.push_back(timeline);
        return timeline;
    }
};

GpuLifetime::GpuLifetime() : impl(std::make_shared<Impl>()) {}
GpuLifetime::~GpuLifetime()
{
    const auto state = impl;
    std::lock_guard lock(state->mutex);
    state->Collect();
    // Keep unresolved callbacks and their captures, including runtime/queue ownership.
    // Destroying a callback without running it can still release objects the GPU needs.
    if (!state->recordings.empty())
        state->abandoned = state; // Preserve the existing teardown quarantine without allocating.
}
void GpuLifetime::Record(ID3D12GraphicsCommandList* commands)
{
    const auto state = impl;
    std::lock_guard lock(state->mutex);
    if (!commands)
        return;
    commands = Identity(commands);
    state->Collect();
    for (const auto& use : state->recordings)
        if (use->open && use->commands == commands)
        {
            if (std::find(state->currentGeneration.begin(), state->currentGeneration.end(), use) ==
                state->currentGeneration.end())
                state->currentGeneration.push_back(use);
            return;
        }
    auto use = std::make_shared<Impl::Recording>();
    use->commands = commands;
    if (state->watchKeyValid)
    {
        auto* watch = new Impl::RecordingWatch(use);
        if (FAILED(commands->SetPrivateDataInterface(state->watchKey, watch)))
            watch->recording.reset(); // Failure must not pretend the recording was discarded.
        watch->Release();
    }
    state->currentGeneration.push_back(use);
    state->recordings.push_back(std::move(use));
}
std::function<bool()> GpuLifetime::CompletionProbe(ID3D12GraphicsCommandList* commands)
{
    const auto state = impl;
    std::lock_guard lock(state->mutex);
    commands = Identity(commands);
    for (const auto& use : state->recordings)
        if (use->open && use->commands == commands)
            return [state, use]
            {
                std::lock_guard lock(state->mutex);
                return !use->completions.empty() && use->Finished();
            };
    return [] { return false; };
}
GpuSubmission GpuLifetime::BeginSubmission(UINT count, ID3D12CommandList* const* lists)
{
    const auto state = impl;
    std::lock_guard lock(state->mutex);
    if (state->recordings.empty() || !lists)
        return {};
    try
    {
        Impl::Uses matched;
        for (UINT i = 0; i < count; ++i)
        {
            const auto* commands = Identity(lists[i]);
            for (const auto& use : state->recordings)
                if (use->open && use->commands == commands &&
                    std::find(matched.begin(), matched.end(), use) == matched.end())
                    matched.push_back(use);
        }
        if (matched.empty())
            return {};
        // Build the owning callback before adding pins, so allocation failure cannot
        // leave a half-constructed transaction. Reset cannot collect these generations.
        GpuSubmission submission(
            [state, matched](ID3D12CommandQueue* queue)
            {
                std::lock_guard lock(state->mutex);
                const auto timeline = state->QueueTimeline(Identity(queue));
                // One signal covers every matched list in this actual ExecuteCommandLists notification.
                const bool signalled = timeline && !timeline->failed && timeline->value != UINT64_MAX - 1 &&
                                       SUCCEEDED(timeline->queue->Signal(timeline->fence.Get(), ++timeline->value));
                if (timeline && !signalled)
                    timeline->failed = true;
                for (const auto& use : matched)
                {
                    if (!signalled)
                    {
                        use->signalFailed = true;
                        --use->pendingSubmissions;
                        continue;
                    }
                    use->completions[timeline] = timeline->value;
                    --use->pendingSubmissions;
                }
                state->Collect();
            });
        for (const auto& use : matched)
            ++use->pendingSubmissions;
        return submission;
    }
    catch (...)
    {
        QuarantineSubmission(count, lists); // Before releasing the tracker lock.
        throw;
    }
}
bool GpuLifetime::QuarantineSubmission(UINT count, ID3D12CommandList* const* lists)
{
    const auto state = impl;
    std::lock_guard lock(state->mutex);
    bool matched = false;
    if (lists)
        for (UINT i = 0; i < count; ++i)
        {
            const auto* commands = Identity(lists[i]);
            for (const auto& use : state->recordings)
                if (use->open && use->commands == commands)
                {
                    use->signalFailed = true;
                    matched = true;
                }
        }
    return matched; // No allocations, collection, or retirement callbacks.
}
void GpuLifetime::Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    BeginSubmission(count, lists).Complete(queue);
}
void GpuLifetime::ResetRecording(ID3D12CommandList* commands)
{
    const auto state = impl;
    std::lock_guard lock(state->mutex);
    commands = Identity(commands);
    for (auto& use : state->recordings)
        if (use->commands == commands)
            use->open = false;
    state->Collect();
}
bool GpuLifetime::HasOpenRecording(ID3D12CommandList* commands)
{
    const auto state = impl;
    std::lock_guard lock(state->mutex);
    commands = Identity(commands);
    return commands && std::any_of(state->recordings.begin(), state->recordings.end(),
                                   [&](const auto& use) { return use->open && use->commands == commands; });
}
void GpuLifetime::Retire(std::function<void()> destroy)
{
    const auto state = impl;
    std::lock_guard lock(state->mutex);
    state->retired.push_back({ state->currentGeneration, std::move(destroy) });
    state->Collect();
}
void GpuLifetime::BeginGeneration()
{
    const auto state = impl;
    std::lock_guard lock(state->mutex);
    state->currentGeneration.clear();
    state->Collect();
}
void GpuLifetime::Collect()
{
    const auto state = impl; // A retirement callback may release the wrapper.
    state->Collect();
}
void GpuLifetime::Impl::Collect()
{
    std::lock_guard lock(mutex);
    if (collecting)
        return;
    struct CollectionScope
    {
        bool& active;
        explicit CollectionScope(bool& value) : active(value) { active = true; }
        ~CollectionScope() { active = false; }
    } scope(collecting);
    for (;;)
    {
        std::vector<std::function<void()>> ready;
        std::erase_if(retired,
                      [&](auto& item)
                      {
                          if (!Impl::Complete(item.uses))
                              return false;
                          ready.push_back(std::move(item.destroy));
                          return true;
                      });
        std::erase_if(recordings, [](const auto& use) { return use->Complete(); });
        std::erase_if(currentGeneration, [](const auto& use) { return use->Complete(); });
        if (ready.empty())
            break;
        // NGX destruction can re-enter queue/reset hooks and Retire. No callback may
        // run while a retired/recording vector is being compacted or iterated.
        for (auto& destroy : ready)
            destroy();
    }
}
bool GpuLifetime::GpuComplete()
{
    const auto state = impl;
    std::lock_guard lock(state->mutex);
    return std::all_of(state->recordings.begin(), state->recordings.end(),
                       [](const auto& use) { return use->Finished(); });
}
bool GpuLifetime::Idle()
{
    const auto state = impl;
    std::lock_guard lock(state->mutex);
    state->Collect();
    return !state->collecting && state->recordings.empty();
}
void GpuLifetime::FinishSubmitted()
{
    const auto state = impl;
    std::lock_guard lock(state->mutex);
    for (auto& use : state->recordings)
        if (!use->completions.empty() && use->Finished())
            use->open = false;
    state->Collect();
}
} // namespace DlssNr

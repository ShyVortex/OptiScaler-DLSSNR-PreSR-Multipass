#include "../../OptiScaler/dlssnr/DlssNr_GpuLifetime.cpp"
#include "../../OptiScaler/dlssnr/DlssNr_DescriptorSlots.h"
#include <cstdio>
#include <cstring>
#include <array>
using Microsoft::WRL::ComPtr;
template <class I> class Object : public I
{
    ULONG refs = 1;

  public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override
    {
        if (!out)
            return E_POINTER;
        *out = static_cast<I*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        auto left = --refs;
        if (!left)
            delete this;
        return left;
    }
    virtual ~Object() = default;
};
struct Fence : Object<ID3D12Fence>
{
    UINT64 value = 0;
    UINT64 GetCompletedValue() override { return value; }
};
struct Device : Object<ID3D12Device>
{
    HRESULT CreateFence(UINT64 initial, D3D12_FENCE_FLAGS, REFIID, void** out) override
    {
        auto* f = new Fence;
        f->value = initial;
        *out = static_cast<ID3D12Fence*>(f);
        return S_OK;
    }
};
struct Commands : Object<ID3D12GraphicsCommandList>
{
    std::vector<std::pair<GUID, ComPtr<IUnknown>>> watches;
    HRESULT SetPrivateDataInterface(REFGUID key, const IUnknown* value) override
    {
        for (auto& entry : watches)
            if (!std::memcmp(&entry.first, &key, sizeof key))
            {
                entry.second = const_cast<IUnknown*>(value);
                return S_OK;
            }
        watches.push_back({ key, const_cast<IUnknown*>(value) });
        return S_OK;
    }
};
struct Queue : Object<ID3D12CommandQueue>
{
    ComPtr<ID3D12Device> device;
    std::vector<std::pair<ComPtr<ID3D12Fence>, UINT64>> signals;
    bool fail = false;
    explicit Queue(ID3D12Device* d) : device(d) {}
    HRESULT GetDevice(REFIID, void** out) override
    {
        *out = device.Get();
        device->AddRef();
        return S_OK;
    }
    HRESULT Signal(ID3D12Fence* f, UINT64 value) override
    {
        if (fail)
            return E_FAIL;
        signals.push_back({ f, value });
        return S_OK;
    }
    void Complete(bool removed = false)
    {
        for (auto& s : signals)
            static_cast<Fence*>(s.first.Get())->value = removed ? UINT64_MAX : s.second;
    }
};
struct Fixture
{
    ComPtr<Device> device;
    ComPtr<Commands> commands, other;
    ComPtr<Queue> q, q2;
    Fixture()
    {
        device.Attach(new Device);
        commands.Attach(new Commands);
        other.Attach(new Commands);
        q.Attach(new Queue(device.Get()));
        q2.Attach(new Queue(device.Get()));
    }
};

bool bounded96()
{
    Fixture f;
    DlssNr::DescriptorSlots<96> slots;
    std::array<bool, 96> seen {};
    for (unsigned i = 0; i < 96; ++i)
    {
        auto slot = slots.Acquire(f.commands.Get());
        if (!slot || *slot >= 96 || seen[*slot])
            return false;
        seen[*slot] = true;
    }
    return !slots.Acquire(f.commands.Get());
}
bool resetAndCompletion()
{
    Fixture f;
    DlssNr::DescriptorSlots<1> slots;
    if (!slots.Acquire(f.commands.Get()))
        return false;
    ID3D12CommandList* lists[] { f.commands.Get() };
    auto old = slots.BeginSubmission(1, lists);
    slots.ResetRecording(f.commands.Get());
    if (slots.Acquire(f.other.Get()))
        return false;
    old.Complete(f.q.Get());
    if (slots.Acquire(f.other.Get()))
        return false;
    f.q->Complete();
    return slots.Acquire(f.other.Get()).has_value();
}
bool handles()
{
    Fixture f;
    DlssNr::DescriptorSlots<96> slots;
    uint32_t handle = UINT32_MAX;
    auto initial = slots.Acquire(f.commands.Get(), &handle);
    if (!initial || handle != UINT32_MAX || !slots.PublishImmutable(f.commands.Get(), *initial, handle))
        return false;
    if (!initial || slots.Acquire(f.commands.Get(), &handle) != initial)
        return false;
    auto foreign = handle;
    if (slots.Acquire(f.other.Get(), &foreign))
        return false;
    uint32_t invalidIndex = (1u << 7) | 127u;
    if (slots.Acquire(f.commands.Get(), &invalidIndex))
        return false;
    slots.ResetRecording(f.commands.Get());
    if (slots.Acquire(f.commands.Get(), &handle))
        return false;
    DlssNr::DescriptorSlots<1> single;
    uint32_t old = UINT32_MAX, current = UINT32_MAX;
    auto oldSlot = single.Acquire(f.commands.Get(), &old);
    if (!oldSlot || !single.PublishImmutable(f.commands.Get(), *oldSlot, old))
        return false;
    single.ResetRecording(f.commands.Get());
    auto currentSlot = single.Acquire(f.commands.Get(), &current);
    if (!currentSlot || !single.PublishImmutable(f.commands.Get(), *currentSlot, current))
        return false;
    return old != current && !single.Acquire(f.commands.Get(), &old);
}
bool immutablePublicationIsTransactional()
{
    Fixture f;
    DlssNr::DescriptorSlots<2> slots;
    uint32_t handle = UINT32_MAX;
    auto failedInitialization = slots.Acquire(f.commands.Get(), &handle);
    // DispatchPass has not initialized the descriptor heap yet. A failure at
    // this point must leave the caller's handle unpublished so a retry cannot
    // take the immutable reuse path.
    if (!failedInitialization || handle != UINT32_MAX)
        return false;
    auto retry = slots.Acquire(f.commands.Get(), &handle);
    if (!retry || *retry == *failedInitialization || handle != UINT32_MAX)
        return false;
    if (!slots.PublishImmutable(f.commands.Get(), *retry, handle))
        return false;
    return slots.Acquire(f.commands.Get(), &handle) == retry;
}
bool replay()
{
    Fixture f;
    DlssNr::DescriptorSlots<1> slots;
    slots.Acquire(f.commands.Get());
    ID3D12CommandList* lists[] { f.commands.Get() };
    slots.BeginSubmission(1, lists).Complete(f.q.Get());
    f.q->Complete();
    if (slots.Acquire(f.other.Get()))
        return false;
    auto later = slots.BeginSubmission(1, lists);
    slots.ResetRecording(f.commands.Get());
    later.Complete(f.q2.Get());
    if (slots.Acquire(f.other.Get()))
        return false;
    f.q2->Complete();
    return slots.Acquire(f.other.Get()).has_value();
}
bool failure(unsigned mode)
{
    Fixture f;
    DlssNr::DescriptorSlots<1> slots;
    slots.Acquire(f.commands.Get());
    ID3D12CommandList* lists[] { f.commands.Get() };
    {
        auto pending = slots.BeginSubmission(1, lists);
        slots.ResetRecording(f.commands.Get());
        if (mode != 2)
        {
            f.q->fail = mode == 0;
            pending.Complete(f.q.Get());
            if (mode == 1)
                f.q->Complete(true);
        }
    }
    return !slots.Acquire(f.other.Get()) && !slots.Idle();
}
bool destroyedAddressReuse()
{
    Fixture f;
    DlssNr::DescriptorSlots<2> slots;
    uint32_t stale = UINT32_MAX;
    auto first = slots.Acquire(f.commands.Get(), &stale);
    if (!first || !slots.PublishImmutable(f.commands.Get(), *first, stale))
        return false;
    ID3D12CommandList* lists[] { f.commands.Get() };
    auto old = slots.BeginSubmission(1, lists);
    old.Complete(f.q.Get());
    // Releasing the private-data interfaces invokes the real RecordingWatch
    // destruction notification, as destruction of a command-list object does.
    // Reusing this fake's address then models a newly allocated list at that address.
    f.commands->watches.clear();
    const bool rejectedStale = !slots.Acquire(f.commands.Get(), &stale);
    auto second = slots.Acquire(f.commands.Get());
    if (!first || !second || *first == *second)
        return false;
    auto newer = slots.BeginSubmission(1, lists);
    slots.ResetRecording(f.commands.Get());
    newer.Complete(f.q2.Get());
    f.q->Complete();
    auto reclaimedOld = slots.Acquire(f.other.Get());
    const bool retainedNew = !slots.Acquire(f.other.Get());
    f.q2->Complete();
    return rejectedStale && reclaimedOld && *reclaimedOld == *first && retainedNew &&
           slots.Acquire(f.other.Get()).has_value();
}
int main()
{
    bool passed = true;
    auto check = [&](const char* name, bool ok)
    {
        std::printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
        passed &= ok;
    };
    check("97th allocation does not overwrite 96 pending slots", bounded96());
    check("reset and actual fence completion are both required", resetAndCompletion());
    check("immutable reuse rejects foreign, invalid and stale handles", handles());
    check("immutable handles publish only after descriptor initialization", immutablePublicationIsTransactional());
    check("replay retains every queue completion", replay());
    check("failed Signal quarantines slots", failure(0));
    check("device removal quarantines slots", failure(1));
    check("abandoned submission quarantines slots", failure(2));
    check("destroyed command address does not alias a new recording", destroyedAddressReuse());
    return passed ? 0 : 1;
}

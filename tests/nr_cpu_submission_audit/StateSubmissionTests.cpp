// Mechanically extracted production State submission method; actual tracker/token.
// Only surrounding owner/COM dependencies are CPU fixtures. No graphics DLL is loaded.
#include "../nr_cpu_timing_audit/CpuD3d12.h"
#include "../../OptiScaler/dlssnr/DlssNr_GpuLifetime.cpp"
#include "../../OptiScaler/dlssnr/DlssNr_PrivateCreation.h"
#include "../../OptiScaler/shaders/dlssnr/DlssNr_GpuTime.h"
#include "../../OptiScaler/dlssnr/DlssNr_FinishedReady.h"
#include "../../OptiScaler/dlssnr/DlssNr_Placement.h"
#include <cstdlib>
#include <cstdio>
#include <new>
#include <string>

// External configuration/frame/logging boundary for the production selection region.
struct CpuOption
{
    bool value;
    bool value_or_default() const { return value; }
};
struct Config
{
    CpuOption DlssNrRunBeforeSr { true }, DlssNrDeferredDlss { false }, DlssNrResidualAcrossRr { false };
    CpuOption DlssNrHoldFrame { true };
    static Config* Instance()
    {
        static Config value;
        return &value;
    }
};
struct State
{
    uint64_t frameCount = 0;
    static State& Instance()
    {
        static State value;
        return value;
    }
};
#define LOG_INFO(...) ((void) 0)

static thread_local int failAllocationAfter = -1;
void* operator new(std::size_t size)
{
    if (failAllocationAfter == 0)
    {
        failAllocationAfter = -1; // fail one allocation, allow exception cleanup
        throw std::bad_alloc();
    }
    if (failAllocationAfter > 0)
        --failAllocationAfter;
    if (auto* memory = std::malloc(size ? size : 1))
        return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

using Microsoft::WRL::ComPtr;
template <class Interface> struct CpuObject : Interface
{
    ULONG references = 1;
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
    explicit CpuReadback(UINT64 bytes) : data(static_cast<size_t>(bytes / sizeof(UINT64)), 0) {}
    HRESULT Map(UINT, const D3D12_RANGE*, void** out) override
    {
        *out = data.data();
        return S_OK;
    }
    void Unmap(UINT, const D3D12_RANGE*) override {}
};
struct CpuDevice final : CpuObject<ID3D12Device>
{
    bool enableTiming = false;
    HRESULT CreateQueryHeap(const D3D12_QUERY_HEAP_DESC*, REFIID, void** out) override
    {
        if (!enableTiming)
            return E_NOTIMPL;
        *out = static_cast<ID3D12QueryHeap*>(new CpuQueryHeap);
        return S_OK;
    }
    HRESULT CreateCommittedResource(const CD3DX12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const CD3DX12_RESOURCE_DESC* desc,
                                    D3D12_RESOURCE_STATES, const void*, REFIID, void** out) override
    {
        if (!enableTiming)
            return E_NOTIMPL;
        *out = static_cast<ID3D12Resource*>(new CpuReadback(desc->Width));
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
struct CpuQueue final : CpuObject<ID3D12CommandQueue>
{
    struct SignalPoint
    {
        ComPtr<ID3D12Fence> fence;
        UINT64 value;
    };
    ComPtr<ID3D12Device> device;
    std::vector<SignalPoint> signals;
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
    void Complete(bool removed = false)
    {
        for (const auto& signal : signals)
            static_cast<CpuFence*>(signal.fence.Get())->completed = removed ? UINT64_MAX : signal.value;
        signals.clear();
    }
};
struct CpuCommands final : ID3D12GraphicsCommandList
{
    ULONG references = 1;
    bool failClose = false;
    HRESULT Close() { return failClose ? E_FAIL : S_OK; }
    std::vector<std::pair<GUID, ComPtr<IUnknown>>> watches;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override
    {
        if (!out)
            return E_POINTER;
        *out = static_cast<ID3D12GraphicsCommandList*>(this);
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
    void EndQuery(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT) override {}
    void ResolveQueryData(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT, UINT, ID3D12Resource* resource,
                          UINT64 offset) override
    {
        // Hand-authored timestamp boundary; the actual timer still gates reads by its fence.
        auto& data = static_cast<CpuReadback*>(resource)->data;
        data[static_cast<size_t>(offset / sizeof(UINT64))] = 1000;
        data[static_cast<size_t>(offset / sizeof(UINT64)) + 1] = 2000;
    }
};

struct DlssNr_Dx12
{
    struct State
    {
        std::recursive_mutex mutex;
        DlssNr::GpuLifetime lifetime;
        struct DeferredSr
        {
            struct Generation
            {
                DlssNr::PrivateFeatureCreation creation;
            };
            std::unique_ptr<Generation> current;
            std::vector<Generation*> retiredGenerations;
            DlssNr::GpuLifetime lifetime;
        } deferredSr;
        struct DenoiseFirst
        {
            struct Generation
            {
                DlssNr::PrivateFeatureCreation denoiserCreation, enlargerCreation;
                std::unique_ptr<DlssNrGpuTime> denoiseTime, enlargeTime;
            };
            std::unique_ptr<Generation> current;
            std::vector<Generation*> retiredGenerations;
            DlssNr::GpuLifetime lifetime;
        } denoiseFirst;
        DlssNr::GpuLifetime captureFrames;
        std::unique_ptr<DlssNrGpuTime> gpuTime, ngxTime;
        unsigned pendingSubmissions = 0;
        struct Nr
        {
            std::array<DlssNr::GpuLifetime, 1> models;
            bool heldActive = true;
        } nr;
        struct Enlarger
        {
            DlssNr::GpuLifetime lifetime;
            ID3D12CommandList* creation = nullptr;
            ComPtr<ID3D12CommandQueue> queue;
            bool submitted = false, failed = false;
            unsigned pendingSubmissions = 0;
        };
        std::unique_ptr<Enlarger> enlarger;
        std::string enlargementStatus;
        std::vector<std::unique_ptr<Enlarger>> retiredEnlargers;
        bool collectingEnlargers = false;
        struct Hold
        {
            ID3D12CommandList* captureCommands = nullptr;
            uint64_t generation = 1;
            bool active = true;
        } inputHold;
        std::vector<uint64_t> pendingHoldGenerations;
        struct LateContext
        {
            State& owner;
            explicit LateContext(State& value) : owner(value) {}
            struct Slot
            {
                DlssNr::GpuLifetime producerLifetime;
                bool pending = false, submitted = false, residualOnly = true, quarantined = false;
                ID3D12CommandList* producer = nullptr;
                ComPtr<ID3D12GraphicsCommandList> commands;
                ComPtr<ID3D12CommandQueue> producerQueue;
                ComPtr<ID3D12Fence> fence;
                struct Frame
                {
                    unsigned OutputWidth = 1, OutputHeight = 1;
                    uint64_t SubmissionEpoch = 0;
                } frame;
                uint64_t ready = 1, done = 0, serial = 0;
                unsigned pendingSubmissions = 0;
            };
            std::array<Slot, 4> slots;
            std::atomic_bool tracking { true };
            ComPtr<ID3D12CommandQueue> producerQueue;
            uint64_t serial = 0;
            bool reset = false;
            bool reportedQueueDelay = false, heldValid = false, heldFailed = false;
            uint64_t heldGeneration = 0, heldSlotSerial = 0;
            Slot* heldSlot = nullptr;
            struct Bridge
            {
                bool Drain() { return true; }
                bool Idle() { return true; }
            } dx11;
            std::string status;
            void Say(const char* message) { status = message; }
            void Arm(Slot&, ID3D12GraphicsCommandList*);
            void Cancel();
            bool Finished(Slot&);
            void DiscardUnsubmitted(Slot&);
            Slot* SelectAcquire()
            {
#include "late-acquire-under-test.inc"
                return next;
            }
        } late { *this };
        void CollectEnlargers();
        void FinishedPictureResetCommandList(ID3D12CommandList*);
        bool WaitForFinishedPicture();
        bool CloseComposition(CpuCommands* cmd, LateContext::Slot& slot, bool holdFinished = false)
        {
#include "late-close-under-test.inc"
            return true;
        }
        LateContext::Slot* SelectComposition(ID3D12CommandQueue* realQueue, bool gameFrameHandoff = false)
        {
            struct
            {
                unsigned Width = 1, Height = 1;
            } desc;
#include "late-selection-under-test.inc"
            return latest;
        }
        DlssNr::GpuSubmission BeginFinishedPictureSubmission(UINT, ID3D12CommandList* const*);
        void QuarantineFinishedPictureSubmission(UINT, ID3D12CommandList* const*);
    };
    State* _state = nullptr;
    struct DescriptorOwner
    {
        bool Idle() { return true; }
    } _descriptorSlots;
    bool ReadyToDestroy();
};

#include "state-method-under-test.inc"
#include "state-quarantine-under-test.inc"
#include "enlarger-collector-under-test.inc"
#include "late-Arm-under-test.inc"
#include "late-Cancel-under-test.inc"
#include "late-Finished-under-test.inc"
#include "late-DiscardUnsubmitted-under-test.inc"
#include "state-reset-under-test.inc"
#include "state-wait-under-test.inc"
#include "owner-ready-under-test.inc"

struct LateFixture
{
    ComPtr<CpuDevice> device;
    ComPtr<CpuQueue> first, second;
    ComPtr<CpuCommands> commands;
    DlssNr_Dx12::State state;
    LateFixture()
    {
        device.Attach(new CpuDevice);
        first.Attach(new CpuQueue(device.Get()));
        second.Attach(new CpuQueue(device.Get()));
        commands.Attach(new CpuCommands);
        state.late.slots[0].fence.Attach(new CpuFence);
        state.late.Arm(state.late.slots[0], commands.Get());
    }
    DlssNr::GpuSubmission Begin()
    {
        ID3D12CommandList* lists[] { commands.Get() };
        return state.BeginFinishedPictureSubmission(1, lists);
    }
    bool Finished() { return state.late.Finished(state.late.slots[0]); }
    void Reset() { state.FinishedPictureResetCommandList(commands.Get()); }
};

bool LateCopyRetainsCompletedReplayableProducer()
{
    LateFixture f;
    f.Begin().Complete(f.first.Get());
    f.first->Complete();
    f.state.late.Cancel();
    const bool retained = !f.Finished();
    f.Reset();
    const bool released = f.Finished();
    std::printf("late copy retains completed replayable producer until reset: %s\n",
                retained && released ? "PASS" : "FAIL");
    return retained && released;
}

bool CancelledLateCopyWaitsForReplayOnEveryQueue()
{
    LateFixture f;
    f.Begin().Complete(f.first.Get());
    f.first->Complete();
    f.state.late.Cancel();
    auto firstReplay = f.Begin();
    auto secondReplay = f.Begin();
    f.Reset(); // Captured executions still own the old recording after this reset.
    const bool pinned = !f.Finished();
    secondReplay.Complete(f.second.Get());
    f.second->Complete();
    const bool otherPending = !f.Finished();
    firstReplay.Complete(f.first.Get());
    const bool firstPending = !f.Finished();
    f.first->Complete();
    const bool finished = f.Finished();
    const bool ok = pinned && otherPending && firstPending && finished;
    std::printf("cancelled late copy waits for reset-racing replay on every queue: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

bool LateCopyFailureStaysQuarantined(unsigned failure)
{
    LateFixture f;
    f.Begin().Complete(f.first.Get());
    f.first->Complete();
    f.state.late.Cancel();
    auto replay = f.Begin();
    f.Reset();
    f.second->failSignal = failure == 1;
    replay.Complete(failure == 0 ? nullptr : f.second.Get());
    f.second->Complete(failure == 2);
    const bool retained = !f.Finished() && f.state.late.SelectAcquire() != &f.state.late.slots[0];
    std::printf("late copy replay failure %u remains quarantined: %s\n", failure, retained ? "PASS" : "FAIL");
    return retained;
}

bool DestroyedLateProducerWaitsForSubmittedCopies()
{
    LateFixture f;
    f.Begin().Complete(f.first.Get());
    f.first->Complete();
    f.state.late.Cancel();
    f.Begin().Complete(f.second.Get());
    f.commands.Reset();
    const bool retained = !f.Finished();
    f.second->Complete();
    const bool released = f.Finished();
    std::printf("destroyed late producer waits for submitted replay: %s\n", retained && released ? "PASS" : "FAIL");
    return retained && released;
}

bool LateCompositionSkipsReplayableInput(bool held)
{
    LateFixture f;
    f.Begin().Complete(f.first.Get());
    f.first->Complete();
    if (held)
    {
        f.state.late.Cancel();
        f.state.late.heldValid = true;
        f.state.late.heldGeneration = f.state.inputHold.generation;
        f.state.late.heldSlot = &f.state.late.slots[0];
        f.state.late.heldSlotSerial = f.state.late.slots[0].serial;
    }
    const bool blocked = f.state.SelectComposition(f.first.Get()) == nullptr && !f.state.late.status.empty();
    f.Reset();
    const bool ready = f.state.SelectComposition(f.first.Get()) == &f.state.late.slots[0];
    std::printf("late composition holds replayable input (held=%d): %s\n", held, blocked && ready ? "PASS" : "FAIL");
    return blocked && ready;
}

bool LateDrainCannotUseFirstFenceToCompleteReplay()
{
    LateFixture f;
    f.Begin().Complete(f.first.Get());
    f.first->Complete();
    const bool openBlocked = !f.state.WaitForFinishedPicture();
    auto replay = f.Begin();
    f.Reset();
    const bool pendingBlocked = !f.state.WaitForFinishedPicture();
    replay.Complete(f.second.Get());
    const bool gpuBlocked = !f.state.WaitForFinishedPicture();
    f.second->Complete();
    const bool drained = f.state.WaitForFinishedPicture();
    const bool ok = openBlocked && pendingBlocked && gpuBlocked && drained;
    std::printf("late drain rejects replayable, pending and unfinished replay: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

bool DiscardedLateRecordingDoesNotRequireUnpromisedFence()
{
    LateFixture f;
    DlssNr_Dx12 owner;
    owner._state = &f.state;
    const bool retained = !owner.ReadyToDestroy();
    f.commands.Reset(); // Destruction closes an unsubmitted recording; there can be no GPU signal.
    const bool released = owner.ReadyToDestroy();
    std::printf("discarded late recording does not require an unpromised fence: %s\n",
                retained && released ? "PASS" : "FAIL");
    return retained && released;
}

bool LateOwnerRetainsReplayUntilEveryQueueCompletes()
{
    LateFixture f;
    DlssNr_Dx12 owner;
    owner._state = &f.state;
    f.Begin().Complete(f.first.Get());
    f.first->Complete();
    auto replay = f.Begin();
    f.Reset();
    const bool pending = !owner.ReadyToDestroy();
    replay.Complete(f.second.Get());
    const bool unfinished = !owner.ReadyToDestroy();
    f.second->Complete();
    const bool released = owner.ReadyToDestroy();
    const bool ok = pending && unfinished && released;
    std::printf("late owner retains reset-racing replay through GPU completion: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

bool DestroyedUnsubmittedCopiesAreReusable()
{
    LateFixture f;
    for (size_t i = 1; i < f.state.late.slots.size(); ++i)
    {
        f.state.late.slots[i].fence.Attach(new CpuFence);
        f.state.late.Arm(f.state.late.slots[i], f.commands.Get());
    }
    const bool liveRetained = f.state.late.SelectAcquire() == nullptr;
    f.commands.Reset();
    f.commands.Attach(new CpuCommands);
    bool recycled = true;
    for (auto& slot : f.state.late.slots)
    {
        auto* next = f.state.late.SelectAcquire();
        recycled &= next == &slot && !slot.pending && slot.done == 0;
        if (next)
            f.state.late.Arm(*next, f.commands.Get());
    }
    const bool fullAgain = f.state.late.SelectAcquire() == nullptr;
    const bool ok = liveRetained && recycled && fullAgain;
    std::printf("active owner reclaims all destroyed unsubmitted slots but retains live recordings: %s\n",
                ok ? "PASS" : "FAIL");
    return ok;
}

bool LateMetadataRequiresCapturedRecording(bool reusedAddress)
{
    LateFixture f;
    if (reusedAddress)
    {
        // End the actual object lifetime (and COM watches), then reuse exactly its storage.
        auto* address = f.commands.Detach();
        address->~CpuCommands();
        f.commands.Attach(new (address) CpuCommands);
    }
    else
    {
        // A stale metadata identity is insufficient even when no recording was registered.
        f.state.lifetime.ResetRecording(f.commands.Get());
        f.state.late.slots[0].producerLifetime.ResetRecording(f.commands.Get());
    }
    auto token = f.Begin();
    const bool ignored = !token && !f.state.late.slots[0].pendingSubmissions;
    token.Complete(f.first.Get());
    const bool untouched = !f.state.late.slots[0].submitted;
    std::printf("late metadata requires a captured recording (reused-address=%d): %s\n", reusedAddress,
                ignored && untouched ? "PASS" : "FAIL");
    return ignored && untouched;
}

bool FailedLateCloseIsNotDiscarded()
{
    LateFixture f;
    f.Begin().Complete(f.first.Get());
    f.first->Complete();
    f.Reset();
    f.commands->failClose = true;
    const bool failed = !f.state.CloseComposition(f.commands.Get(), f.state.late.slots[0]);
    f.Reset();
    const bool retained = f.state.late.SelectAcquire() != &f.state.late.slots[0];
    std::printf("failed late command close remains quarantined during reset/acquire: %s\n",
                failed && retained ? "PASS" : "FAIL");
    return failed && retained;
}

bool InvalidInitializationDoesNotBindReusedPointer(bool failed, bool recorded)
{
    ComPtr<CpuCommands> commands;
    commands.Attach(new CpuCommands);
    DlssNr_Dx12::State state;
    state.late.tracking = false;
    state.enlarger = std::make_unique<DlssNr_Dx12::State::Enlarger>();
    state.enlarger->creation = commands.Get();
    state.enlarger->failed = failed;
    if (recorded)
        state.enlarger->lifetime.Record(commands.Get());
    ID3D12CommandList* lists[] { commands.Get() };
    auto token = state.BeginFinishedPictureSubmission(1, lists);
    const bool ignored = state.enlarger->pendingSubmissions == 0;
    token.Complete(nullptr);
    std::printf("invalid initialization ignored (failed=%d recorded=%d): %s\n", failed, recorded,
                ignored ? "PASS" : "FAIL");
    return ignored;
}

bool RetiredEnlargerWaitsForMetadataPins()
{
    DlssNr_Dx12::State state;
    auto retired = std::make_unique<DlssNr_Dx12::State::Enlarger>();
    // The metadata token's raw pointer needs protection even when its tracker is idle.
    retired->pendingSubmissions = 1;
    state.retiredEnlargers.push_back(std::move(retired));
    state.CollectEnlargers();
    const bool retained = state.retiredEnlargers.size() == 1;
    if (retained)
        state.retiredEnlargers[0]->pendingSubmissions = 0;
    state.CollectEnlargers();
    const bool collected = state.retiredEnlargers.empty();
    std::printf("enlarger collector respects metadata pin then reclaims: %s\n",
                retained && collected ? "PASS" : "FAIL");
    return retained && collected;
}

bool CollectorAllocationFailureRestoresReentrancyGuard()
{
    DlssNr_Dx12::State state;
    state.retiredEnlargers.push_back(std::make_unique<DlssNr_Dx12::State::Enlarger>());
    bool threw = false;
    failAllocationAfter = 0;
    try
    {
        state.CollectEnlargers();
    }
    catch (const std::bad_alloc&)
    {
        threw = true;
    }
    failAllocationAfter = -1;
    const bool preserved = threw && !state.collectingEnlargers && state.retiredEnlargers.size() == 1 &&
                           state.retiredEnlargers[0] != nullptr;
    state.CollectEnlargers();
    const bool reclaimed = state.retiredEnlargers.empty() && !state.collectingEnlargers;
    std::printf("collector allocation failure restores guard/preserves item/retries: %s\n",
                preserved && reclaimed ? "PASS" : "FAIL");
    return preserved && reclaimed;
}

bool CollectorFaultSweepDoesNotLeaveMovedFromEntries()
{
    unsigned failures = 0, injected = 0;
    bool reachedSuccess = false;
    for (int allocation = 0; allocation < 16; ++allocation)
    {
        DlssNr_Dx12::State state;
        for (unsigned i = 0; i < 3; ++i)
            state.retiredEnlargers.push_back(std::make_unique<DlssNr_Dx12::State::Enlarger>());
        bool threw = false;
        failAllocationAfter = allocation;
        try
        {
            state.CollectEnlargers();
        }
        catch (const std::bad_alloc&)
        {
            threw = true;
            ++injected;
        }
        failAllocationAfter = -1;
        const bool valid =
            !state.collectingEnlargers && std::all_of(state.retiredEnlargers.begin(), state.retiredEnlargers.end(),
                                                      [](const auto& item) { return item != nullptr; });
        if (!valid)
            ++failures;
        else
        {
            state.CollectEnlargers();
            if (!state.retiredEnlargers.empty())
                ++failures;
        }
        if (!threw)
        {
            reachedSuccess = true;
            break;
        }
    }
    const bool ok = reachedSuccess && injected && !failures;
    std::printf("collector multi-item fault sweep leaves no moved-from entries: %u sites, %s\n", injected,
                ok ? "PASS" : "FAIL");
    return ok;
}

bool PreparationAllocationFailureRetainsRecording()
{
    unsigned injected = 0, failures = 0;
    for (int allocation = 0; allocation < 32; ++allocation)
    {
        ComPtr<CpuCommands> commands;
        commands.Attach(new CpuCommands);
        DlssNr::GpuLifetime lifetime;
        auto released = std::make_shared<bool>(false);
        lifetime.Record(commands.Get());
        lifetime.Retire([released] { *released = true; });
        ID3D12CommandList* lists[] { commands.Get() };
        bool threw = false;
        failAllocationAfter = allocation;
        try
        {
            auto submission = lifetime.BeginSubmission(1, lists);
        }
        catch (const std::bad_alloc&)
        {
            threw = true;
            ++injected;
        }
        failAllocationAfter = -1;
        if (!threw)
            break;
        // The hook still executes the application's batch. A reset during that
        // Execute must not release ownership just because no token was returned.
        lifetime.ResetRecording(commands.Get());
        lifetime.Collect();
        if (*released || lifetime.Idle())
            ++failures;
    }
    std::printf("tracker preparation allocation failure retains submitted ownership: %u sites, %u failures\n", injected,
                failures);
    return injected > 0 && !failures;
}

bool FailedOwnerPreparationQuarantinesUnvisitedChildren()
{
    LateFixture f;
    auto& state = f.state;
    auto* commands = f.commands.Get();
    state.lifetime.Record(commands);
    state.deferredSr.lifetime.Record(commands);
    state.captureFrames.Record(commands);
    state.nr.models[0].Record(commands);
    state.enlarger = std::make_unique<DlssNr_Dx12::State::Enlarger>();
    state.enlarger->creation = commands;
    state.enlarger->lifetime.Record(commands);
    state.inputHold.captureCommands = commands;
    auto& slot = state.late.slots[0];
    state.late.Arm(slot, commands);
    ID3D12CommandList* lists[] { commands };
    failAllocationAfter = 0; // Failure before any metadata pins exist.
    bool threw = false;
    try
    {
        auto submission = state.BeginFinishedPictureSubmission(1, lists);
    }
    catch (const std::bad_alloc&)
    {
        threw = true;
    }
    failAllocationAfter = -1;
    state.QuarantineFinishedPictureSubmission(1, lists);
    state.FinishedPictureResetCommandList(commands);
    const bool ok = threw && !state.lifetime.Idle() && !state.deferredSr.lifetime.Idle() &&
                    !state.captureFrames.Idle() && !state.nr.models[0].Idle() && !state.enlarger->lifetime.Idle() &&
                    state.enlarger->failed && !slot.producerLifetime.Idle() && slot.quarantined && !slot.pending &&
                    !state.inputHold.active && !state.nr.heldActive;
    std::printf("failed owner preparation quarantines unvisited child storage and metadata: %s\n",
                ok ? "PASS" : "FAIL");
    return ok;
}

// Exercise the actual owner aggregation/reset/quarantine methods with all three
// creation gates, including old generations still reachable through the registry.
struct PrivateCreationFixture
{
    using Deferred = DlssNr_Dx12::State::DeferredSr::Generation;
    using Denoise = DlssNr_Dx12::State::DenoiseFirst::Generation;
    ComPtr<CpuDevice> device;
    ComPtr<CpuQueue> queue;
    ComPtr<CpuCommands> commands;
    std::unique_ptr<Deferred> oldDeferred;
    std::unique_ptr<Denoise> oldDenoise;
    DlssNr_Dx12::State state;
    PrivateCreationFixture()
    {
        device.Attach(new CpuDevice);
        device->enableTiming = true;
        queue.Attach(new CpuQueue(device.Get()));
        commands.Attach(new CpuCommands);
        state.deferredSr.current = std::make_unique<Deferred>();
        state.denoiseFirst.current = std::make_unique<Denoise>();
        oldDeferred = std::make_unique<Deferred>();
        oldDenoise = std::make_unique<Denoise>();
        state.deferredSr.retiredGenerations.push_back(oldDeferred.get());
        state.denoiseFirst.retiredGenerations.push_back(oldDenoise.get());
        state.lifetime.Record(commands.Get());
        state.deferredSr.lifetime.Record(commands.Get());
        state.denoiseFirst.lifetime.Record(commands.Get());
        ForEach([&](auto& creation) { creation.Record(commands.Get()); });
        state.denoiseFirst.current->denoiseTime = std::make_unique<DlssNrGpuTime>(device.Get());
        state.denoiseFirst.current->enlargeTime = std::make_unique<DlssNrGpuTime>(device.Get());
        oldDenoise->denoiseTime = std::make_unique<DlssNrGpuTime>(device.Get());
        oldDenoise->enlargeTime = std::make_unique<DlssNrGpuTime>(device.Get());
        ForEachTimer(
            [&](auto& timer)
            {
                timer.Start(commands.Get());
                timer.End(commands.Get());
            });
    }
    template <class Action> void ForEach(Action action)
    {
        action(state.deferredSr.current->creation);
        action(oldDeferred->creation);
        action(state.denoiseFirst.current->denoiserCreation);
        action(state.denoiseFirst.current->enlargerCreation);
        action(oldDenoise->denoiserCreation);
        action(oldDenoise->enlargerCreation);
    }
    bool AllReady()
    {
        bool ready = true;
        ForEach([&](auto& creation) { ready &= creation.Ready(); });
        return ready;
    }
    template <class Action> void ForEachTimer(Action action)
    {
        action(*state.denoiseFirst.current->denoiseTime);
        action(*state.denoiseFirst.current->enlargeTime);
        action(*oldDenoise->denoiseTime);
        action(*oldDenoise->enlargeTime);
    }
    bool AllTimersReady()
    {
        bool ready = true;
        ForEachTimer([&](auto& timer) { ready &= timer.ReadGpuTime() == 1.0; });
        return ready;
    }
    bool AllQuarantined()
    {
        bool quarantined = true;
        ForEach([&](auto& creation) { quarantined &= !creation.Ready() && !creation.Discarded() && !creation.Idle(); });
        ForEachTimer([&](auto& timer) { quarantined &= !timer.ReadGpuTime(); });
        return quarantined && !state.lifetime.Idle() && !state.deferredSr.lifetime.Idle() &&
               !state.denoiseFirst.lifetime.Idle();
    }
    DlssNr::GpuSubmission Begin()
    {
        ID3D12CommandList* lists[] { commands.Get() };
        return state.BeginFinishedPictureSubmission(1, lists);
    }
    void Quarantine()
    {
        ID3D12CommandList* lists[] { commands.Get() };
        state.QuarantineFinishedPictureSubmission(1, lists);
    }
    void Reset() { state.FinishedPictureResetCommandList(commands.Get()); }
};

bool CurrentAndRetiredCreationNotificationsAreCoupled()
{
    PrivateCreationFixture f;
    auto submission = f.Begin();
    f.Reset(); // Represents Reset after real Execute but before its notification.
    const bool pinned = f.AllQuarantined();
    submission.Complete(f.queue.Get());
    const bool incomplete = f.AllQuarantined() && !f.AllReady();
    f.queue->Complete();
    const bool ready = f.AllReady() && f.AllTimersReady() && f.state.pendingSubmissions == 0 &&
                       f.state.lifetime.Idle() && f.state.deferredSr.lifetime.Idle() &&
                       f.state.denoiseFirst.lifetime.Idle();
    const bool ok = pinned && incomplete && ready;
    std::printf("current and retired private creation notifications retain exact reset-racing submissions: %s\n",
                ok ? "PASS" : "FAIL");
    return ok;
}

bool CurrentAndRetiredCreationFailuresStayQuarantined(bool abandon)
{
    PrivateCreationFixture f;
    {
        auto submission = f.Begin();
        if (!abandon)
            f.Quarantine();
        f.Reset();
        if (!abandon)
        {
            submission.Complete(f.queue.Get());
            f.queue->Complete();
        }
    }
    const bool ok = f.AllQuarantined() && f.state.pendingSubmissions == 0;
    std::printf("current and retired creation %s remains quarantined: %s\n", abandon ? "abandonment" : "quarantine",
                ok ? "PASS" : "FAIL");
    return ok;
}

bool CreationPreparationAllocationFailuresQuarantineEveryGeneration()
{
    unsigned injected = 0;
    bool success = false;
    bool safe = true;
    for (int allocation = 0; allocation < 128; ++allocation)
    {
        PrivateCreationFixture f;
        bool threw = false;
        failAllocationAfter = allocation;
        try
        {
            auto submission = f.Begin();
            failAllocationAfter = -1;
            submission.Complete(nullptr);
        }
        catch (const std::bad_alloc&)
        {
            threw = true;
            ++injected;
        }
        failAllocationAfter = -1;
        if (!threw)
        {
            success = true;
            break;
        }
        // The real batch still executes after preparation fails; every child,
        // including not-yet-visited retired helpers, must fail closed on Reset.
        f.Quarantine();
        f.Reset();
        safe &= f.AllQuarantined() && f.state.pendingSubmissions == 0;
    }
    const bool ok = success && injected > 0 && safe;
    std::printf("private creation aggregation allocation sweep: %u sites, %s\n", injected, ok ? "PASS" : "FAIL");
    return ok;
}

int main()
{
    if (!PreparationAllocationFailureRetainsRecording())
        return 1;
    if (!FailedOwnerPreparationQuarantinesUnvisitedChildren())
        return 1;
    if (!CurrentAndRetiredCreationNotificationsAreCoupled() ||
        !CurrentAndRetiredCreationFailuresStayQuarantined(false) ||
        !CurrentAndRetiredCreationFailuresStayQuarantined(true) ||
        !CreationPreparationAllocationFailuresQuarantineEveryGeneration())
        return 1;
    unsigned failures = 0, injected = 0;
    bool reachedSuccess = false;
    for (int allocation = 0; allocation < 64; ++allocation)
    {
        ComPtr<CpuCommands> commands;
        commands.Attach(new CpuCommands);
        DlssNr_Dx12::State state;
        state.enlarger = std::make_unique<DlssNr_Dx12::State::Enlarger>();
        state.enlarger->creation = commands.Get();
        state.inputHold.captureCommands = commands.Get();
        state.late.Arm(state.late.slots[0], commands.Get());
        state.enlarger->lifetime.Record(commands.Get());
        ID3D12CommandList* lists[] { commands.Get() };
        bool threw = false;
        failAllocationAfter = allocation;
        try
        {
            auto token = state.BeginFinishedPictureSubmission(1, lists);
            failAllocationAfter = -1;
            if (!token || state.pendingSubmissions != 1 || state.enlarger->pendingSubmissions != 1 ||
                state.late.slots[0].pendingSubmissions != 1 || state.pendingHoldGenerations.size() != 1)
                ++failures;
            token.Complete(nullptr); // fail-closed abandonment must balance successfully committed owner pins
        }
        catch (const std::bad_alloc&)
        {
            threw = true;
            ++injected;
        }
        failAllocationAfter = -1;
        const bool balanced = state.pendingSubmissions == 0 && state.enlarger->pendingSubmissions == 0 &&
                              state.late.slots[0].pendingSubmissions == 0 && state.pendingHoldGenerations.empty();
        if (!balanced)
        {
            ++failures;
            std::printf("FAIL: allocation %d left owner=%u init=%u copy=%u hold=%zu pins\n", allocation,
                        state.pendingSubmissions, state.enlarger->pendingSubmissions,
                        state.late.slots[0].pendingSubmissions, state.pendingHoldGenerations.size());
        }
        if (!threw)
        {
            reachedSuccess = true;
            break;
        }
    }
    if (!reachedSuccess || !injected)
        ++failures;
    std::printf("production State allocation fault sweep: %u injected sites, %u failures\n", injected, failures);
    if (!InvalidInitializationDoesNotBindReusedPointer(true, true))
        ++failures;
    if (!InvalidInitializationDoesNotBindReusedPointer(false, false))
        ++failures;
    if (!RetiredEnlargerWaitsForMetadataPins())
        ++failures;
    if (!CollectorAllocationFailureRestoresReentrancyGuard())
        ++failures;
    if (!CollectorFaultSweepDoesNotLeaveMovedFromEntries())
        ++failures;
    if (!LateCopyRetainsCompletedReplayableProducer())
        ++failures;
    if (!CancelledLateCopyWaitsForReplayOnEveryQueue())
        ++failures;
    for (unsigned failure = 0; failure < 3; ++failure)
        if (!LateCopyFailureStaysQuarantined(failure))
            ++failures;
    if (!DestroyedLateProducerWaitsForSubmittedCopies())
        ++failures;
    if (!LateCompositionSkipsReplayableInput(false))
        ++failures;
    if (!LateCompositionSkipsReplayableInput(true))
        ++failures;
    if (!LateDrainCannotUseFirstFenceToCompleteReplay())
        ++failures;
    if (!DiscardedLateRecordingDoesNotRequireUnpromisedFence())
        ++failures;
    if (!LateOwnerRetainsReplayUntilEveryQueueCompletes())
        ++failures;
    if (!DestroyedUnsubmittedCopiesAreReusable())
        ++failures;
    if (!LateMetadataRequiresCapturedRecording(false))
        ++failures;
    if (!LateMetadataRequiresCapturedRecording(true))
        ++failures;
    if (!FailedLateCloseIsNotDiscarded())
        ++failures;
    return failures ? 1 : 0;
}

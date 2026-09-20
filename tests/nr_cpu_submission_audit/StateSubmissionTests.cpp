// Mechanically extracted production State submission method; actual tracker/token.
// Only surrounding owner/COM dependencies are CPU fixtures. No graphics DLL is loaded.
#include "../nr_cpu_timing_audit/CpuD3d12.h"
#include "../../OptiScaler/dlssnr/DlssNr_GpuLifetime.cpp"
#include "../../OptiScaler/shaders/dlssnr/DlssNr_GpuTime.h"
#include <cstdlib>
#include <cstdio>
#include <new>

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
struct CpuCommands final : ID3D12GraphicsCommandList
{
    ULONG references = 1;
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
    void ResolveQueryData(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT, UINT, ID3D12Resource*, UINT64) override {}
};

struct DlssNr_Dx12
{
    struct State
    {
        std::recursive_mutex mutex;
        DlssNr::GpuLifetime lifetime;
        struct DeferredSr
        {
            DlssNr::GpuLifetime lifetime;
        } deferredSr;
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
            struct Slot
            {
                bool pending = false, submitted = false;
                ID3D12CommandList* producer = nullptr;
                ComPtr<ID3D12CommandQueue> producerQueue;
                ComPtr<ID3D12Fence> fence;
                uint64_t ready = 1;
                unsigned pendingSubmissions = 0;
            };
            std::array<Slot, 1> slots;
            std::atomic_bool tracking { true };
            ComPtr<ID3D12CommandQueue> producerQueue;
            void Say(const char*) {}
        } late;
        void CollectEnlargers();
        DlssNr::GpuSubmission BeginFinishedPictureSubmission(UINT, ID3D12CommandList* const*);
    };
};

#include "state-method-under-test.inc"
#include "enlarger-collector-under-test.inc"

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

int main()
{
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
        state.late.slots[0].producer = commands.Get();
        state.late.slots[0].pending = true;
        state.lifetime.Record(commands.Get());
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
    return failures ? 1 : 0;
}

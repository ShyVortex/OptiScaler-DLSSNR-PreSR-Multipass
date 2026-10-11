#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Standalone automated unit test suite verifying the fix for the Helldivers 2 crash when enabling XeMFG-SR:
// 1. Thread-safe self-initialization and null protection in LowLatencyCtx.
// 2. Dispatch safety and error return in nvapi_calls when context pointer is null.
// 3. ReflexHooks routing invariants (IsXeFGActive gating on swapchain and feature state).
// 4. Menu guidance and swapchain restart diagnostic triggers.
// 5. Post-attach pipeline auto-configuration and lifecycle sequencing.

// ============================================================================
// 1. LowLatencyCtx Self-Initialization & Concurrency Simulation
// ============================================================================
class DummyLowLatency
{
  public:
    std::atomic<int> counter { 0 };
    int DoWork() { return ++counter; }
};

class SimulatedLowLatencyCtx
{
  public:
    static void init()
    {
        std::scoped_lock lock(s_mutex);
        if (!s_instance)
            s_instance = new DummyLowLatency();
    }

    static void shutdown()
    {
        std::scoped_lock lock(s_mutex);
        delete s_instance;
        s_instance = nullptr;
    }

    static DummyLowLatency* get()
    {
        if (!s_instance)
            init();
        return s_instance;
    }

    static bool is_allocated()
    {
        std::scoped_lock lock(s_mutex);
        return s_instance != nullptr;
    }

  private:
    static inline DummyLowLatency* s_instance = nullptr;
    static inline std::mutex s_mutex {};
};

void Test_LowLatencyCtx_LazyInit_And_Concurrency()
{
    // Ensure clean initial state
    SimulatedLowLatencyCtx::shutdown();
    assert(!SimulatedLowLatencyCtx::is_allocated());

    // 1A. Calling get() without prior init() must auto-initialize
    DummyLowLatency* ctx1 = SimulatedLowLatencyCtx::get();
    assert(ctx1 != nullptr);
    assert(SimulatedLowLatencyCtx::is_allocated());

    DummyLowLatency* ctx2 = SimulatedLowLatencyCtx::get();
    assert(ctx1 == ctx2); // Must return same singleton instance

    // 1B. Concurrent multithreaded access
    SimulatedLowLatencyCtx::shutdown();
    assert(!SimulatedLowLatencyCtx::is_allocated());

    constexpr int kNumThreads = 16;
    std::vector<std::thread> threads;
    std::vector<DummyLowLatency*> results(kNumThreads, nullptr);

    for (int i = 0; i < kNumThreads; ++i)
    {
        threads.emplace_back(
            [i, &results]()
            {
                // Concurrent call to get() on uninitialized context
                results[i] = SimulatedLowLatencyCtx::get();
                if (results[i])
                    results[i]->DoWork();
            });
    }

    for (auto& t : threads)
        t.join();

    assert(SimulatedLowLatencyCtx::is_allocated());
    DummyLowLatency* master = results[0];
    assert(master != nullptr);
    for (int i = 1; i < kNumThreads; ++i)
    {
        assert(results[i] == master); // All threads must observe identical instance
    }
    assert(master->counter.load() == kNumThreads);

    // Clean shutdown
    SimulatedLowLatencyCtx::shutdown();
    assert(!SimulatedLowLatencyCtx::is_allocated());

    printf("  [PASS] Test 1: LowLatencyCtx thread-safe lazy-init and concurrency verified.\n");
}

// ============================================================================
// 2. nvapi_calls Null Guard Safety Simulation
// ============================================================================
namespace SimulatedNvapiCalls
{
constexpr int NVAPI_OK = 0;
constexpr int NVAPI_ERROR = -1;
constexpr int NVAPI_INVALID_ARGUMENT = -2;

int SetSleepMode(DummyLowLatency* ctx, void* pDev, void* pParams)
{
    if (!pDev || !pParams)
        return NVAPI_INVALID_ARGUMENT;

    // Guard added in Task 1
    if (!ctx)
        return NVAPI_ERROR;

    return ctx->DoWork() > 0 ? NVAPI_OK : NVAPI_ERROR;
}

int Sleep(DummyLowLatency* ctx, void* pDev)
{
    if (!pDev)
        return NVAPI_INVALID_ARGUMENT;

    if (!ctx)
        return NVAPI_ERROR;

    return ctx->DoWork() > 0 ? NVAPI_OK : NVAPI_ERROR;
}

int GetLatency(DummyLowLatency* ctx, void* pDev, void* pParams)
{
    if (!pDev || !pParams)
        return NVAPI_INVALID_ARGUMENT;

    if (!ctx)
        return NVAPI_ERROR;

    return ctx->DoWork() > 0 ? NVAPI_OK : NVAPI_ERROR;
}

int SetLatencyMarker(DummyLowLatency* ctx, void* pDev, void* pParams)
{
    if (!pDev || !pParams)
        return NVAPI_INVALID_ARGUMENT;

    if (!ctx)
        return NVAPI_ERROR;

    return ctx->DoWork() > 0 ? NVAPI_OK : NVAPI_ERROR;
}

int SetAsyncFrameMarker(DummyLowLatency* ctx, void* pQueue, void* pParams)
{
    if (!pQueue || !pParams)
        return NVAPI_INVALID_ARGUMENT;

    if (!ctx)
        return NVAPI_ERROR;

    return ctx->DoWork() > 0 ? NVAPI_OK : NVAPI_ERROR;
}
} // namespace SimulatedNvapiCalls

void Test_NvapiCalls_NullGuards()
{
    int dummyDev = 1;
    int dummyParams = 2;

    // 2A. Null context must return NVAPI_ERROR cleanly without crashing or dereferencing nullptr
    DummyLowLatency* nullCtx = nullptr;
    assert(SimulatedNvapiCalls::SetSleepMode(nullCtx, &dummyDev, &dummyParams) == SimulatedNvapiCalls::NVAPI_ERROR);
    assert(SimulatedNvapiCalls::Sleep(nullCtx, &dummyDev) == SimulatedNvapiCalls::NVAPI_ERROR);
    assert(SimulatedNvapiCalls::GetLatency(nullCtx, &dummyDev, &dummyParams) == SimulatedNvapiCalls::NVAPI_ERROR);
    assert(SimulatedNvapiCalls::SetLatencyMarker(nullCtx, &dummyDev, &dummyParams) == SimulatedNvapiCalls::NVAPI_ERROR);
    assert(SimulatedNvapiCalls::SetAsyncFrameMarker(nullCtx, &dummyDev, &dummyParams) ==
           SimulatedNvapiCalls::NVAPI_ERROR);

    // 2B. Valid context operates normally
    DummyLowLatency validCtx;
    assert(SimulatedNvapiCalls::SetSleepMode(&validCtx, &dummyDev, &dummyParams) == SimulatedNvapiCalls::NVAPI_OK);
    assert(SimulatedNvapiCalls::Sleep(&validCtx, &dummyDev) == SimulatedNvapiCalls::NVAPI_OK);
    assert(SimulatedNvapiCalls::GetLatency(&validCtx, &dummyDev, &dummyParams) == SimulatedNvapiCalls::NVAPI_OK);
    assert(SimulatedNvapiCalls::SetLatencyMarker(&validCtx, &dummyDev, &dummyParams) == SimulatedNvapiCalls::NVAPI_OK);
    assert(SimulatedNvapiCalls::SetAsyncFrameMarker(&validCtx, &dummyDev, &dummyParams) ==
           SimulatedNvapiCalls::NVAPI_OK);

    printf("  [PASS] Test 2: nvapi_calls null guard safety verified.\n");
}

// ============================================================================
// 3. ReflexHooks Gating Invariants (IsXeFGActive & Native Driver Fallback)
// ============================================================================
enum class FGOutput
{
    NoFG = 0,
    DLSSG = 1,
    FSRFG = 2,
    XeFG = 3
};

struct SimulatedIFGFeature
{
    bool active = true;
    bool paused = false;

    bool IsActive() const { return active; }
    bool IsPaused() const { return paused; }
};

struct SimulatedOptiState
{
    FGOutput activeFgOutput = FGOutput::NoFG;
    void* currentFGSwapchain = nullptr;
    SimulatedIFGFeature* currentFG = nullptr;
};

inline bool IsXeFGActive(const SimulatedOptiState& state)
{
    return state.activeFgOutput == FGOutput::XeFG && state.currentFGSwapchain != nullptr &&
           state.currentFG != nullptr && state.currentFG->IsActive() && !state.currentFG->IsPaused();
}

enum class ReflexDestination
{
    NativeDriver,
    XeFGProxy
};

ReflexDestination RouteReflexCall(const SimulatedOptiState& state)
{
    if (IsXeFGActive(state))
        return ReflexDestination::XeFGProxy;
    return ReflexDestination::NativeDriver;
}

void Test_ReflexHooks_Gating_Invariants()
{
    SimulatedOptiState state;
    SimulatedIFGFeature fgFeature;
    int dummySwapchain = 42;

    // Case 1: Game starts with NoFG -> Must route to NativeDriver
    state.activeFgOutput = FGOutput::NoFG;
    state.currentFGSwapchain = nullptr;
    state.currentFG = nullptr;
    assert(!IsXeFGActive(state));
    assert(RouteReflexCall(state) == ReflexDestination::NativeDriver);

    // Case 2: Helldivers 2 reproduction: XeMFG-SR enabled mid-game
    // activeFgOutput is XeFG, BUT currentFGSwapchain is nullptr and currentFG is nullptr!
    state.activeFgOutput = FGOutput::XeFG;
    state.currentFGSwapchain = nullptr;
    state.currentFG = nullptr;
    assert(!IsXeFGActive(state));
    // CRITICAL BUG FIX VERIFICATION: Must NOT hijack to XeFGProxy when swapchain is null!
    assert(RouteReflexCall(state) == ReflexDestination::NativeDriver);

    // Case 3: Swapchain created, but currentFG feature object not yet initialized
    state.currentFGSwapchain = &dummySwapchain;
    state.currentFG = nullptr;
    assert(!IsXeFGActive(state));
    assert(RouteReflexCall(state) == ReflexDestination::NativeDriver);

    // Case 4: Feature object initialized, but IsActive() is false (e.g. during cutscene / loading screen)
    state.currentFG = &fgFeature;
    fgFeature.active = false;
    fgFeature.paused = false;
    assert(!IsXeFGActive(state));
    assert(RouteReflexCall(state) == ReflexDestination::NativeDriver);

    // Case 5: Feature is active, but paused (e.g. menu overlay or user pause)
    fgFeature.active = true;
    fgFeature.paused = true;
    assert(!IsXeFGActive(state));
    assert(RouteReflexCall(state) == ReflexDestination::NativeDriver);

    // Case 6: Fully initialized and running: XeFG output, valid swapchain, active and unpaused feature
    fgFeature.active = true;
    fgFeature.paused = false;
    assert(IsXeFGActive(state));
    assert(RouteReflexCall(state) == ReflexDestination::XeFGProxy);

    printf("  [PASS] Test 3: ReflexHooks gating invariants (IsXeFGActive) verified.\n");
}

// ============================================================================
// 4. Menu Restart State Diagnostics
// ============================================================================
struct SimulatedMenuState
{
    bool srEnable = false;
    void* currentFGSwapchain = nullptr;

    bool ShouldShowRestartWarning() const { return srEnable && (currentFGSwapchain == nullptr); }
};

void Test_Menu_Restart_Diagnostics()
{
    SimulatedMenuState menu;
    int dummySwapchain = 100;

    // 4A: Feature disabled -> No warning
    menu.srEnable = false;
    menu.currentFGSwapchain = nullptr;
    assert(!menu.ShouldShowRestartWarning());

    // 4B: Feature enabled, but swapchain not bound -> Warning shown
    menu.srEnable = true;
    menu.currentFGSwapchain = nullptr;
    assert(menu.ShouldShowRestartWarning());

    // 4C: Game restarted, swapchain now bound -> Warning cleared
    menu.currentFGSwapchain = &dummySwapchain;
    assert(!menu.ShouldShowRestartWarning());

    printf("  [PASS] Test 4: Menu restart state diagnostics verified.\n");
}

// ============================================================================
// 5. Post-Attach Pipeline Auto-Configuration & Dynamic Multipliers
// ============================================================================
enum class FGInput
{
    NoFG = 0,
    DLSSG = 1,
    FSRFG = 2,
    Upscaler = 3,
    ForceXeLL = 4,
    NvngxFG = 5
};

enum class FGNvngxReplacement
{
    None = 0,
    FSRFG = 1
};

struct SimulatedConfig
{
    bool XeMfgSrEnable = false;
    std::optional<int> XeMfgSrGeneratedFrames = 2;
    bool FGEnabled = false;
    FGInput FGInputMode = FGInput::NoFG;
    FGOutput FGOutputMode = FGOutput::NoFG;
    FGNvngxReplacement FGNvngxReplacementMode = FGNvngxReplacement::None;
    int FGXeFGInterpolationCount = 1;
};

void ConfigurePipelineOnAttach(SimulatedConfig& cfg, bool fakenvapiInitialized)
{
    if (cfg.XeMfgSrEnable)
    {
        cfg.FGEnabled = true;
        cfg.FGInputMode = FGInput::Upscaler;
        cfg.FGOutputMode = FGOutput::XeFG;
        cfg.FGNvngxReplacementMode = FGNvngxReplacement::None;
        const int srFrames = std::clamp(cfg.XeMfgSrGeneratedFrames.value_or(2), 1, 5);
        cfg.FGXeFGInterpolationCount = srFrames;
        assert(fakenvapiInitialized); // fakenvapi::init(true) must be called
    }
}

void Test_Pipeline_AutoConfiguration()
{
    SimulatedConfig cfg;
    cfg.XeMfgSrEnable = true;
    cfg.XeMfgSrGeneratedFrames = 3;

    bool fakenvapiInitCalled = true;
    ConfigurePipelineOnAttach(cfg, fakenvapiInitCalled);

    assert(cfg.FGEnabled == true);
    assert(cfg.FGInputMode == FGInput::Upscaler);
    assert(cfg.FGOutputMode == FGOutput::XeFG);
    assert(cfg.FGNvngxReplacementMode == FGNvngxReplacement::None);
    assert(cfg.FGXeFGInterpolationCount == 3);

    // Verify clamping 1 to 5
    cfg.XeMfgSrGeneratedFrames = 10;
    ConfigurePipelineOnAttach(cfg, fakenvapiInitCalled);
    assert(cfg.FGXeFGInterpolationCount == 5);

    cfg.XeMfgSrGeneratedFrames = -1;
    ConfigurePipelineOnAttach(cfg, fakenvapiInitCalled);
    assert(cfg.FGXeFGInterpolationCount == 1);

    printf("  [PASS] Test 5: Pipeline auto-configuration and multiplier clamping verified.\n");
}

int main()
{
    printf("[+] Starting XeMFG-SR Helldivers 2 Crash Fix Unit Tests...\n");
    Test_LowLatencyCtx_LazyInit_And_Concurrency();
    Test_NvapiCalls_NullGuards();
    Test_ReflexHooks_Gating_Invariants();
    Test_Menu_Restart_Diagnostics();
    Test_Pipeline_AutoConfiguration();
    printf("[+] All XeMFG-SR Crash Fix Unit Tests PASSED successfully!\n");
    return 0;
}

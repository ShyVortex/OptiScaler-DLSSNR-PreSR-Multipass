// Executes the production hook bodies with real NVAPI/Streamline types. Only
// configuration, logging, and the external runtimes are replaced; no GPU calls.
#define NOMINMAX
#include <windows.h>
#include <nvapi.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <cstdio>
#include <thread>

static unsigned warnings = 0;
#define LOG_FUNC(...) ((void) 0)
#define LOG_TRACE(...) ((void) 0)
#define LOG_WARN(...) (++warnings)
namespace magic_enum
{
template <class T> const char* enum_name(T) { return "test"; }
} // namespace magic_enum

enum class FGOutput
{
    NoFG,
    DLSSG,
    XeFG
};
enum class FGInput
{
    NoFG,
    DLSSG
};
namespace GameQuirk
{
constexpr unsigned HitmanReflexHacks = 1, FixSlSimulationMarkers = 2;
}
struct FakeFG
{
    uint64_t frame = 42;
    bool IsActive() const { return true; }
    bool IsPaused() const { return false; }
    uint64_t FrameCount() const { return frame; }
    void SetFrameCount(uint64_t value) { frame = value; }
};
struct State
{
    struct Version
    {
        unsigned major = 2;
    } streamlineVersion;
    struct Inputs
    {
        void evaluateState() {}
        void markPresent(const sl::FrameToken&) {}
    } slFGInputs, s_sl1FGInputs;
    FGInput activeFgInput = FGInput::NoFG;
    FGOutput activeFgOutput = FGOutput::DLSSG;
    FakeFG* currentFG = nullptr;
    unsigned gameQuirks = 0;
    bool rtssReflexInjection = false;
    uint64_t reflexFrameId = 42;
    uint64_t fgLastFrame = 42;
    static State& Instance()
    {
        static State state;
        return state;
    }
};
struct Config
{
    struct Option
    {
        bool value_or_default() const { return true; }
    } FGDLSSGUseGamesReflexMarkers;
    static Config* Instance()
    {
        static Config config;
        return &config;
    }
};
enum class ImGuiToastType
{
    Warning
};
struct ImGuiToast
{
    ImGuiToast(ImGuiToastType, int) {}
    void setTitle(const char*) {}
    void setContent(const char*) {}
};
namespace ImGui
{
void InsertNotification(const ImGuiToast&) {}
} // namespace ImGui

struct TestToken : sl::FrameToken
{
    operator uint32_t() const override { return 42; }
};
static TestToken token;
static sl::Result tokenReturnValue = sl::Result::eOk;
static sl::Result markerReturnValue = sl::Result::eOk;
static bool writeToken = true;
static sl::FrameToken* tokenOutput = &token;
static unsigned sleepCalls = 0, markerCalls = 0, originalSleepCalls = 0, originalMarkerCalls = 0;
static const sl::FrameToken* lastToken = nullptr;
static sl::Result GetToken(sl::FrameToken*& output, const uint32_t*)
{
    if (writeToken)
        output = tokenOutput;
    return tokenReturnValue;
}
static sl::Result RuntimeSleep(const sl::FrameToken& value)
{
    ++sleepCalls;
    lastToken = &value;
    return sl::Result::eOk;
}
static sl::Result Marker(sl::PCLMarker, const sl::FrameToken& value)
{
    ++markerCalls;
    lastToken = &value;
    return markerReturnValue;
}
static NvAPI_Status OriginalSleep(IUnknown*)
{
    ++originalSleepCalls;
    return NVAPI_ERROR;
}
static NvAPI_Status OriginalMarker(IUnknown*, NV_LATENCY_MARKER_PARAMS*)
{
    ++originalMarkerCalls;
    return NVAPI_ERROR;
}
struct StreamlineProxy
{
    static auto GetNewFrameToken() { return &GetToken; }
    static auto ReflexSleep() { return &RuntimeSleep; }
    static auto PCLSetMarker() { return &Marker; }
    static bool IsD3D12Inited() { return true; }
};
namespace nvapi_calls
{
NvAPI_Status NvAPI_D3D_Sleep(IUnknown* device) { return OriginalSleep(device); }
NvAPI_Status NvAPI_D3D_SetLatencyMarker(IUnknown* device, NV_LATENCY_MARKER_PARAMS* params)
{
    return OriginalMarker(device, params);
}
} // namespace nvapi_calls
static uint64_t _lastFrameId[20] {};
static IUnknown* _lastDev[20] {};
struct ReflexHooks
{
    inline static auto o_NvAPI_D3D_Sleep = &OriginalSleep;
    inline static auto o_NvAPI_D3D_SetLatencyMarker = &OriginalMarker;
    inline static IUnknown* _lastSleepDev = nullptr;
    inline static std::thread::id _lastSetSleepThread {};
    inline static uint64_t _lastAsyncMarkerFrameId = 42, _lastMarkerFrame = 0;
    inline static unsigned _updatesWithoutMarker = 0, _FgNumFramesToGenerate = 0;
    static NvAPI_Status hkNvAPI_D3D_Sleep(IUnknown*);
    static NvAPI_Status hkNvAPI_D3D_SetLatencyMarker(IUnknown*, NV_LATENCY_MARKER_PARAMS*);
};
struct StreamlineHooks
{
    inline static auto o_slGetNewFrameToken = &GetToken;
    inline static auto o_slPCLSetMarker = &Marker;
    static sl::Result hkslPCLSetMarker(sl::PCLMarker, const sl::FrameToken&);
};
#include "production-reflex.inc"

static unsigned checks = 0, failures = 0;
static void Expect(bool condition, const char* message)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}
int main()
{
    FakeFG fg;
    State::Instance().currentFG = &fg;
    auto* device = reinterpret_cast<IUnknown*>(static_cast<uintptr_t>(0x1234));
    NV_LATENCY_MARKER_PARAMS params {};
    params.version = NV_LATENCY_MARKER_PARAMS_VER;
    params.frameID = 42;
    params.markerType = SIMULATION_START;
    for (const auto scenario : { 0, 1, 2, 3 })
    {
        // Error with untouched output, error with output, success with null, success.
        tokenReturnValue = scenario < 2 ? sl::Result::eErrorInvalidParameter : sl::Result::eOk;
        writeToken = scenario != 0;
        tokenOutput = scenario == 2 ? nullptr : &token;
        warnings = sleepCalls = markerCalls = originalSleepCalls = originalMarkerCalls = 0;
        ReflexHooks::_lastSleepDev = nullptr;
        lastToken = nullptr;
        const auto slept = ReflexHooks::hkNvAPI_D3D_Sleep(device);
        const auto marked = ReflexHooks::hkNvAPI_D3D_SetLatencyMarker(device, &params);
        if (scenario < 3)
        {
            Expect(slept == NVAPI_ERROR && originalSleepCalls == 1 && sleepCalls == 0,
                   "failed/null frame token must preserve original NVAPI sleep without a Streamline call");
            Expect(marked == NVAPI_ERROR && originalMarkerCalls == 1 && markerCalls == 0,
                   "failed/null frame token must preserve original NVAPI marker without a Streamline call");
            Expect(ReflexHooks::_lastSleepDev == device, "failed token fallback must retain the sleep device");
            Expect(warnings >= 2, "token failures must report their rejected bridge calls");
        }
        else
        {
            Expect(slept == NVAPI_OK && sleepCalls == 1 && originalSleepCalls == 0,
                   "valid token must use the Streamline sleep bridge");
            Expect(marked == NVAPI_OK && markerCalls == 1 && originalMarkerCalls == 0,
                   "valid token must use the Streamline marker bridge");
            Expect(lastToken == &token, "bridge must forward the token returned by Streamline");
            Expect(warnings == 0, "successful token requests must remain quiet");
        }
    }
    State::Instance().gameQuirks = GameQuirk::FixSlSimulationMarkers;
    TestToken originalToken;
    markerReturnValue = sl::Result::eErrorInvalidParameter;
    for (const auto scenario : { 0, 1, 2, 3 })
    {
        StreamlineHooks::hkslPCLSetMarker(sl::PCLMarker::eSimulationEnd, originalToken);
        tokenReturnValue = scenario < 2 ? sl::Result::eErrorInvalidParameter : sl::Result::eOk;
        writeToken = scenario != 0;
        tokenOutput = scenario == 2 ? nullptr : &token;
        markerCalls = 0;
        lastToken = nullptr;
        const auto result = StreamlineHooks::hkslPCLSetMarker(sl::PCLMarker::eSimulationStart, originalToken);
        Expect(result == markerReturnValue && markerCalls == 1,
               "simulation correction must preserve the marker runtime result and forward once");
        Expect(lastToken == (scenario < 3 ? static_cast<sl::FrameToken*>(&originalToken) : &token),
               "failed/null correction token must preserve the original frame; valid token applies correction");
    }
    std::printf("%u checks, %u failures; CPU only\n", checks, failures);
    return failures ? 1 : 0;
}

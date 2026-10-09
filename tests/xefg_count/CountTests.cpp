// Compile the actual count method with real XeFG result/handle types. Only
// external provider calls and unrelated activation/logging are substituted.
// These tests exercise Off acknowledgement, not context initialization or GPU work.
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <xefg_swapchain.h>

#define LOG_INFO(...) ((void) 0)
#define LOG_DEBUG(...) ((void) 0)
#define LOG_WARN(...) ((void) 0)
#define LOG_ERROR(...) ((void) 0)
namespace magic_enum
{
template <class T> const char* enum_name(T) { return "test"; }
} // namespace magic_enum
struct ScopedSkipSpoofingGlobal
{
};
struct Config
{
    struct Count
    {
        UINT value = 3;
        void set_volatile_value(UINT next) { value = next; }
    } FGXeFGInterpolationCount;
    static Config* Instance()
    {
        static Config config;
        return &config;
    }
};
static xefg_swapchain_result_t enableResult = XEFG_SWAPCHAIN_RESULT_SUCCESS;
static unsigned enableCalls = 0;
static uint32_t lastEnable = 1;
static xefg_swapchain_result_t ProviderEnabled(xefg_swapchain_handle_t, uint32_t enable)
{
    ++enableCalls;
    lastEnable = enable;
    return enableResult;
}
static xefg_swapchain_result_t ProviderCount(xefg_swapchain_handle_t, uint32_t)
{
    return XEFG_SWAPCHAIN_RESULT_SUCCESS;
}
struct XeFGProxy
{
    inline static decltype(&xefgSwapChainSetEnabled) enable = ProviderEnabled;
    static auto SetEnabled() { return enable; }
    static auto SetNumInterpolatedFrames() { return &ProviderCount; }
};
struct XeFG_Dx12
{
    xefg_swapchain_handle_t _swapChainContext = reinterpret_cast<xefg_swapchain_handle_t>(0x1000);
    bool _passthrough = false;
    UINT _framesToInterpolate = 3;
    UINT _maxInterpolationCount = 5;
    bool _needResetHistory = false;
    bool _isActive = true;
    bool resourcesReady = true;
    void ClearAllResourceReady() { resourcesReady = false; }
    void Activate() { _isActive = true; } // Not exercised by these Off-only tests.
    bool SetInterpolatedFrameCount(UINT interpolatedFrameCount);
};
#include "production-count.inc"

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
    XeFG_Dx12 fg;
    enableResult = XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
    Expect(!fg.SetInterpolatedFrameCount(0), "rejected provider disable must return failure");
    Expect(fg._isActive && !fg._passthrough && fg._framesToInterpolate == 3,
           "rejected disable must preserve the accepted active/count state");
    Expect(fg.resourcesReady && !fg._needResetHistory,
           "rejected disable must not clear inputs or publish reset-history state");
    Expect(Config::Instance()->FGXeFGInterpolationCount.value == 3,
           "rejected disable must preserve the configured interpolation count");
    enableResult = XEFG_SWAPCHAIN_RESULT_SUCCESS;
    Expect(fg.SetInterpolatedFrameCount(0), "the unchanged Off request must succeed on retry");
    Expect(!fg._isActive && fg._passthrough && fg._framesToInterpolate == 0,
           "successful disable must publish passthrough/count zero");
    Expect(!fg.resourcesReady && fg._needResetHistory, "successful disable must clear readiness and reset history");
    Expect(enableCalls == 2 && lastEnable == 0, "rejected disable must be retried at the provider boundary");

    fg = XeFG_Dx12 {};
    XeFGProxy::enable = nullptr;
    Expect(!fg.SetInterpolatedFrameCount(0), "an existing context without a disable export must not claim success");
    Expect(fg._isActive && !fg._passthrough && fg._framesToInterpolate == 3 && fg.resourcesReady,
           "a missing disable export must preserve accepted state and inputs");

    fg._swapChainContext = nullptr;
    fg._isActive = false;
    Expect(fg.SetInterpolatedFrameCount(0), "Off with no context requires no provider call");
    Expect(!fg._isActive && fg._passthrough && fg._framesToInterpolate == 0 && !fg.resourcesReady,
           "no-context Off must publish clean passthrough state");
    Expect(enableCalls == 2, "a missing context/export must not invoke a provider function");
    std::printf("%u checks, %u failures; production Off method, CPU only\n", checks, failures);
    return failures ? 1 : 0;
}

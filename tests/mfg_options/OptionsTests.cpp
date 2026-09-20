// The runner compiles the actual production hook methods with the real Streamline
// ABI. Only config/state, logging and the external runtime are substituted. No GPU.
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <optional>
#include <mutex>
#include <memory>
#include <functional>
#include <sl_dlss_g.h>
#include <hooks/DlssgOptionsState.h>

static unsigned warnings = 0;
#define LOG_TRACE(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define LOG_FUNC(...) ((void)0)
#define LOG_WARN(...) (++warnings)
#define LOG_ERROR(...) (++warnings)
namespace magic_enum { template<class T> const char* enum_name(T) { return "test"; } }

template<class T> struct Option : std::optional<T>
{
    using std::optional<T>::operator=;
    T fallback {};
    T value_or_default() const { return this->value_or(fallback); }
    void set_volatile_value(T value) { *this = value; }
};
struct Config
{
    Option<int> FGDLSSGOverrideInterpolationCount;
    Option<bool> FGDLSSGOverrideForceDMFG;
    Option<float> FGDLSSGFramerateTargetDMFG;
    static Config* Instance() { static Config config; return &config; }
};
struct feature_version
{
    unsigned major, minor, patch;
    auto operator<=>(const feature_version&) const = default;
};
enum class FGInput { NoFG, DLSSG };
enum class FGOutput { NoFG, DLSSG };
enum class API { DX12, Vulkan };
struct FakeFG
{
    bool IsActive() const { return false; }
    bool IsPaused() const { return false; }
    unsigned GetInterpolatedFrameCount() const { return 0; }
};
struct State
{
    FGInput activeFgInput = FGInput::NoFG;
    FGOutput activeFgOutput = FGOutput::NoFG;
    feature_version streamlineVersion { 2, 14, 0 };
    bool dlssgGameDMFGSupported = false;
    std::optional<int> dlssgMfgMax;
    sl::DLSSGMode dlssgLastSetMode = sl::DLSSGMode::eOff;
    bool externalFrameGeneration = false;
    API swapchainApi = API::DX12;
    bool menuOverlayIsVulkan = false;
    int delayMenuRenderBy = 0;
    unsigned long long frameCount = 41;
    FakeFG* currentFG = nullptr;
    static State& Instance() { static State state; return state; }
};
namespace MfgUnlock
{
    enum class Failure { None, PatchFailed, RollbackFailed };
    static bool enabled = false;
    static unsigned maximum = 0;
    static Failure failure = Failure::None;
    void TryApply() {}
    bool Pending() { return false; }
    bool EnabledForSession() { return enabled; }
    unsigned UnlockedMax() { return maximum; }
    Failure LastFailure() { return failure; }
    unsigned EffectiveMax(unsigned nativeMaximum)
    { return failure == Failure::None ? std::max(nativeMaximum,maximum) : 1; }
}
namespace ReflexHooks
{
    static unsigned count = 1;
    void setDlssgFrameCount(unsigned value) { count = value; }
}
namespace MenuOverlayBase
{
    static bool visible = false;
    bool IsVisible() { return visible; }
}
static sl::Result nextResult = sl::Result::eOk;
static unsigned calls = 0, getCalls = 0, fakeNativeMaximum = 1;
static std::function<void()> onSet;
static sl::DLSSGOptions submitted;
static const sl::DLSSGOptions* submittedAddress = nullptr;
static sl::Result RuntimeSet(const sl::ViewportHandle&,const sl::DLSSGOptions& options)
{
    ++calls;
    submittedAddress = &options;
    submitted = options;
    if (onSet) onSet();
    return nextResult;
}
static sl::Result RuntimeGet(const sl::ViewportHandle&,sl::DLSSGState& state,const sl::DLSSGOptions*)
{
    ++getCalls;
    state.numFramesToGenerateMax = fakeNativeMaximum;
    state.bIsDynamicMFGSupported = sl::eFalse;
    return sl::Result::eOk;
}
struct StreamlineHooks
{
    inline static PFun_slDLSSGSetOptions* o_slDLSSGSetOptions = RuntimeSet;
    inline static PFun_slDLSSGGetState* o_slDLSSGGetState = RuntimeGet;
    inline static DlssgOptionsState dlssgOptionsState {};
    inline static std::atomic_bool gameDlssgOptionsObserved { false };
    static sl::Result hkslDLSSGSetOptions(const sl::ViewportHandle&,const sl::DLSSGOptions&);
    static sl::Result hkslDLSSGGetState(const sl::ViewportHandle&,sl::DLSSGState&,const sl::DLSSGOptions*);
    static void updateDlssgOptions();
    static void initializeDlssgOptions();
    static void applyMenuDlssgInterlock(sl::DLSSGOptions&,bool);
};
#include "production-methods.inc"

static unsigned checks = 0, failures = 0;
static void Expect(bool condition,const char* text)
{
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL: %s\n",text); }
}
static void Reset()
{
    *Config::Instance() = Config {};
    std::destroy_at(&StreamlineHooks::dlssgOptionsState);
    std::construct_at(&StreamlineHooks::dlssgOptionsState);
    StreamlineHooks::initializeDlssgOptions();
    State::Instance() = State {};
    MfgUnlock::enabled = false;
    MfgUnlock::maximum = 0;
    MfgUnlock::failure = MfgUnlock::Failure::None;
    ReflexHooks::count = 1;
    warnings = calls = getCalls = 0;
    nextResult = sl::Result::eOk;
    fakeNativeMaximum = 1;
    onSet = {};
    MenuOverlayBase::visible = false;
    submittedAddress = nullptr;
    StreamlineHooks::gameDlssgOptionsObserved = false;
}
// SEH contains a possible CPU read access violation in the unpatched method.
// Nothing executes GPU code and an expected violation never escapes this process.
static bool CallWithoutReadFault(const sl::DLSSGOptions* options)
{
    __try { StreamlineHooks::hkslDLSSGSetOptions(sl::ViewportHandle(0),*options); return true; }
    __except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { return false; }
}
int main()
{
    sl::DLSSGOptions options;
    options.mode = sl::DLSSGMode::eOn;
    Reset();
    bool observedBeforeRuntime = false;
    onSet = [&] { observedBeforeRuntime = StreamlineHooks::gameDlssgOptionsObserved.load(); };
    nextResult = sl::Result::eErrorInvalidParameter;
    StreamlineHooks::hkslDLSSGSetOptions(sl::ViewportHandle(0),options);
    Expect(observedBeforeRuntime,"record Streamline ownership before entering the runtime, even on rejection");
    Reset();
    State::Instance().dlssgMfgMax = 5;
    Config::Instance()->FGDLSSGOverrideInterpolationCount = 3;
    StreamlineHooks::updateDlssgOptions();
    nextResult = sl::Result::eErrorInvalidParameter;
    Expect(StreamlineHooks::hkslDLSSGSetOptions(sl::ViewportHandle(0),options) == nextResult,"preserve runtime rejection");
    Expect(ReflexHooks::count == 1,"rejected multiplier must retain accepted pacing count");
    Expect(State::Instance().dlssgLastSetMode == sl::DLSSGMode::eOff,"rejected mode must not commit");
    Expect(warnings > 0,"rejected options must produce diagnostic");
    Expect(StreamlineHooks::dlssgOptionsState.Pending(),"rejected intent must stay pending");
    nextResult = sl::Result::eOk;
    StreamlineHooks::hkslDLSSGSetOptions(sl::ViewportHandle(0),options);
    Expect(ReflexHooks::count == 3 && State::Instance().dlssgLastSetMode == sl::DLSSGMode::eOn,"successful options commit count and mode");
    Expect(!StreamlineHooks::dlssgOptionsState.Pending(),"accepted current intent clears pending");

    Reset();
    MfgUnlock::enabled = true; // Enabled but failed / unrecognized binary.
    Config::Instance()->FGDLSSGOverrideInterpolationCount = 3;
    StreamlineHooks::updateDlssgOptions();
    StreamlineHooks::hkslDLSSGSetOptions(sl::ViewportHandle(0),options);
    Expect(submitted.numFramesToGenerate == 1,"failed unlock must not permit a count above native");
    sl::DLSSGState state;
    StreamlineHooks::hkslDLSSGGetState(sl::ViewportHandle(0),state,nullptr);
    Expect(state.numFramesToGenerateMax == 1,"failed unlock must not advertise MFG");
    Expect(State::Instance().dlssgMfgMax == 1,"cached capability must reflect actual native maximum");

    Reset();
    MfgUnlock::failure = MfgUnlock::Failure::PatchFailed;
    fakeNativeMaximum = 5; // The advertise instruction cannot be trusted after a write failure.
    StreamlineHooks::hkslDLSSGGetState(sl::ViewportHandle(0),state,nullptr);
    Expect(state.numFramesToGenerateMax == 1 && State::Instance().dlssgMfgMax == 1,
           "failed patch must not trust a potentially modified native maximum");
    options.numFramesToGenerate = 3;
    StreamlineHooks::hkslDLSSGSetOptions(sl::ViewportHandle(0),options);
    Expect(submitted.numFramesToGenerate == 1,"failed patch limits even a game's own MFG request");
    options.numFramesToGenerate = 1;

    Reset();
    MfgUnlock::failure = MfgUnlock::Failure::RollbackFailed;
    StreamlineHooks::hkslDLSSGSetOptions(sl::ViewportHandle(0),options);
    Expect(submitted.mode == sl::DLSSGMode::eOff && ReflexHooks::count == 0,
           "incomplete rollback must disable FG rather than evaluate partially modified kernels");

    Reset();
    StreamlineHooks::hkslDLSSGGetState(sl::ViewportHandle(0),state,nullptr);
    Expect(getCalls == 1,"game state query must not consume the presented-frame counter twice");

    Reset();
    State::Instance().dlssgMfgMax = 5;
    StreamlineHooks::hkslDLSSGSetOptions(sl::ViewportHandle(0),options);
    calls = 0;
    Config::Instance()->FGDLSSGOverrideInterpolationCount = 3;
    StreamlineHooks::updateDlssgOptions();
    Expect(calls == 0,"UI change must not inject another runtime options call");
    StreamlineHooks::hkslDLSSGSetOptions(sl::ViewportHandle(0),options);
    Expect(calls == 1 && submitted.numFramesToGenerate == 3,"next game options call applies the queued ratio exactly once");
    Expect(getCalls == 0,"setting options must not secretly consume a state query");

    // A UI edit racing an in-flight call must not be acknowledged by that
    // older call's success. The fake external boundary controls the interleaving.
    onSet = [] {
        Config::Instance()->FGDLSSGOverrideInterpolationCount = 4;
        StreamlineHooks::updateDlssgOptions();
    };
    StreamlineHooks::hkslDLSSGSetOptions(sl::ViewportHandle(0),options);
    Expect(StreamlineHooks::dlssgOptionsState.Pending(),"older success must not acknowledge a newer queued ratio");
    Expect(ReflexHooks::count == 3,"old call commits only its captured ratio");
    onSet = {};
    StreamlineHooks::hkslDLSSGSetOptions(sl::ViewportHandle(0),options);
    Expect(submitted.numFramesToGenerate == 4 && !StreamlineHooks::dlssgOptionsState.Pending(),"subsequent game call accepts newest ratio");

    Reset();
    options.numFramesToGenerate = 2;
    StreamlineHooks::hkslDLSSGSetOptions(sl::ViewportHandle(0),options);
    Expect(submitted.numFramesToGenerate == 2 && ReflexHooks::count == 2,"no override preserves native game ratio and accepted pacing");
    options.numFramesToGenerate = 1;

    Reset();
    Config::Instance()->FGDLSSGOverrideInterpolationCount = 3;
    StreamlineHooks::updateDlssgOptions();
    options.structVersion = 6;
    StreamlineHooks::hkslDLSSGSetOptions(sl::ViewportHandle(0),options);
    Expect(submittedAddress == &options,"unknown future ABI must be forwarded without slicing into a current-version temporary");
    Expect(StreamlineHooks::dlssgOptionsState.Pending(),"unknown ABI must not acknowledge an override that was not applied");
    options.structVersion = 5;

    Reset();
    State::Instance().dlssgMfgMax = 5;
    fakeNativeMaximum = 0;
    StreamlineHooks::hkslDLSSGGetState(sl::ViewportHandle(0),state,nullptr);
    Expect(State::Instance().dlssgMfgMax.value_or(0) <= 1,"zero native maximum must invalidate stale cached MFG capability");

    Reset();
    MfgUnlock::enabled = true;
    MfgUnlock::maximum = 5;
    Config::Instance()->FGDLSSGOverrideInterpolationCount = 6;
    StreamlineHooks::updateDlssgOptions();
    StreamlineHooks::hkslDLSSGSetOptions(sl::ViewportHandle(0),options);
    Expect(submitted.numFramesToGenerate == 5,"override must be clamped at use, not only capability discovery");

    Reset();
    fakeNativeMaximum = 8; // Do not impose the Ada unlock ceiling on native future hardware.
    Config::Instance()->FGDLSSGOverrideInterpolationCount = 7;
    StreamlineHooks::updateDlssgOptions();
    StreamlineHooks::hkslDLSSGGetState(sl::ViewportHandle(0),state,nullptr);
    StreamlineHooks::hkslDLSSGSetOptions(sl::ViewportHandle(0),options);
    Expect(submitted.numFramesToGenerate == 7,"honor an actual native capability above the Ada ceiling");

    Reset();
    State::Instance().swapchainApi = API::Vulkan;
    State::Instance().dlssgMfgMax = 5;
    MenuOverlayBase::visible = true;
    Config::Instance()->FGDLSSGOverrideInterpolationCount = 3;
    StreamlineHooks::updateDlssgOptions();
    nextResult = sl::Result::eErrorInvalidParameter;
    StreamlineHooks::hkslDLSSGSetOptions(sl::ViewportHandle(0),options);
    Expect(submitted.mode == sl::DLSSGMode::eOff,"menu interlock requests off");
    Expect(ReflexHooks::count == 1,"failed interlock must not commit pacing zero or override count");
    nextResult = sl::Result::eOk;
    StreamlineHooks::hkslDLSSGSetOptions(sl::ViewportHandle(0),options);
    Expect(ReflexHooks::count == 0,"accepted interlock off commits count zero");
    Expect(StreamlineHooks::dlssgOptionsState.Pending(),"menu suppression must not acknowledge a still-unapplied positive multiplier");

    Reset();
    Config::Instance()->FGDLSSGOverrideInterpolationCount = 0;
    StreamlineHooks::updateDlssgOptions();
    StreamlineHooks::hkslDLSSGSetOptions(sl::ViewportHandle(0),options);
    Expect(submitted.mode == sl::DLSSGMode::eOff && !StreamlineHooks::dlssgOptionsState.Pending(),
           "explicit accepted Off request must clear pending");

    SYSTEM_INFO info {};
    GetSystemInfo(&info);
    auto* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr,info.dwPageSize*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if (!memory) return 2;
    DWORD oldProtection;
    if (!VirtualProtect(memory+info.dwPageSize,info.dwPageSize,PAGE_NOACCESS,&oldProtection)) return 2;
    for (auto version : {1u,2u,3u,4u,5u})
    {
        Reset();
        const size_t size = version == 1 ? 104 : version <= 3 ? 112 : 120;
        sl::DLSSGOptions seed;
        seed.structVersion = version;
        auto* input = reinterpret_cast<sl::DLSSGOptions*>(memory+info.dwPageSize-size);
        std::memcpy(input,&seed,size);
        const bool safe = CallWithoutReadFault(input);
        std::printf("ABI v%u (%zu bytes): %s\n",version,size,safe ? "safe" : "READ FAULT");
        Expect(safe,"version-bounded caller must not be read beyond its guard page");
        if (safe) Expect(submitted.structVersion == version,"preserve caller structure version");
    }
    VirtualFree(memory,0,MEM_RELEASE);
    std::printf("%u checks, %u failures; CPU only\n",checks,failures);
    return failures ? 1 : 0;
}

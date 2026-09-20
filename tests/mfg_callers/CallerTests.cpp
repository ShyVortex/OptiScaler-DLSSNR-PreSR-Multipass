#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>

enum NVSDK_NGX_Result
{
    NVSDK_NGX_Result_Success = 0,
    NVSDK_NGX_Result_Fail = 1,
};

namespace sl
{
enum class Result
{
    eOk,
    eErrorInvalidParameter,
};
enum class DLSSGMode
{
    eOff,
    eOn,
    eDynamic,
};
struct DLSSGOptions
{
    DLSSGMode mode = DLSSGMode::eOff;
    unsigned numFramesToGenerate = 1;
};
} // namespace sl

enum class FGInput
{
    NoFG,
    Upscaler,
    DLSSG,
    NvngxFG,
};

enum class FGOutput
{
    NoFG,
    FSRFG,
    DLSSG,
};

struct State
{
    int dlssgDetectedInterpolationCount = 0;
    sl::DLSSGMode dlssgLastSetMode = sl::DLSSGMode::eOff;

    static State& Instance()
    {
        static State state;
        return state;
    }
};

namespace ReflexHooks
{
static unsigned frameCount = 0;
static void setDlssgFrameCount(unsigned value) { frameCount = value; }
} // namespace ReflexHooks

namespace MfgUnlock
{
enum class Failure
{
    None,
    PatchFailed,
    RollbackFailed,
};
static bool patchFailed = false;
static unsigned verifiedMaximum = 0;
static void TryApply() {}
static unsigned EffectiveMax(unsigned nativeMaximum)
{
    return patchFailed ? 1 : std::max(nativeMaximum, verifiedMaximum);
}
} // namespace MfgUnlock

struct StreamlineHooks
{
    inline static std::optional<uint64_t> acceptedGeneration {};
    static void acceptDlssgOverrides(uint64_t generation) { acceptedGeneration = generation; }
};

static unsigned errorLogs = 0;
#define LOG_ERROR(...) (++errorLogs)
#define LOG_INFO(...) ((void) 0)
#define OPTISCALER_RTX40_MFG 1

#include "production-transactions.inc"

static unsigned checks = 0;
static unsigned failures = 0;

static void Expect(bool condition, const char* message)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

static void Reset()
{
    State::Instance() = {};
    ReflexHooks::frameCount = 0;
    StreamlineHooks::acceptedGeneration.reset();
    errorLogs = 0;
}

int main()
{
    MfgUnlock::patchFailed = false;
    MfgUnlock::verifiedMaximum = 0;
    Expect(ResolveDlssgEvaluationMaximum(std::nullopt) == 1,
           "unknown capability without a verified unlock must remain native x2");
    MfgUnlock::verifiedMaximum = 5;
    Expect(ResolveDlssgEvaluationMaximum(std::nullopt) == 5,
           "a verified unlock maximum must raise the evaluation capability");
    MfgUnlock::verifiedMaximum = 0;
    Expect(ResolveDlssgEvaluationMaximum(8) == 8,
           "native future-hardware capability must not be capped at the Ada limit");
    MfgUnlock::patchFailed = true;
    MfgUnlock::verifiedMaximum = 5;
    Expect(ResolveDlssgEvaluationMaximum(5) == 1, "failed patch transaction must distrust a stale native maximum");
    Expect(ResolveDlssgRuntimeMaximum(5) == 1, "DLSSG runtime caller must fail closed after a patch failure");
    Expect(ResolveNvngxAdvertisedMfgMaximum(true) == 1, "NGX capability table must fail closed after a patch failure");
    MfgUnlock::patchFailed = false;
    Expect(ResolveDlssgRuntimeMaximum(1) == 5, "DLSSG runtime caller must accept a verified unlock maximum");
    Expect(ResolveNvngxAdvertisedMfgMaximum(true) == 5,
           "NGX capability table must advertise a verified unlock maximum");
    Expect(CanEvaluateDlssg(MfgUnlock::Failure::PatchFailed),
           "completed rollback may evaluate at the fail-closed count");
    Expect(!CanEvaluateDlssg(MfgUnlock::Failure::RollbackFailed), "incomplete rollback must refuse NGX evaluation");
    Expect(CanDispatchDlssg(MfgUnlock::Failure::PatchFailed),
           "completed rollback may dispatch at the fail-closed count");
    Expect(!CanDispatchDlssg(MfgUnlock::Failure::RollbackFailed), "incomplete rollback must refuse owned dispatch");
    Expect(ShouldApplyDlssgEvaluationOverride(false, FGInput::NoFG, FGOutput::NoFG),
           "direct NGX baseline must apply desired override");
    Expect(ShouldApplyDlssgEvaluationOverride(false, FGInput::NvngxFG, FGOutput::NoFG),
           "explicit direct NGX mode must apply desired override");
    Expect(!ShouldApplyDlssgEvaluationOverride(true, FGInput::NoFG, FGOutput::NoFG),
           "observed Streamline ownership must preserve runtime count");
    Expect(!ShouldApplyDlssgEvaluationOverride(false, FGInput::DLSSG, FGOutput::NoFG),
           "configured Streamline input must preserve runtime count before first observed options call");
    Expect(!ShouldApplyDlssgEvaluationOverride(false, FGInput::Upscaler, FGOutput::DLSSG),
           "owned Streamline output must preserve runtime count");

    // A UI override is bounded by verified capability. Unknown capability is
    // the native x2 fallback, and zero is not submitted to the evaluation API.
    Expect(ResolveDlssgEvaluationFrameCount(4, 5, 3, true) == 3, "NGX override must clamp to the verified maximum");
    Expect(ResolveDlssgEvaluationFrameCount(4, 0, 5, true) == 1,
           "NGX evaluation override zero must use the API minimum");
    Expect(ResolveDlssgEvaluationFrameCount(4, 5, 0, true) == 1,
           "unknown capability must limit our override to native x2");
    Expect(ResolveDlssgEvaluationFrameCount(7, std::nullopt, 1, true) == 7,
           "native game count must not be clamped by our stale capability");
    Expect(ResolveDlssgEvaluationFrameCount(0, std::nullopt, 5, true) == 1,
           "missing native evaluation count must use the API minimum");
    Expect(ResolveDlssgEvaluationFrameCount(3, std::nullopt, 1, false) == 1,
           "completed rollback must clamp a native count learned from partial advertise mutation");
    Expect(ResolveDlssgEvaluationFrameCount(1, std::nullopt, 5, true) == 1,
           "managed pending override must preserve the runtime-supplied count");
    Expect(ResolveDlssgEvaluationFrameCount(1, 3, 5, true) == 3,
           "unmanaged direct NGX evaluation must apply the desired count");
    Expect(DirectDlssgOverrideGeneration(3, 17) == 17,
           "positive direct NGX override must capture its snapshot generation");
    Expect(!DirectDlssgOverrideGeneration(0, 18).has_value(),
           "direct NGX zero request must remain pending because evaluation cannot disable FG");
    Expect(DirectDlssgOverrideGeneration(std::nullopt, 19) == 19,
           "direct NGX Default intent must capture its snapshot generation");

    Reset();
    State::Instance().dlssgDetectedInterpolationCount = 2;
    ReflexHooks::frameCount = 2;
    auto rejected = CommitDlssgEvaluationResult(NVSDK_NGX_Result_Fail, 4, 17);
    Expect(rejected == NVSDK_NGX_Result_Fail, "NGX rejection must be preserved");
    Expect(State::Instance().dlssgDetectedInterpolationCount == 2 && ReflexHooks::frameCount == 2,
           "NGX rejection must retain the last accepted pacing state");
    Expect(!StreamlineHooks::acceptedGeneration.has_value(), "NGX rejection must leave the direct override pending");
    Expect(errorLogs == 1, "NGX rejection must emit a diagnostic");

    auto accepted = CommitDlssgEvaluationResult(NVSDK_NGX_Result_Success, 4, 17);
    Expect(accepted == NVSDK_NGX_Result_Success, "NGX success must be preserved");
    Expect(State::Instance().dlssgDetectedInterpolationCount == 4 && ReflexHooks::frameCount == 4,
           "NGX success must commit the submitted pacing count");
    Expect(StreamlineHooks::acceptedGeneration == 17,
           "successful direct NGX evaluation must acknowledge its snapshot generation");

    StreamlineHooks::acceptedGeneration.reset();
    CommitDlssgEvaluationResult(NVSDK_NGX_Result_Success, 1, std::nullopt);
    Expect(!StreamlineHooks::acceptedGeneration.has_value(),
           "managed NGX success must not acknowledge pending UI intent");

    CommitDlssgEvaluationResult(NVSDK_NGX_Result_Success, 1, DirectDlssgOverrideGeneration(std::nullopt, 19));
    Expect(StreamlineHooks::acceptedGeneration == 19,
           "successful direct NGX Default evaluation must acknowledge its generation");

    Reset();
    int acceptedFrames = 2;
    State::Instance().dlssgDetectedInterpolationCount = 2;
    State::Instance().dlssgLastSetMode = sl::DLSSGMode::eOn;
    ReflexHooks::frameCount = 2;
    sl::DLSSGOptions options {};
    options.mode = sl::DLSSGMode::eDynamic;
    options.numFramesToGenerate = 4;
    Expect(!CommitDlssgDispatchOptions(sl::Result::eErrorInvalidParameter, options, acceptedFrames),
           "dispatch must stop after rejected options");
    Expect(acceptedFrames == 2 && State::Instance().dlssgDetectedInterpolationCount == 2 &&
               State::Instance().dlssgLastSetMode == sl::DLSSGMode::eOn && ReflexHooks::frameCount == 2,
           "rejected dispatch options must retain all accepted state");
    Expect(errorLogs == 1, "dispatch rejection must emit a diagnostic");

    Expect(CommitDlssgDispatchOptions(sl::Result::eOk, options, acceptedFrames),
           "accepted dispatch options must continue dispatch");
    Expect(acceptedFrames == 4 && State::Instance().dlssgDetectedInterpolationCount == 4 &&
               State::Instance().dlssgLastSetMode == sl::DLSSGMode::eDynamic && ReflexHooks::frameCount == 4,
           "accepted dispatch options must commit actual mode and pacing");

    options.mode = sl::DLSSGMode::eOff;
    options.numFramesToGenerate = 4;
    Expect(CommitDlssgDispatchOptions(sl::Result::eOk, options, acceptedFrames),
           "accepted menu interlock must continue dispatch");
    Expect(State::Instance().dlssgDetectedInterpolationCount == 0 && ReflexHooks::frameCount == 0 &&
               State::Instance().dlssgLastSetMode == sl::DLSSGMode::eOff,
           "accepted off mode must commit zero pacing");

    std::printf("%u checks, %u failures; CPU only\n", checks, failures);
    return failures ? 1 : 0;
}

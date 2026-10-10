// Production evaluate/reset/jitter scope, with only the NGX/GPU boundary replaced.
#include <algorithm>
#include <cstdio>
#include <optional>

constexpr int NVSDK_NGX_Result_Success = 0;
constexpr int NVSDK_NGX_Result_Fail = 1;
constexpr const char* NVSDK_NGX_Parameter_Reset = "Reset";
constexpr const char* NVSDK_NGX_Parameter_Jitter_Offset_X = "JitterX";
constexpr const char* NVSDK_NGX_Parameter_Jitter_Offset_Y = "JitterY";
struct ID3D12GraphicsCommandList
{
};
struct ID3D12Resource
{
};
struct NVSDK_NGX_Parameter
{
    std::optional<unsigned> reset;
    float x = .25f, y = -.375f;
    int Get(const char* key, unsigned* out)
    {
        if (key != NVSDK_NGX_Parameter_Reset || !reset)
            return NVSDK_NGX_Result_Fail;
        *out = *reset;
        return NVSDK_NGX_Result_Success;
    }
    int Get(const char* key, float* out)
    {
        if (key == NVSDK_NGX_Parameter_Jitter_Offset_X)
            *out = x;
        else if (key == NVSDK_NGX_Parameter_Jitter_Offset_Y)
            *out = y;
        else
            return NVSDK_NGX_Result_Fail;
        return NVSDK_NGX_Result_Success;
    }
    void Set(const char* key, unsigned value)
    {
        if (key == NVSDK_NGX_Parameter_Reset)
            reset = value;
    }
    void Set(const char* key, int value) { Set(key, static_cast<unsigned>(value)); }
    void Set(const char* key, float value)
    {
        if (key == NVSDK_NGX_Parameter_Jitter_Offset_X)
            x = value;
        if (key == NVSDK_NGX_Parameter_Jitter_Offset_Y)
            y = value;
    }
};
template <class T> struct Option
{
    T value {};
    T value_or_default() const { return value; }
    Option& operator=(T v)
    {
        value = v;
        return *this;
    }
};
struct Config
{
    Option<int> DlssNrDenoiseFirstStep;
    static Config* Instance()
    {
        static Config c;
        return &c;
    }
};
struct TimerBoundary
{
    void Start(ID3D12GraphicsCommandList*) {}
    void End(ID3D12GraphicsCommandList*) {}
};
struct Handoff
{
    ID3D12Resource* color = nullptr;
    bool replaceOutput = false, zeroJitter = false, modelEvaluated = false;
};
struct Feature
{
    int _denoiseGameRoute = 0;
    bool nextSuccess = true;
    unsigned observedReset = 0;
    float observedX = 0, observedY = 0;
    TimerBoundary timer;
    TimerBoundary* UpscalerTime = &timer;
    bool EvaluateInternal(ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter* p)
    {
        observedReset = p->reset.value_or(0);
        observedX = p->x;
        observedY = p->y;
        return nextSuccess;
    }
    bool Frame(NVSDK_NGX_Parameter* InParameters, Handoff denoiseHandoff, bool ordinaryNrInputPrepared = false)
    {
        ID3D12GraphicsCommandList command;
        auto* InCommandList = &command;
#include "production-evaluate.inc"
        return evalResult;
    }
};
static unsigned failures = 0;
void Check(bool good, const char* name)
{
    std::printf("%s %s\n", good ? "PASS" : "FAIL", name);
    if (!good)
        ++failures;
}
int main()
{
    struct FrameCase
    {
        const char* name;
        bool accepted;
        int step;
        bool success;
        unsigned wantReset;
    };
    const FrameCase cases[] { { "initial ordinary route keeps game history", false, 2, true, 0 },
                              { "first composite handoff resets game history", true, 2, true, 1 },
                              { "stable composite handoff retains game history", true, 2, true, 0 },
                              { "switch to clean zero-jitter handoff resets game history", true, 0, true, 1 },
                              { "stable clean zero-jitter handoff retains game history", true, 0, true, 0 },
                              { "fallback to raw input resets game history", false, 0, true, 1 },
                              { "stable raw fallback retains game history", false, 2, true, 0 },
                              { "failed private route switch requests game reset", true, 2, false, 1 },
                              { "retry after failed route switch repeats game reset", true, 2, true, 1 },
                              { "successful retried route retains history next frame", true, 2, true, 0 },
                              { "switch to private output replacement resets game history", true, 1, true, 1 },
                              { "stable output replacement retains game history", true, 1, true, 0 },
                              { "replacement fallback resets game history", false, 1, true, 1 } };
    Feature feature;
    ID3D12Resource edited;
    for (const auto& test : cases)
    {
        Config::Instance()->DlssNrDenoiseFirstStep = test.step;
        Handoff handoff;
        if (test.accepted && test.step == 1)
            handoff.replaceOutput = true;
        else if (test.accepted)
            handoff.color = &edited;
        handoff.zeroJitter = test.accepted && test.step != 2;
        NVSDK_NGX_Parameter params;
        params.reset = 0;
        feature.nextSuccess = test.success;
        const bool result = feature.Frame(&params, handoff);
        Check(result == test.success && feature.observedReset == test.wantReset, test.name);
        Check(params.reset == 0u && params.x == .25f && params.y == -.375f,
              "evaluate scope restores caller Reset and jitter even after failure");
        Check(feature.observedX == (handoff.zeroJitter ? 0 : .25f) &&
                  feature.observedY == (handoff.zeroJitter ? 0 : -.375f),
              "game evaluation receives jitter appropriate for its accepted input");
    }
    Feature callerResetFeature;
    NVSDK_NGX_Parameter callerReset;
    callerReset.reset = 1;
    callerResetFeature.Frame(&callerReset, {});
    Check(callerResetFeature.observedReset == 1 && callerReset.reset == 1u,
          "caller requested Reset remains set after evaluation");
    Feature absentResetFeature;
    NVSDK_NGX_Parameter absentReset;
    Config::Instance()->DlssNrDenoiseFirstStep = 2;
    absentResetFeature.Frame(&absentReset, Handoff { &edited });
    Check(absentResetFeature.observedReset == 1 && absentReset.reset.value_or(0) == 0,
          "previously unset Reset restores semantic zero after route transition");
    Feature ordinary;
    struct OrdinaryCase
    {
        const char* name;
        bool edited, success;
        unsigned wantReset;
    };
    const OrdinaryCase ordinaryCases[] {
        { "initial raw input retains history", false, true, 0 },
        { "ordinary edited input resets game history", true, true, 1 },
        { "stable ordinary edited input retains history", true, true, 0 },
        { "rejected raw fallback resets without committing", false, false, 1 },
        { "raw fallback retry repeats reset", false, true, 1 },
        { "stable raw fallback retains history", false, true, 0 },
        { "rejected ordinary entry resets without committing", true, false, 1 },
        { "ordinary entry retry repeats reset", true, true, 1 },
        { "stable ordinary entry retains history", true, true, 0 },
    };
    for (const auto& test : ordinaryCases)
    {
        NVSDK_NGX_Parameter params;
        params.reset = 0;
        ordinary.nextSuccess = test.success;
        const auto accepted = ordinary.Frame(&params, {}, test.edited);
        Check(accepted == test.success && ordinary.observedReset == test.wantReset, test.name);
        Check(params.reset == 0u && params.x == .25f && params.y == -.375f && ordinary.observedX == params.x &&
                  ordinary.observedY == params.y,
              "ordinary route preserves raw game jitter and restores caller Reset");
    }
    for (int step = 0; step != 3; ++step)
    {
        Config::Instance()->DlssNrDenoiseFirstStep = step;
        NVSDK_NGX_Parameter params;
        params.reset = 0;
        ordinary.Frame(&params, step == 1 ? Handoff { nullptr, true } : Handoff { &edited });
        Check(ordinary.observedReset == 1, "ordinary-to-private transition resets history");
        ordinary.Frame(&params, {}, true);
        Check(ordinary.observedReset == 1, "private-to-ordinary transition resets history");
    }
    NVSDK_NGX_Parameter ordinaryCallerReset;
    ordinaryCallerReset.reset = 1;
    ordinary.Frame(&ordinaryCallerReset, {}, true);
    Check(ordinary.observedReset == 1 && ordinaryCallerReset.reset == 1u,
          "stable ordinary input preserves caller requested Reset");
    std::printf("%u assertion failure(s)\n", failures);
    return failures ? 1 : 0;
}

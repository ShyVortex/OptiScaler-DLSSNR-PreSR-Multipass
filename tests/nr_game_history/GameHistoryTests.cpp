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
    bool Frame(NVSDK_NGX_Parameter* InParameters, Handoff denoiseHandoff)
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
    std::printf("%u assertion failure(s)\n", failures);
    return failures ? 1 : 0;
}

// The runner extracts the real EncodeInput method and its declaration.
#include <algorithm>
#include <cstdio>
#include <functional>
#include <type_traits>
#include <vector>
#define LOG_INFO(...) ((void) 0)
#define LOG_WARN(...) ((void) 0)

static unsigned checks = 0, failures = 0;
static const char* scenario = "";
static void Expect(bool condition, const char* message)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::printf("FAIL [%s]: %s\n", scenario, message);
    }
}

enum D3D12_RESOURCE_STATES
{
    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
    D3D12_RESOURCE_STATE_COPY_SOURCE,
    D3D12_RESOURCE_STATE_COPY_DEST
};
enum
{
    D3D12_RESOURCE_DIMENSION_TEXTURE2D,
    DXGI_FORMAT_R32G32B32A32_FLOAT
};
struct D3D12_RESOURCE_DESC
{
    unsigned Width = 100, Height = 100, Format = 28;
    unsigned Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, DepthOrArraySize = 1;
    struct
    {
        unsigned Count = 1;
    } SampleDesc;
};
struct ID3D12Resource
{
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    unsigned writes = 0;
    D3D12_RESOURCE_DESC desc;
    D3D12_RESOURCE_DESC GetDesc() const { return desc; }
};
struct ID3D12Device
{
};
struct ID3D12GraphicsCommandList
{
    void CopyResource(ID3D12Resource* to, ID3D12Resource* from)
    {
        Expect(to->state == D3D12_RESOURCE_STATE_COPY_DEST && from->state == D3D12_RESOURCE_STATE_COPY_SOURCE,
               "copies must use the current source and destination states");
        ++to->writes;
    }
};
enum class Scaler
{
    Count,
    Test
};
static bool scalerSucceeds = false;
struct OS_Dx12
{
    OS_Dx12(const char*, ID3D12Device*, bool, Scaler) {}
    bool DispatchResources(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource* out)
    {
        if (scalerSucceeds)
            ++out->writes;
        return scalerSucceeds;
    }
};
template <class T> struct Value
{
    T value {};
    T value_or_default() const { return value; }
};
struct Config
{
    Value<unsigned> DlssNrWhitePointSource, DlssNrReversibleMode;
    Value<float> DlssNrWhitePointTrim { 1.0f };
    Value<float> DlssNrWhitePointScale { 1.0f };
    Value<bool> DlssNrHoldFrame;
    Value<Scaler> DlssNrScalingDownscaler { Scaler::Test };
    static Config* Instance()
    {
        static Config cfg;
        return &cfg;
    }
};
struct DlssNrFrameInfo
{
    bool ColourIsLinearHdr = true;
    ID3D12Resource* ExposureTexture = nullptr;
    D3D12_RESOURCE_STATES ExposureState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    float PreExposure = 1.0f, WhitePointOverride = 0.0f;
};
enum
{
    DlssNrMode_Meter = 1,
    DlssNrMode_Encode = 2,
    DlssNrMode_Downsample = 3,
    DlssNrMode_AutoExposure = 4
};
struct DlssNrConstants
{
    unsigned Mode = 0, Width = 0, Height = 0, Passthrough = 0, UseGameExposure = 0, ReversibleMode = 0;
    float WhitePoint = 0, ExposurePreMul = 0;
    unsigned ExposureSourceWidth = 0, ExposureSourceHeight = 0;
    float PreExposure = 1.0f;
};
namespace DlssNr
{
void ExposureConstants(DlssNrConstants& constants, const Config&, unsigned, float preExposure)
{
    constants.PreExposure = preExposure;
}
} // namespace DlssNr
struct Shader
{
    unsigned failMode = 0;
    std::vector<unsigned> calls;
    bool DispatchPass(ID3D12GraphicsCommandList*, const DlssNrConstants& p, ID3D12Resource* in, ID3D12Resource*,
                      ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource* out, ID3D12Resource* second)
    {
        calls.push_back(p.Mode);
        Expect(in->state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "dispatch input must be readable");
        Expect(out->state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "dispatch output must be writable");
        if (p.Mode == failMode)
            return false;
        ++out->writes;
        if (second)
            ++second->writes;
        return true;
    }
};
struct DlssNr_Dx12
{
    struct State
    {
        struct Model
        {
            unsigned width = 100, height = 100, workWidth = 100, workHeight = 100;
            ID3D12Resource *exposure = nullptr, *exposureMeter = nullptr;
            ID3D12Resource *colorCopy = nullptr, *hdrCopy = nullptr, *colorSmall = nullptr;
            bool exposureReadable = false;
            unsigned exposureSource = 0;
            float exposurePreExposure = 1.0f;
            ID3D12Resource* heldColor = nullptr;
            bool exposureSettingWasOn = true, heldActive = false;
            unsigned heldWidth = 0, heldHeight = 0, heldFormat = 0;
            float gamePreExposure = 1.0f, heldWhitePoint = 1.0f;
            Scaler nrScaler = Scaler::Test;
            OS_Dx12 *superUp = nullptr, *superDown = nullptr;
        } nr;
        struct Lifetime
        {
            void Retire(std::function<void()> destroy) { destroy(); }
        } lifetime;
        Shader shader;
        bool warnedSuper = false;
        unsigned meterCopies = 0;
        struct EncodeContext
        {
            ID3D12GraphicsCommandList* cmdList;
            ID3D12Device* device;
            ID3D12Resource* target;
            D3D12_RESOURCE_STATES targetState;
            const DlssNrFrameInfo& frame;
            float workScale;
            bool targetSupportsUav;
            bool spatial = false, encodeSucceeded = false;
            DlssNrConstants exposureConstants;
            ID3D12Resource* exposure = nullptr;
            float whitePoint = 1.0f, exposurePreMul = 0.0f;
            unsigned useGameExposure = 0;
            ID3D12Resource *exposureTex = nullptr, *modelInput = nullptr;
        };
#include "encode-declaration.inc"
        static void Barrier(ID3D12GraphicsCommandList*, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                            D3D12_RESOURCE_STATES after)
        {
            Expect(resource->state == before, "a barrier must name the actual previous state");
            resource->state = after;
        }
        void InvalidateExposureMeter() {}
        void CopyMeterToReadback(ID3D12GraphicsCommandList*, ID3D12Device*, bool) { ++meterCopies; }
        void ConsumeMeterReadback() {}
        void ReleaseSupersamplers()
        {
            delete nr.superUp;
            delete nr.superDown;
            nr.superUp = nr.superDown = nullptr;
        }
        float ResolveWhitePoint(const Config&, bool) { return 1.0f; }
        void ParkNrResource(ID3D12Resource*& resource)
        {
            delete resource;
            resource = nullptr;
        }
        ID3D12Resource* CreateScratch(ID3D12Device*, unsigned, unsigned, unsigned) { return new ID3D12Resource; }
    };
};
#include "encode-method.inc"

template <class T> bool Encode(T& state, typename T::EncodeContext& context)
{
    // The baseline void API cannot report failure; preserve that behavior for the red run.
    if constexpr (std::is_void_v<decltype(state.EncodeInput(context))>)
    {
        state.EncodeInput(context);
        return true;
    }
    else
        return state.EncodeInput(context);
}

static void Run(unsigned failMode, float scale, bool uav, bool useScaler = false)
{
    scenario = failMode == DlssNrMode_Meter        ? "optional meter failure"
               : failMode == DlssNrMode_Encode     ? "required encode failure"
               : failMode == DlssNrMode_Downsample ? "required resample failure"
                                                   : "successful encode";
    *Config::Instance() = {};
    Config::Instance()->DlssNrWhitePointSource.value = failMode == DlssNrMode_Meter ? 3u : 1u;
    scalerSucceeds = useScaler;
    ID3D12Resource target, meter, meterWide, color, hdr, small, exposure;
    exposure.desc.Width = exposure.desc.Height = 1;
    const auto entryState =
        uav ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    target.state = entryState;
    ID3D12Device device;
    ID3D12GraphicsCommandList commands;
    DlssNrFrameInfo frame;
    frame.ExposureTexture = &exposure;
    DlssNr_Dx12::State state;
    state.nr.exposure = &meter;
    state.nr.exposureMeter = &meterWide;
    state.nr.colorCopy = &color;
    state.nr.hdrCopy = &hdr;
    state.nr.colorSmall = &small;
    state.nr.workWidth = state.nr.workHeight = unsigned(100 * scale);
    state.shader.failMode = failMode;
    DlssNr_Dx12::State::EncodeContext context { &commands, &device, &target, entryState, frame, scale, uav };
    const bool result = Encode(state, context);
    const bool requiredFailure = failMode == DlssNrMode_Encode || failMode == DlssNrMode_Downsample;
    Expect(result == !requiredFailure, "required dispatch failure must be reported to Run");
    Expect(bool(context.exposure) == (failMode != DlssNrMode_Meter && failMode != DlssNrMode_Downsample),
           "failed optional exposure must not expose stale exposure to the encode");
    Expect(target.state == context.targetState, "target state must remain accurately tracked");
    if (requiredFailure)
    {
        Expect(context.modelInput == nullptr, "a failed encode must not expose unwritten model input");
        Expect(target.state == entryState, "failed encode must restore the target entry state");
        Expect(color.state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS &&
                   hdr.state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS &&
                   small.state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               "failed encode must restore touched scratch resources to UAV");
        if (failMode == DlssNrMode_Encode)
            Expect(state.shader.calls == std::vector<unsigned>({ DlssNrMode_Downsample, DlssNrMode_Encode }),
                   "failed encode must not dispatch downsampling");
    }
    else
    {
        Expect(context.modelInput == (scale == 1.0f ? &color : &small) && context.modelInput->writes == 1,
               "successful encode must expose the newly written model input");
        Expect(color.state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE &&
                   hdr.state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               "successful encoding must retain existing readable scratch states");
    }
    delete state.nr.superUp;
    delete state.nr.superDown;
}

int main()
{
    for (bool uav : { false, true })
    {
        Run(DlssNrMode_Encode, 0.5f, uav);
        Run(DlssNrMode_Downsample, 0.5f, uav);
        Run(DlssNrMode_Downsample, 2.0f, uav);
        Run(DlssNrMode_Meter, 1.0f, uav);
        Run(0, 0.5f, uav);
        Run(0, 2.0f, uav, true);
    }
    std::printf("%u checks, %u failures (CPU-only production EncodeInput)\n", checks, failures);
    return failures ? 1 : 0;
}

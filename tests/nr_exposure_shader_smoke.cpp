// WARP runs the production shader: metering, pre-exposure, trim curves and invalid-value fallback.
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include "../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#include "../OptiScaler/shaders/dlssnr/precompile/DlssNr_Shader.h"
using Microsoft::WRL::ComPtr;
struct Pixel
{
    float r, g, b, a;
};
static void check(HRESULT hr)
{
    if (FAILED(hr))
        throw std::runtime_error("D3D call failed");
}
static void expectNear(float actual, float expected, const char* why)
{
    if (!std::isfinite(actual) || std::abs(actual - expected) > 0.0003f * std::max(1.0f, std::abs(expected)))
    {
        std::printf("%s: actual %f expected %f\n", why, actual, expected);
        throw std::runtime_error(why);
    }
}
struct Texture
{
    ComPtr<ID3D11Texture2D> resource;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
};
int main()
try
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                            &context));
    ComPtr<ID3D11ComputeShader> shader;
    check(device->CreateComputeShader(DlssNr_cso, sizeof(DlssNr_cso), nullptr, &shader));
    context->CSSetShader(shader.Get(), nullptr, 0);
    auto texture = [&](unsigned width, unsigned height, const std::vector<Pixel>& data = {})
    {
        Texture t;
        D3D11_TEXTURE2D_DESC d {};
        d.Width = width;
        d.Height = height;
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        D3D11_SUBRESOURCE_DATA initial { data.data(), width * sizeof(Pixel), 0 };
        check(device->CreateTexture2D(&d, data.empty() ? nullptr : &initial, &t.resource));
        check(device->CreateShaderResourceView(t.resource.Get(), nullptr, &t.srv));
        check(device->CreateUnorderedAccessView(t.resource.Get(), nullptr, &t.uav));
        return t;
    };
    constexpr unsigned width = 67, height = 65;
    std::vector<Pixel> pixels(width * height);
    double sum = 0;
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
        {
            const float v = x == width - 1 ? 100.0f : 0.1f;
            pixels[y * width + x] = { v, v, v, 1 };
            sum += v;
        }
    auto input = texture(width, height, pixels), meter = texture(64, 64), exposure = texture(1, 1);
    auto keep = texture(width, height), output = texture(1, 1), sample = texture(1, 1, { { 2, 2, 2, 1 } });
    ComPtr<ID3D11Buffer> cb;
    D3D11_BUFFER_DESC buffer { sizeof(DlssNrConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
    check(device->CreateBuffer(&buffer, nullptr, &cb));
    context->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
    auto dispatch = [&](const DlssNrConstants& c, Texture& src, Texture& dst, Texture* ex = nullptr)
    {
        ID3D11ShaderResourceView* reads[] { src.srv.Get(), src.srv.Get(), src.srv.Get(),
                                            ex ? ex->srv.Get() : src.srv.Get(), src.srv.Get() };
        ID3D11UnorderedAccessView* writes[] { dst.uav.Get(), keep.uav.Get() };
        context->CSSetShaderResources(0, 5, reads);
        context->CSSetUnorderedAccessViews(0, 2, writes, nullptr);
        context->UpdateSubresource(cb.Get(), 0, nullptr, &c, 0, 0);
        context->Dispatch(c.Mode == DlssNrMode_Meter ? c.Width : (c.Width + 7) / 8,
                          c.Mode == DlssNrMode_Meter ? c.Height : (c.Height + 7) / 8, 1);
        ID3D11ShaderResourceView* emptyReads[5] {};
        ID3D11UnorderedAccessView* emptyWrites[2] {};
        context->CSSetShaderResources(0, 5, emptyReads);
        context->CSSetUnorderedAccessViews(0, 2, emptyWrites, nullptr);
    };
    auto read = [&](Texture& t)
    {
        D3D11_TEXTURE2D_DESC d;
        t.resource->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        check(device->CreateTexture2D(&d, nullptr, &staging));
        context->CopyResource(staging.Get(), t.resource.Get());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        const auto pixel = *static_cast<const Pixel*>(mapped.pData);
        context->Unmap(staging.Get(), 0);
        return pixel.r;
    };
    DlssNrConstants c {};
    c.Mode = DlssNrMode_Meter;
    c.Width = c.Height = 64;
    dispatch(c, input, meter);
    c.Mode = DlssNrMode_AutoExposure;
    c.Width = c.Height = 1;
    c.PreExposure = 1;
    c.ExposureSourceWidth = width;
    c.ExposureSourceHeight = height;
    dispatch(c, meter, exposure);
    const float expected = static_cast<float>(sum / pixels.size() * 0.82 / 0.18);
    expectNear(read(exposure), expected, "weighted meter over non-divisible dimensions");
    c.PreExposure = 8;
    dispatch(c, meter, exposure);
    expectNear(read(exposure), expected, "buffer white point invariant to pre-exposure metadata");
    c.ExposureProtection = 100;
    dispatch(c, meter, exposure);
    if (!(read(exposure) > 0 && read(exposure) < expected))
        throw std::runtime_error("highlight protection");

    c = {};
    c.Mode = DlssNrMode_Encode;
    c.Width = c.Height = 1;
    c.WhitePoint = 8;
    c.ReversibleMode = 1;
    dispatch(c, sample, output);
    const float manual = read(output);
    c.ExposureMode = 1;
    c.PreExposure = 8;
    c.ExposureTrim = 2;
    dispatch(c, sample, output, &sample); // exposure 2: base white 4, trim 2 => 8.
    expectNear(read(output), manual, "game exposure and trim");
    c.ExposureAnchorCount = 2;
    c.ExposureAnchors[0] = 1;
    c.ExposureAnchors[1] = 1;
    c.ExposureAnchors[2] = 16;
    c.ExposureAnchors[3] = 4;
    dispatch(c, sample, output, &sample); // base 4 is logarithmic midpoint, trim 2.
    expectNear(read(output), manual, "logarithmic trim interpolation");
    auto invalid = texture(1, 1, { { 0, 0, 0, 1 } });
    dispatch(c, sample, output, &invalid);
    expectNear(read(output), manual, "invalid exposure falls back to manual white");
    std::puts("Exposure shader: weighted meter, protection, pre-exposure, trim anchors and fallback passed");
    return 0;
}
catch (const std::exception& e)
{
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
}

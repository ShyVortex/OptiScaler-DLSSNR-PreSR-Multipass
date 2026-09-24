// Opt-in direct NVIDIA NGX FG resource test. No game injection or DLL downloads.
// Reference: NVIDIA-RTX/Streamline docs/DLSS-FG Programming Guide.pdf, SDK 310.7.
// Tests static carriers and a translating plane; not a game integration test.
// Uses documented parameter keys, without redistributing additional SDK headers.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>
#include <DirectXMath.h>
#include <nvsdk_ngx.h>
#include "../OptiScaler/dlssnr/ResidualFg.h"
#include <cstdio>
#include <cmath>
#include <stdexcept>
#include <vector>
#include <string>
using Microsoft::WRL::ComPtr;
void check(HRESULT h)
{
    if (FAILED(h))
        throw std::runtime_error("D3D12 failure");
}
void ngx(NVSDK_NGX_Result r)
{
    if (r != NVSDK_NGX_Result_Success)
    {
        std::fprintf(stderr, "NGX result: %08X\n", (unsigned) r);
        throw std::runtime_error("NGX failure");
    }
}
void expect(bool b, const char* why)
{
    if (!b)
        throw std::runtime_error(why);
}
template <class T> T proc(HMODULE m, const char* name)
{
    auto p = GetProcAddress(m, name);
    expect(p != nullptr, name);
    return (T) p;
}
void barrier(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x {};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    c->ResourceBarrier(1, &x);
}
int wmain(int argc, wchar_t** argv)
try
{
    expect(argc == 3, "Usage: nr_residual_fg_smoke <installed nvngx.dll> <official FG DLL directory>");
    HMODULE module = LoadLibraryW(argv[1]);
    expect(module != nullptr, "LoadLibrary nvngx.dll");
    using Init = NVSDK_NGX_Result (*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
                                      const NVSDK_NGX_FeatureCommonInfo*);
    auto init = proc<Init>(module, "NVSDK_NGX_D3D12_Init_Ext");
    auto allocate = proc<decltype(&NVSDK_NGX_D3D12_AllocateParameters)>(module, "NVSDK_NGX_D3D12_AllocateParameters");
    auto destroy = proc<decltype(&NVSDK_NGX_D3D12_DestroyParameters)>(module, "NVSDK_NGX_D3D12_DestroyParameters");
    auto create = proc<decltype(&NVSDK_NGX_D3D12_CreateFeature)>(module, "NVSDK_NGX_D3D12_CreateFeature");
    auto evaluate = proc<decltype(&NVSDK_NGX_D3D12_EvaluateFeature)>(module, "NVSDK_NGX_D3D12_EvaluateFeature");
    auto release = proc<decltype(&NVSDK_NGX_D3D12_ReleaseFeature)>(module, "NVSDK_NGX_D3D12_ReleaseFeature");
    ComPtr<IDXGIFactory1> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc {};
        adapter->GetDesc1(&desc);
        if (desc.VendorId == 0x10de &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
            break;
        adapter.Reset();
    }
    expect(device != nullptr, "No NVIDIA D3D12 adapter");
    const wchar_t* paths[] = { argv[2] };
    NVSDK_NGX_FeatureCommonInfo info {};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = [](const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
    { std::fprintf(stderr, "NGX: %s\n", message); };
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
    info.LoggingInfo.DisableOtherLoggingSinks = true;
    ngx(init(0x24480451, L".", device.Get(), NVSDK_NGX_Version_API, &info));
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC q {};
    check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> commands;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                    IID_PPV_ARGS(&commands)));
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    expect(event != nullptr, "Fence event");
    UINT64 serial = 0;
    auto submit = [&]()
    {
        check(commands->Close());
        ID3D12CommandList* lists[] = { commands.Get() };
        queue->ExecuteCommandLists(1, lists);
        check(queue->Signal(fence.Get(), ++serial));
        check(fence->SetEventOnCompletion(serial, event));
        expect(WaitForSingleObject(event, 15000) == WAIT_OBJECT_0, "GPU fence timeout");
        check(allocator->Reset());
        check(commands->Reset(allocator.Get(), nullptr));
    };
    constexpr UINT w = 1920, h = 1080, ow = 3840, oh = 2160;
    auto texture = [&](DXGI_FORMAT format, UINT width, UINT height)
    {
        D3D12_RESOURCE_DESC d {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = width;
        d.Height = height;
        d.DepthOrArraySize = d.MipLevels = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> r;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                              nullptr, IID_PPV_ARGS(&r)));
        return r;
    };
    auto carrier = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, ow, oh), depth = texture(DXGI_FORMAT_R32_FLOAT, w, h),
         motion = texture(DXGI_FORMAT_R16G16_FLOAT, w, h), exposure = texture(DXGI_FORMAT_R32_FLOAT, 1, 1),
         output = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, ow, oh);
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 4;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    UINT stride = device->GetDescriptorHandleIncrementSize(hd.Type);
    ID3D12Resource* inputs[] = { carrier.Get(), depth.Get(), motion.Get(), exposure.Get() };
    for (UINT i = 0; i < 4; ++i)
    {
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += i * stride;
        device->CreateUnorderedAccessView(inputs[i], nullptr, nullptr, cpu);
    }
    auto clear = [&](UINT i, float value, const D3D12_RECT* rect = nullptr)
    {
        ID3D12DescriptorHeap* heaps[] = { heap.Get() };
        commands->SetDescriptorHeaps(1, heaps);
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += i * stride;
        auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += i * stride;
        float v[] = { value, value, value, 1 };
        commands->ClearUnorderedAccessViewFloat(gpu, cpu, inputs[i], v, rect ? 1 : 0, rect);
        D3D12_RESOURCE_BARRIER uav {};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = inputs[i];
        commands->ResourceBarrier(1, &uav);
    };
    clear(0, 0.5f);
    clear(1, 0.5f);
    clear(2, 0.0f);
    clear(3, 1.0f);
    for (auto* r : inputs)
        barrier(commands.Get(), r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto capabilities =
        proc<decltype(&NVSDK_NGX_D3D12_GetCapabilityParameters)>(module, "NVSDK_NGX_D3D12_GetCapabilityParameters");
    DlssNr::ResidualFg fg({ capabilities, allocate, destroy, create, evaluate, release });
    std::puts("Creating offscreen FG through the production adapter");
    ngx(fg.Create(commands.Get(), ow, oh, w, h));
    submit();
    D3D12_RESOURCE_DESC flagDesc {};
    flagDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    flagDesc.Width = 256;
    flagDesc.Height = 1;
    flagDesc.DepthOrArraySize = flagDesc.MipLevels = 1;
    flagDesc.SampleDesc.Count = 1;
    flagDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    flagDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES flagHeap {};
    flagHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> suppression;
    check(device->CreateCommittedResource(&flagHeap, D3D12_HEAP_FLAG_NONE, &flagDesc,
                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&suppression)));
    DlssNr::ResidualFgCamera camera;
    DirectX::XMFLOAT4X4 matrix;
    auto projection = DirectX::XMMatrixPerspectiveFovRH(1.0f, float(w) / h, 0.1f, 100.0f);
    DirectX::XMStoreFloat4x4(&matrix, projection);
    memcpy(camera.viewToClip, &matrix, sizeof(matrix));
    DirectX::XMStoreFloat4x4(&matrix, DirectX::XMMatrixInverse(nullptr, projection));
    memcpy(camera.clipToView, &matrix, sizeof(matrix));
    DirectX::XMStoreFloat4x4(&matrix, DirectX::XMMatrixIdentity());
    memcpy(camera.clipToPrevious, &matrix, sizeof(matrix));
    memcpy(camera.previousToClip, &matrix, sizeof(matrix));
    camera.up[1] = camera.right[0] = 1;
    camera.forward[2] = -1;
    camera.nearPlane = 0.1f;
    camera.farPlane = 100;
    camera.fov = 1;
    camera.aspect = float(w) / h;
    camera.valid = true;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 bytes = 0;
    auto od = output->GetDesc();
    device->GetCopyableFootprints(&od, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    D3D12_RESOURCE_DESC bd {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes;
    bd.Height = 1;
    bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES rh {};
    rh.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    check(device->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&readback)));
    for (int test = 0; test < 3; ++test)
    {
        if (test == 1)
        {
            barrier(commands.Get(), carrier.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            D3D12_RECT left { 0, 0, (LONG) ow / 3, (LONG) oh }, right { 2 * (LONG) ow / 3, 0, (LONG) ow, (LONG) oh };
            clear(0, 0.25f, &left);
            clear(0, 0.75f, &right);
            barrier(commands.Get(), carrier.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        for (unsigned frame = 0; frame < 8; ++frame)
        {
            if (test == 2)
            {
                // A textured plane translates 32 pixels between NR anchors. Its
                // midpoint must be 16 pixels behind the current anchor, not a copy.
                barrier(commands.Get(), carrier.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                clear(0, 0.25f);
                D3D12_RECT band { 128 + 32 * (LONG) frame, 0, 384 + 32 * (LONG) frame, (LONG) oh };
                clear(0, 0.75f, &band);
                barrier(commands.Get(), carrier.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                if (!frame)
                {
                    barrier(commands.Get(), motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
                    cpu.ptr += 2 * stride;
                    auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
                    gpu.ptr += 2 * stride;
                    float mv[] = { -32.0f / ow, 0, 0, 0 };
                    commands->ClearUnorderedAccessViewFloat(gpu, cpu, motion.Get(), mv, 0, nullptr);
                    barrier(commands.Get(), motion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                }
            }
            ngx(fg.Evaluate(commands.Get(), carrier.Get(), depth.Get(), motion.Get(), output.Get(), suppression.Get(),
                            camera, false, frame == 0, (unsigned long long) (test * 8 + frame)));
            D3D12_RESOURCE_BARRIER uav {};
            uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uav.UAV.pResource = output.Get();
            commands->ResourceBarrier(1, &uav);
            submit();
        }
        barrier(commands.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
        src.pResource = output.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier(commands.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        submit();
        unsigned char* mapped = nullptr;
        check(readback->Map(0, nullptr, (void**) &mapped));
        auto sample = [&](UINT x)
        {
            auto* row = (uint16_t*) (mapped + (oh / 2) * footprint.Footprint.RowPitch);
            return DirectX::PackedVector::XMConvertHalfToFloat(row[x * 4]);
        };
        float a = sample(ow / 6), b = sample(ow / 2), c = sample(5 * ow / 6);
        std::printf("%s carrier output: %.6f %.6f %.6f\n", test == 2 ? "moving" : test ? "signed" : "neutral", a, b, c);
        expect(std::isfinite(a) && std::isfinite(b) && std::isfinite(c), "Non-finite DLSS result");
        if (!test)
            expect(std::abs(a - 0.5f) < 0.01f && std::abs(b - 0.5f) < 0.01f && std::abs(c - 0.5f) < 0.01f,
                   "Neutral carrier drift > 0.01");
        else if (test == 1)
            expect(a < 0.4f && b > 0.45f && b < 0.55f && c > 0.6f, "DLSS lost the signed regions");
        else
        {
            UINT edge = 0;
            while (edge < ow && sample(edge) < 0.5f)
                ++edge;
            std::printf("Moving carrier left edge: %u (expected midpoint 336, current anchor 352)\n", edge);
            expect(std::abs(int(edge) - 336) <= 5, "Output is not the expected motion-interpolated midpoint");
        }
        readback->Unmap(0, nullptr);
    }
    CloseHandle(event);
    std::puts(
        "PASS: direct NVIDIA FG neutral/signed carriers and translating-plane midpoint (not a game integration test)");
    return 0;
}
catch (const std::exception& e)
{
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}

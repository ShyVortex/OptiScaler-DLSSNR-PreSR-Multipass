// Opt-in real-GPU test of the production compatibility backend. Supply your own runtime.
// Run under an ordinary executable name to exercise the caller adapter.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>
#include <nvsdk_ngx.h>
#include <cstdio>
#include <cmath>
#include <stdexcept>
#include <vector>
#include <filesystem>
#include <psapi.h>
#include <thread>
#include <atomic>
#include "../../OptiScaler/dlssnr/DlssNr_RuntimeImports.h"
#include "../../OptiScaler/dlssnr/DlssNr_CompatibilityRuntime.h"
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
static unsigned pathCalls = 0;
static DWORD WINAPI WrappedPath(HMODULE module, LPWSTR path, DWORD capacity)
{
    ++pathCalls;
    return GetModuleFileNameW(module, path, capacity);
}
int wmain(int argc, wchar_t** argv)
try
{
    expect(argc == 3 || argc == 4,
           "Usage: nr_compatibility_smoke <installed _nvngx.dll> <compatibility runtime directory>");
    HMODULE module = LoadLibraryW(argv[1]);
    expect(module != nullptr, "LoadLibrary nvngx.dll");
    using Init = NVSDK_NGX_Result (*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
                                      const NVSDK_NGX_FeatureCommonInfo*);
    auto init = proc<Init>(module, "NVSDK_NGX_D3D12_Init_Ext");
    auto allocate =
        proc<decltype(&NVSDK_NGX_D3D12_GetCapabilityParameters)>(module, "NVSDK_NGX_D3D12_GetCapabilityParameters");
    auto destroy = proc<decltype(&NVSDK_NGX_D3D12_DestroyParameters)>(module, "NVSDK_NGX_D3D12_DestroyParameters");
    auto driverCreate = proc<decltype(&NVSDK_NGX_D3D12_CreateFeature)>(module, "NVSDK_NGX_D3D12_CreateFeature");
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
    if (argc == 4 && std::wstring(argv[3]) == L"--reject")
    {
        expect(!DlssNr::CompatibilityRuntime::Open(std::filesystem::path(argv[2]) / L"nvngx_dlssnr.dll", device.Get(),
                                                   allocate, destroy),
               "incompatible module accepted");
        expect(!GetModuleHandleW(L"nvngx_dlssnr.dll"), "rejected module not unloaded");
        puts("PASS: incompatible module rejected and unloaded");
        return 0;
    }
    const wchar_t* paths[] = { argv[2] };
    NVSDK_NGX_FeatureCommonInfo info {};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
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
    constexpr UINT w = 1920, h = 1080, ow = 1920, oh = 1080;
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
    auto carrier = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, w, h), depth = texture(DXGI_FORMAT_R32_FLOAT, w, h),
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
    };
    clear(0, 0.5f);
    clear(1, 0.5f);
    clear(2, 0.0f);
    clear(3, 1.0f);
    for (auto* r : inputs)
        barrier(commands.Get(), r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    NVSDK_NGX_Parameter *p = nullptr, *p2 = nullptr;
    ngx(allocate(&p));
    ngx(allocate(&p2));
    auto setup = [&](NVSDK_NGX_Parameter* param)
    {
        param->Set("DLSSNR.Enabled", 1u);
        param->Set("DLSSNR.Width", w);
        param->Set("DLSSNR.Height", h);
        param->Set("CreationNodeMask", 1u);
        param->Set("VisibilityNodeMask", 1u);
        param->Set("DLSSNR.Hint.Render.Preset", 0u);
        param->Set("DLSSNR.Intensity", 1.0f);
        param->Set("DLSSNR.Style", 0u);
        param->Set("DLSSNR.LocalStructureStrength", 1.0f);
        param->Set("DLSSNR.LocalToneStrength", 0.0f);
        param->Set("DLSSNR.SkinStructureStrength", -1.0f);
        param->Set("DLSSNR.UseAutoMask", 1u);
        param->Set("DLSSNR.UICorrection", 1u);
    };
    auto nrPath = std::filesystem::path(argv[2]) / L"nvngx_dlssnr.dll";
    if (argc == 4 && std::wstring(argv[3]) == L"--owners")
    {
        std::atomic<unsigned> failures = 0;
        std::vector<std::thread> threads;
        for (unsigned worker = 0; worker < 4; ++worker)
            threads.emplace_back(
                [&]
                {
                    for (unsigned step = 0; step < 8; ++step)
                    {
                        auto owner = DlssNr::CompatibilityRuntime::Open(nrPath, device.Get(), allocate, destroy);
                        if (!owner)
                            ++failures;
                        std::this_thread::yield();
                    }
                });
        for (auto& thread : threads)
            thread.join();
        expect(failures == 0, "concurrent owner acquisition rejected a live/retiring runtime");
        ngx(destroy(p));
        ngx(destroy(p2));
        CloseHandle(event);
        puts("PASS: 32 owner acquisitions across four threads without teardown rejection");
        return 0;
    }
    HMODULE preload = nullptr;
    std::vector<DlssNr::RuntimeImports::Slot> wrappedSlots;
    if (argc == 4 && std::wstring(argv[3]) == L"--preloaded")
    {
        preload = LoadLibraryExW(nrPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        expect(preload != nullptr, "preload");
        MODULEINFO loadedImage {};
        expect(GetModuleInformation(GetCurrentProcess(), preload, &loadedImage, sizeof(loadedImage)) != 0,
               "module info");
        std::vector<DlssNr::RuntimeImports::Slot> slots;
        expect(DlssNr::RuntimeImports::Find(
                   { static_cast<unsigned char*>(loadedImage.lpBaseOfDll), loadedImage.SizeOfImage }, slots),
               "imports");
        for (auto slot : slots)
            if (slot.wide)
            {
                DWORD protection = 0, ignored = 0;
                expect(VirtualProtect(slot.address, sizeof(void*), PAGE_READWRITE, &protection) != 0,
                       "wrapper protect");
                *slot.address = reinterpret_cast<void*>(&WrappedPath);
                VirtualProtect(slot.address, sizeof(void*), protection, &ignored);
                wrappedSlots.push_back(slot);
            }
    }
    const bool externallyLoaded = GetModuleHandleW(L"nvngx_dlssnr.dll") != nullptr;
    NVSDK_NGX_Handle* externalFeature = nullptr;
    for (unsigned cycle = 0; cycle < 4; ++cycle)
    {
        printf("cycle=%u\n", cycle);
        fflush(stdout);
        setup(p);
        NVSDK_NGX_Handle* driverHandle = nullptr;
        if (!externallyLoaded && (argc != 4 || std::wstring(argv[3]) != L"--direct"))
            expect(driverCreate(commands.Get(), (NVSDK_NGX_Feature) 18, p, &driverHandle) ==
                           NVSDK_NGX_Result_FAIL_UnableToInitializeFeature &&
                       !driverHandle,
                   "expected driver rejection before direct fallback");
        auto backend = DlssNr::CompatibilityRuntime::Open(nrPath, device.Get(), allocate, destroy);
        expect(backend != nullptr, "production direct backend open");
        auto shared = DlssNr::CompatibilityRuntime::Open(nrPath, device.Get(), allocate, destroy);
        expect(shared == backend, "device initialization must be shared");
        if (preload)
        {
            wchar_t path[MAX_PATH] {};
            auto query = reinterpret_cast<decltype(&GetModuleFileNameW)>(*wrappedSlots[0].address);
            const auto before = pathCalls;
            expect(query(preload, path, MAX_PATH) != 0 && pathCalls == before + 1,
                   "existing path wrapper was not chained");
        }
        auto nr = GetModuleHandleW(L"nvngx_dlssnr.dll");
        expect(nr != nullptr, "runtime module present");
        auto rawCreate = proc<decltype(driverCreate)>(nr, "NVSDK_NGX_D3D12_CreateFeature");
        NVSDK_NGX_Handle* rejected = nullptr;
        expect(rawCreate(commands.Get(), (NVSDK_NGX_Feature) 18, p, &rejected) == NVSDK_NGX_Result_FAIL_PlatformError &&
                   !rejected,
               "caller adaptation leaked outside backend scope");
        auto create = [&](auto* cmd, auto, auto* params, auto** feature)
        { return backend->Create(cmd, params, feature); };
        auto evaluate = [&](auto* cmd, auto* feature, auto* params, auto)
        { return backend->Evaluate(cmd, feature, params); };
        auto release = [&](auto* feature) { return backend->Release(feature); };
        setup(p);
        setup(p2);
        NVSDK_NGX_Handle *feature = nullptr, *other = nullptr;
        auto first = create(commands.Get(), (NVSDK_NGX_Feature) 18, p, &feature);
        printf("create1=%08X handle=%p id=%u\n", (unsigned) first, feature, feature ? feature->Id : 0);
        fflush(stdout);
        ngx(first);
        submit();
        auto second = create(commands.Get(), (NVSDK_NGX_Feature) 18, p2, &other);
        printf("create2=%08X handle=%p id=%u distinct=%d\n", (unsigned) second, other, other ? other->Id : 0,
               feature != other);
        fflush(stdout);
        if (second != NVSDK_NGX_Result_Success || !other)
        {
            submit();
            ngx(release(feature));
            ngx(destroy(p));
            ngx(destroy(p2));
            return 2;
        }
        submit();
        expect(feature != other, "NR reused pointer for identical creation settings");
        expect(feature->Id != other->Id, "NR reused feature ID for identical creation settings");
        auto bind = [&](NVSDK_NGX_Parameter* param)
        {
            param->Set("DLSSNR.Color", carrier.Get());
            param->Set("DLSSNR.Depth", depth.Get());
            param->Set("DLSSNR.MVec", motion.Get());
            param->Set("DLSSNR.Output", output.Get());
            param->Set("DLSSNR.DepthInverted", 0u);
            param->Set("DLSSNR.MVecScaleX", 1.0f);
            param->Set("DLSSNR.MVecScaleY", 1.0f);
            for (auto prefix : { "Color", "Output", "Depth", "MVec" })
            {
                char key[80];
                sprintf_s(key, "DLSSNR.%sSubrectBaseX", prefix);
                param->Set(key, 0u);
                sprintf_s(key, "DLSSNR.%sSubrectBaseY", prefix);
                param->Set(key, 0u);
                sprintf_s(key, "DLSSNR.%sSubrectWidth", prefix);
                param->Set(key, w);
                sprintf_s(key, "DLSSNR.%sSubrectHeight", prefix);
                param->Set(key, h);
            }
        };
        bind(p);
        bind(p2);
        if (externalFeature)
        {
            ngx(backend->Evaluate(commands.Get(), externalFeature, p));
            submit();
        }
        if (preload && cycle == 0)
        {
            ngx(backend->Create(commands.Get(), p, &externalFeature));
            submit();
        }
        if (externalFeature && cycle == 3)
        {
            ngx(backend->Release(externalFeature));
            externalFeature = nullptr;
        }

        for (unsigned frame = 0; frame < 3; ++frame)
            for (unsigned index = 0; index < 2; ++index)
            {
                auto* params = index ? p2 : p;
                auto* handle = index ? other : feature;
                params->Set("DLSSNR.Reset", frame == 0 ? 1u : 0u);
                auto result = evaluate(commands.Get(), handle, params, nullptr);
                printf("evaluate context=%u frame=%u result=%08X\n", index, frame, (unsigned) result);
                fflush(stdout);
                ngx(result);
                D3D12_RESOURCE_BARRIER uav {};
                uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                uav.UAV.pResource = output.Get();
                commands->ResourceBarrier(1, &uav);
                submit();
            }
        // Verify actual finite image data, not just successful API return codes.
        D3D12_RESOURCE_DESC desc = output->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        D3D12_HEAP_PROPERTIES rbHeap {};
        rbHeap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rbDesc {};
        rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rbDesc.Width = bytes;
        rbDesc.Height = 1;
        rbDesc.DepthOrArraySize = rbDesc.MipLevels = 1;
        rbDesc.SampleDesc.Count = 1;
        rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;
        check(device->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&readback)));
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
        void* mapped = nullptr;
        check(readback->Map(0, nullptr, &mapped));
        auto* pixel =
            reinterpret_cast<DirectX::PackedVector::HALF*>(static_cast<unsigned char*>(mapped) + footprint.Offset);
        float rgb[3];
        for (unsigned i = 0; i < 3; ++i)
        {
            rgb[i] = DirectX::PackedVector::XMConvertHalfToFloat(pixel[i]);
            expect(std::isfinite(rgb[i]), "non-finite NR output");
        }
        expect(rgb[0] + rgb[1] + rgb[2] > 0.01f, "black NR output");
        printf("readback RGB: %f %f %f\n", rgb[0], rgb[1], rgb[2]);
        readback->Unmap(0, nullptr);
        ngx(release(feature));
        ngx(release(other));
        backend.reset();
        expect(GetModuleHandleW(L"nvngx_dlssnr.dll") != nullptr, "shared owner lost runtime");
        shared.reset();
        expect((GetModuleHandleW(L"nvngx_dlssnr.dll") != nullptr) == externallyLoaded,
               "runtime ownership changed after teardown");
        for (auto slot : wrappedSlots)
            expect(*slot.address == reinterpret_cast<void*>(&WrappedPath), "previous import wrapper was not restored");
    }
    if (preload)
        FreeLibrary(preload);
    ngx(destroy(p));
    ngx(destroy(p2));
    CloseHandle(event);
    puts("PASS: four backend-owner cycles, independent features, fenced evaluations, image readback, and preserved "
         "module/import ownership");
    return 0;
}
catch (const std::exception& e)
{
    fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}

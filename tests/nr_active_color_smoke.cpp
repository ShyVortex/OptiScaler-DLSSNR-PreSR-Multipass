// Headless D3D12/WARP regression for padded pre-SR colour. No game or NVIDIA model is loaded.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <stdexcept>
#include <vector>
#include "../OptiScaler/shaders/dlssnr/DlssNr_ActiveColor.h"

using Microsoft::WRL::ComPtr;
void check(HRESULT hr)
{
    if (FAILED(hr))
        throw std::runtime_error("D3D12 call failed");
}
void expect(bool ok, const char* why)
{
    if (!ok)
        throw std::runtime_error(why);
}

D3D12_RESOURCE_DESC texture(unsigned int width, unsigned int height, bool uav = false)
{
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_UINT;
    desc.SampleDesc.Count = 1;
    desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

ComPtr<ID3D12Resource> create(ID3D12Device* device, D3D12_RESOURCE_DESC desc, D3D12_HEAP_TYPE type,
                              D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = type;
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)));
    return resource;
}

ComPtr<ID3D12Resource> buffer(ID3D12Device* device, UINT64 size, bool readback)
{
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return create(device, desc, readback ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_UPLOAD,
                  readback ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ);
}

void barrier(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to, UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
{
    if (from == to)
        return;
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = subresource;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    commands->ResourceBarrier(1, &b);
}

D3D12_TEXTURE_COPY_LOCATION location(ID3D12Resource* resource)
{
    D3D12_TEXTURE_COPY_LOCATION result {};
    result.pResource = resource;
    result.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    return result;
}

void run(ID3D12Device* device, unsigned int allocationW, unsigned int allocationH, unsigned int activeW,
         unsigned int activeH, bool uav, bool writeBack, unsigned int mipLevels = 1,
         D3D12_RESOURCE_STATES arrival = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
{
    auto desc = texture(allocationW, allocationH, uav);
    desc.MipLevels = static_cast<UINT16>(mipLevels);
    const auto active = DlssNr::PreSrColorExtent(desc, activeW, activeH);
    expect(active.has_value(), "valid active extent rejected");
    auto game = create(device, desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    auto compact =
        create(device, texture(activeW, activeH, true), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto upload = buffer(device, bytes, false), edited = buffer(device, bytes, false);
    auto croppedReadback = buffer(device, bytes, true), finalReadback = buffer(device, bytes, true);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT mip1Footprint {};
    UINT64 mip1Bytes = 0;
    ComPtr<ID3D12Resource> mip1Upload, mip1Readback;
    constexpr uint32_t mip1Sentinel = 0xA1B2C3D4u;
    if (mipLevels > 1)
    {
        device->GetCopyableFootprints(&desc, 1, 1, 0, &mip1Footprint, nullptr, nullptr, &mip1Bytes);
        mip1Upload = buffer(device, mip1Bytes, false);
        mip1Readback = buffer(device, mip1Bytes, true);
        unsigned char* memory = nullptr;
        D3D12_RANGE noRead { 0, 0 };
        check(mip1Upload->Map(0, &noRead, (void**) &memory));
        for (unsigned int y = 0; y < mip1Footprint.Footprint.Height; ++y)
        {
            auto* row = (uint32_t*) (memory + y * mip1Footprint.Footprint.RowPitch);
            for (unsigned int x = 0; x < mip1Footprint.Footprint.Width; ++x)
                row[x] = mip1Sentinel;
        }
        mip1Upload->Unmap(0, nullptr);
    }
    constexpr uint32_t padding = 0xD00DF00Du;
    constexpr uint32_t edit = 0x12345678u;
    for (auto* resource : { upload.Get(), edited.Get() })
    {
        unsigned char* memory = nullptr;
        D3D12_RANGE noRead { 0, 0 };
        check(resource->Map(0, &noRead, (void**) &memory));
        for (unsigned int y = 0; y < allocationH; ++y)
        {
            auto* row = (uint32_t*) (memory + y * footprint.Footprint.RowPitch);
            for (unsigned int x = 0; x < allocationW; ++x)
                row[x] = resource == edited.Get() ? edit : (x < activeW && y < activeH ? y * allocationW + x : padding);
        }
        resource->Unmap(0, nullptr);
    }

    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> commands;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                    IID_PPV_ARGS(&commands)));
    auto src = location(upload.Get()), dst = location(game.Get());
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;
    commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (mip1Upload)
    {
        auto mip1Source = location(mip1Upload.Get()), mip1Target = location(game.Get());
        mip1Source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        mip1Source.PlacedFootprint = mip1Footprint;
        mip1Target.SubresourceIndex = 1;
        commands->CopyTextureRegion(&mip1Target, 0, 0, 0, &mip1Source, nullptr);
    }
    // Match the production round trip, including restoring the game's readable state between copies.
    barrier(commands.Get(), game.Get(), D3D12_RESOURCE_STATE_COPY_DEST, arrival);
    // A game can use another mip independently while NR reads/copies the base image.
    const auto mip1State = arrival == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                               ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                               : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (mipLevels > 1)
        barrier(commands.Get(), game.Get(), arrival, mip1State, 1);
    DlssNr::TransitionActiveColor(commands.Get(), game.Get(), arrival, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    DlssNr::TransitionActiveColor(commands.Get(), game.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(commands.Get(), compact.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    DlssNr::CopyActiveColor(commands.Get(), compact.Get(), game.Get(), *active);
    DlssNr::TransitionActiveColor(commands.Get(), game.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(commands.Get(), compact.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    src = location(compact.Get());
    dst = location(croppedReadback.Get());
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    const D3D12_BOX box { 0, 0, 0, activeW, activeH, 1 };
    commands->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

    // Stand in for the NR edit without requiring proprietary DLLs. The copied-in image is checked
    // separately, so this cannot hide an incorrect crop or accidental resampling.
    barrier(commands.Get(), compact.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    src = location(edited.Get());
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;
    dst = location(compact.Get());
    commands->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    barrier(commands.Get(), compact.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (writeBack)
    {
        barrier(commands.Get(), compact.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        DlssNr::TransitionActiveColor(commands.Get(), game.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                      D3D12_RESOURCE_STATE_COPY_DEST);
        DlssNr::CopyActiveColor(commands.Get(), game.Get(), compact.Get(), *active);
        DlssNr::TransitionActiveColor(commands.Get(), game.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(commands.Get(), compact.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    DlssNr::TransitionActiveColor(commands.Get(), game.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, arrival);
    DlssNr::TransitionActiveColor(commands.Get(), game.Get(), arrival, D3D12_RESOURCE_STATE_COPY_SOURCE);
    src = location(game.Get());
    dst = location(finalReadback.Get());
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    DlssNr::TransitionActiveColor(commands.Get(), game.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, arrival);
    if (mip1Readback)
    {
        barrier(commands.Get(), game.Get(), mip1State, D3D12_RESOURCE_STATE_COPY_SOURCE, 1);
        auto mip1Source = location(game.Get()), mip1Target = location(mip1Readback.Get());
        mip1Source.SubresourceIndex = 1;
        mip1Target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        mip1Target.PlacedFootprint = mip1Footprint;
        commands->CopyTextureRegion(&mip1Target, 0, 0, 0, &mip1Source, nullptr);
        barrier(commands.Get(), game.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, mip1State, 1);
    }
    check(commands->Close());
    ID3D12CommandList* lists[] = { commands.Get() };
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    expect(completed != nullptr, "event creation failed");
    check(queue->Signal(fence.Get(), 1));
    check(fence->SetEventOnCompletion(1, completed));
    const auto waitResult = WaitForSingleObject(completed, 30000);
    CloseHandle(completed);
    expect(waitResult == WAIT_OBJECT_0, "GPU copy timed out");
    for (auto* resource : { croppedReadback.Get(), finalReadback.Get() })
    {
        unsigned char* memory = nullptr;
        D3D12_RANGE read { 0, (SIZE_T) bytes };
        check(resource->Map(0, &read, (void**) &memory));
        for (unsigned int y = 0; y < allocationH; ++y)
        {
            const auto* row = (const uint32_t*) (memory + y * footprint.Footprint.RowPitch);
            for (unsigned int x = 0; x < allocationW; ++x)
            {
                const bool inside = x < activeW && y < activeH;
                if (resource == croppedReadback.Get() && !inside)
                    continue;
                const uint32_t expected =
                    !inside ? padding : (resource == finalReadback.Get() && writeBack ? edit : y * allocationW + x);
                expect(row[x] == expected, "active pixels or allocation padding changed incorrectly");
            }
        }
        D3D12_RANGE noWrite { 0, 0 };
        resource->Unmap(0, &noWrite);
    }
    if (mip1Readback)
    {
        unsigned char* memory = nullptr;
        D3D12_RANGE read { 0, (SIZE_T) mip1Bytes };
        check(mip1Readback->Map(0, &read, (void**) &memory));
        for (unsigned int y = 0; y < mip1Footprint.Footprint.Height; ++y)
        {
            const auto* row = (const uint32_t*) (memory + y * mip1Footprint.Footprint.RowPitch);
            for (unsigned int x = 0; x < mip1Footprint.Footprint.Width; ++x)
                expect(row[x] == mip1Sentinel, "another game-owned mip was changed");
        }
        D3D12_RANGE noWrite { 0, 0 };
        mip1Readback->Unmap(0, &noWrite);
    }
    std::printf("PASS: %ux%u in %ux%u, UAV=%d, copy-back=%d\n", activeW, activeH, allocationW, allocationH, uav,
                writeBack);
}

int main()
try
{
    using DlssNr::PreSrColorExtent;
    auto desc = texture(2560, 1440);
    expect(PreSrColorExtent(desc, 0, 0)->width == 2560, "absent active size changed");
    expect(PreSrColorExtent(desc, 2558, 1439)->height == 1439, "odd extent rejected");
    expect(!PreSrColorExtent(desc, 0, 1439) && !PreSrColorExtent(desc, 2558, 0), "partial size accepted");
    expect(!PreSrColorExtent(desc, 2561, 1440) && !PreSrColorExtent(desc, 2560, 1441), "out-of-bounds size accepted");
    expect(!PreSrColorExtent(desc, 2558, 1439, 1, 0) && !PreSrColorExtent(desc, 0, 0, 0, 1), "offset accepted");
    desc.SampleDesc.Count = 4;
    expect(!PreSrColorExtent(desc, 2558, 1439), "MSAA accepted");
    desc.SampleDesc.Count = 1;
    desc.DepthOrArraySize = 2;
    expect(!PreSrColorExtent(desc, 2558, 1439), "array accepted");
    auto multiMip = texture(2258, 1270);
    multiMip.MipLevels = 12;
    multiMip.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    expect(DlssNr::PreSrColorCanUseScratch(multiMip, 2258, 1270), "typed multi-mip input rejected");
    multiMip.Format = DXGI_FORMAT_R16G16B16A16_TYPELESS;
    expect(!DlssNr::PreSrColorCanUseScratch(multiMip, 2258, 1270),
           "multi-mip format-changing scratch admitted without a post-SR fallback");
    multiMip.MipLevels = 1;
    expect(DlssNr::PreSrColorCanUseScratch(multiMip, 2258, 1270), "existing single-mip typeless admission changed");
    ComPtr<ID3D12Debug> debug;
    const bool debugLayer = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    if (debugLayer)
        debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> warp;
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    run(device.Get(), 2560, 1440, 2558, 1439, false, true);
    run(device.Get(), 2560, 1440, 2558, 1439, true, true);
    run(device.Get(), 3840, 2160, 2227, 1253, false, true);
    run(device.Get(), 1920, 1080, 1920, 1080, false, true);
    run(device.Get(), 2560, 1440, 2558, 1439, false, false);
    // CONTROL Resonant supplies a full mip chain; NR copies only mip 0 into its single-mip scratch.
    run(device.Get(), 2258, 1270, 2258, 1270, false, false, 12, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ComPtr<ID3D12InfoQueue> messages;
    if (debugLayer && SUCCEEDED(device.As(&messages)))
    {
        for (UINT64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i)
        {
            SIZE_T size = 0;
            check(messages->GetMessage(i, nullptr, &size));
            std::vector<unsigned char> storage(size);
            auto* message = (D3D12_MESSAGE*) storage.data();
            check(messages->GetMessage(i, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            {
                std::fprintf(stderr, "%s\n", message->pDescription);
                throw std::runtime_error("D3D12 validation warning/error");
            }
        }
    }
    std::printf("PASS: extent guards and active-region GPU copies (WARP, debug layer %s)\n",
                debugLayer ? "on" : "unavailable");
    return 0;
}
catch (const std::exception& e)
{
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}

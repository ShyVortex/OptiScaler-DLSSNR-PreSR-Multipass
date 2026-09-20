#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

// Definitions matching DirectX 12 and DXGI enums
enum DXGI_FORMAT : uint32_t
{
    DXGI_FORMAT_UNKNOWN = 0,
    DXGI_FORMAT_R32G32B32A32_TYPELESS = 1,
    DXGI_FORMAT_R32G32B32A32_FLOAT = 2,
    DXGI_FORMAT_R16G16B16A16_TYPELESS = 9,
    DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
    DXGI_FORMAT_R16G16B16A16_UNORM = 11,
    DXGI_FORMAT_R32G32_TYPELESS = 15,
    DXGI_FORMAT_R32G32_FLOAT = 16,
    DXGI_FORMAT_R10G10B10A2_TYPELESS = 23,
    DXGI_FORMAT_R10G10B10A2_UNORM = 24,
    DXGI_FORMAT_R8G8B8A8_TYPELESS = 27,
    DXGI_FORMAT_R8G8B8A8_UNORM = 28,
    DXGI_FORMAT_R16G16_TYPELESS = 33,
    DXGI_FORMAT_R16G16_FLOAT = 34,
    DXGI_FORMAT_R32_TYPELESS = 39,
    DXGI_FORMAT_R32_FLOAT = 40,
    DXGI_FORMAT_R24G8_TYPELESS = 44,
    DXGI_FORMAT_R24_UNORM_X8_TYPELESS = 46,
    DXGI_FORMAT_R16_TYPELESS = 53,
    DXGI_FORMAT_R16_UNORM = 56,
    DXGI_FORMAT_R32G8X24_TYPELESS = 19,
    DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS = 20,
};

enum D3D12_RESOURCE_DIMENSION : uint32_t
{
    D3D12_RESOURCE_DIMENSION_UNKNOWN = 0,
    D3D12_RESOURCE_DIMENSION_BUFFER = 1,
    D3D12_RESOURCE_DIMENSION_TEXTURE1D = 2,
    D3D12_RESOURCE_DIMENSION_TEXTURE2D = 3,
    D3D12_RESOURCE_DIMENSION_TEXTURE3D = 4
};

enum D3D12_RESOURCE_FLAGS : uint32_t
{
    D3D12_RESOURCE_FLAG_NONE = 0,
    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET = 0x1,
    D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL = 0x2,
    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS = 0x4,
    D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE = 0x8,
    D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER = 0x10,
    D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS = 0x20
};

enum D3D12_HEAP_TYPE : uint32_t
{
    D3D12_HEAP_TYPE_DEFAULT = 1,
    D3D12_HEAP_TYPE_UPLOAD = 2,
    D3D12_HEAP_TYPE_READBACK = 3,
    D3D12_HEAP_TYPE_CUSTOM = 4
};

enum D3D12_HEAP_FLAGS : uint32_t
{
    D3D12_HEAP_FLAG_NONE = 0,
    D3D12_HEAP_FLAG_SHARED = 0x1,
    D3D12_HEAP_FLAG_DENY_BUFFERS = 0x4,
    D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES = 0,
    D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES = 0x40,
    D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES = 0x80
};

struct D3D12_HEAP_PROPERTIES
{
    D3D12_HEAP_TYPE Type;
    uint32_t CPUPageProperty;
    uint32_t MemoryPoolPreference;
    uint32_t CreationNodeMask;
    uint32_t VisibleNodeMask;
};

struct DXGI_SAMPLE_DESC
{
    uint32_t Count;
    uint32_t Quality;
};

struct D3D12_RESOURCE_DESC
{
    D3D12_RESOURCE_DIMENSION Dimension;
    uint64_t Alignment;
    uint64_t Width;
    uint32_t Height;
    uint16_t DepthOrArraySize;
    uint16_t MipLevels;
    DXGI_FORMAT Format;
    DXGI_SAMPLE_DESC SampleDesc;
    uint32_t Layout;
    uint32_t Flags;
};

// Simulation of DlssNr_Dx12::State::TypedGuideFormat
DXGI_FORMAT SimulatedTypedGuideFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return f;
    }
}

// Mock ID3D12Resource
struct MockD3D12Resource
{
    D3D12_RESOURCE_DESC desc;
    bool getHeapPropertiesFails = false;
    D3D12_HEAP_PROPERTIES heapProperties { D3D12_HEAP_TYPE_DEFAULT, 0, 0, 0, 0 };
    D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE;

    const D3D12_RESOURCE_DESC& GetDesc() const { return desc; }

    int32_t GetHeapProperties(D3D12_HEAP_PROPERTIES* pProps, D3D12_HEAP_FLAGS* pFlags) const
    {
        if (getHeapPropertiesFails)
            return -1; // E_FAIL
        if (pProps) *pProps = heapProperties;
        if (pFlags) *pFlags = heapFlags;
        return 0; // S_OK
    }
};

// Mock ID3D12Device
struct MockD3D12Device
{
    bool failCustomHeap = true; // Simulates driver rejecting committed resources with certain heap types/flags
    int createCommittedCallCount = 0;
    D3D12_RESOURCE_DESC lastCreatedDesc {};
    D3D12_HEAP_PROPERTIES lastCreatedHeapProps {};
    D3D12_HEAP_FLAGS lastCreatedHeapFlags {};

    int32_t CreateCommittedResource(const D3D12_HEAP_PROPERTIES* pHeapProperties,
                                   D3D12_HEAP_FLAGS HeapFlags,
                                   const D3D12_RESOURCE_DESC* pDesc,
                                   uint32_t InitialResourceState,
                                   const void* pOptimizedClearValue,
                                   void** ppvResource)
    {
        createCommittedCallCount++;
        if (pHeapProperties && (pHeapProperties->Type == D3D12_HEAP_TYPE_CUSTOM ||
                               (HeapFlags & (D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES | D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES))))
        {
            if (failCustomHeap)
                return -2147024809; // E_INVALIDARG
        }

        lastCreatedDesc = *pDesc;
        lastCreatedHeapProps = *pHeapProperties;
        lastCreatedHeapFlags = HeapFlags;
        static int dummyRes = 42;
        if (ppvResource) *ppvResource = &dummyRes;
        return 0; // S_OK
    }
};

// Simulation of CreateBufferResource pipeline logic
bool SimulateCreateBufferResource(MockD3D12Device* device, MockD3D12Resource* source, void** outBuffer)
{
    if (device == nullptr || source == nullptr)
        return false;

    auto desc = source->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
        desc.DepthOrArraySize != 1)
        return false;
    desc.MipLevels = 1;
    desc.Alignment = 0;
    desc.Layout = 0; // D3D12_TEXTURE_LAYOUT_UNKNOWN

    // Format normalization: typeless to typed
    desc.Format = SimulatedTypedGuideFormat(desc.Format);

    // Flag sanitization: strip DEPTH_STENCIL and DENY_SHADER_RESOURCE, ensure UAV
    desc.Flags = (desc.Flags | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &
                 ~(D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

    D3D12_HEAP_PROPERTIES heapProps { D3D12_HEAP_TYPE_DEFAULT, 0, 0, 0, 0 };
    D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE;

    if (source->GetHeapProperties(&heapProps, &heapFlags) != 0)
    {
        heapProps = D3D12_HEAP_PROPERTIES { D3D12_HEAP_TYPE_DEFAULT, 0, 0, 0, 0 };
        heapFlags = D3D12_HEAP_FLAG_NONE;
    }
    else
    {
        heapFlags = static_cast<D3D12_HEAP_FLAGS>(
            heapFlags & ~(D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES | D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES | D3D12_HEAP_FLAG_DENY_BUFFERS));
    }

    int32_t hr = device->CreateCommittedResource(&heapProps, heapFlags, &desc, 0, nullptr, outBuffer);
    if (hr != 0)
    {
        if (heapProps.Type != D3D12_HEAP_TYPE_DEFAULT || heapFlags != D3D12_HEAP_FLAG_NONE)
        {
            heapProps = D3D12_HEAP_PROPERTIES { D3D12_HEAP_TYPE_DEFAULT, 0, 0, 0, 0 };
            heapFlags = D3D12_HEAP_FLAG_NONE;
            hr = device->CreateCommittedResource(&heapProps, heapFlags, &desc, 0, nullptr, outBuffer);
        }
    }

    return (hr == 0);
}

int main()
{
    std::cout << "Running DLSS-NR Buffer Resource Creation Unit Tests...\n";

    // Test 1: Typeless source normalization
    {
        MockD3D12Device device;
        MockD3D12Resource source;
        source.desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        source.desc.Width = 2560;
        source.desc.Height = 1440;
        source.desc.DepthOrArraySize = 1;
        source.desc.MipLevels = 1;
        source.desc.SampleDesc = { 1, 0 };
        source.desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        source.desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        void* outBuffer = nullptr;
        bool ok = SimulateCreateBufferResource(&device, &source, &outBuffer);
        assert(ok);
        assert(outBuffer != nullptr);
        assert(device.lastCreatedDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
        assert((device.lastCreatedDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0);
        assert((device.lastCreatedDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0);
        std::cout << "  [PASS] Test 1: Typeless R8G8B8A8 format normalized to UNORM with UAV flag\n";
    }

    // Test 2: Stripping conflicting DEPTH_STENCIL flag
    {
        MockD3D12Device device;
        MockD3D12Resource source;
        source.desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        source.desc.Width = 3840;
        source.desc.Height = 2160;
        source.desc.DepthOrArraySize = 1;
        source.desc.MipLevels = 1;
        source.desc.SampleDesc = { 1, 0 };
        source.desc.Format = DXGI_FORMAT_R16G16B16A16_TYPELESS;
        source.desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

        void* outBuffer = nullptr;
        bool ok = SimulateCreateBufferResource(&device, &source, &outBuffer);
        assert(ok);
        assert(device.lastCreatedDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
        assert((device.lastCreatedDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0);
        assert((device.lastCreatedDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0);
        assert((device.lastCreatedDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0);
        std::cout << "  [PASS] Test 2: Conflicting DEPTH_STENCIL and DENY_SHADER_RESOURCE stripped cleanly\n";
    }

    // Test 3: Heap property query failure falls back to D3D12_HEAP_TYPE_DEFAULT
    {
        MockD3D12Device device;
        MockD3D12Resource source;
        source.desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        source.desc.Width = 1920;
        source.desc.Height = 1080;
        source.desc.DepthOrArraySize = 1;
        source.desc.MipLevels = 1;
        source.desc.SampleDesc = { 1, 0 };
        source.desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        source.desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        source.getHeapPropertiesFails = true; // e.g. VKD3D or imported resource where GetHeapProperties fails

        void* outBuffer = nullptr;
        bool ok = SimulateCreateBufferResource(&device, &source, &outBuffer);
        assert(ok);
        assert(device.createCommittedCallCount == 1);
        assert(device.lastCreatedHeapProps.Type == D3D12_HEAP_TYPE_DEFAULT);
        assert(device.lastCreatedHeapFlags == D3D12_HEAP_FLAG_NONE);
        std::cout << "  [PASS] Test 3: GetHeapProperties failure safely defaults to HEAP_TYPE_DEFAULT\n";
    }

    // Test 4: Custom heap failure retry logic
    {
        MockD3D12Device device;
        device.failCustomHeap = true;

        MockD3D12Resource source;
        source.desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        source.desc.Width = 1920;
        source.desc.Height = 1080;
        source.desc.DepthOrArraySize = 1;
        source.desc.MipLevels = 1;
        source.desc.SampleDesc = { 1, 0 };
        source.desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        source.desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        source.heapProperties.Type = D3D12_HEAP_TYPE_CUSTOM; // Will fail first attempt in mock device

        void* outBuffer = nullptr;
        bool ok = SimulateCreateBufferResource(&device, &source, &outBuffer);
        assert(ok);
        assert(device.createCommittedCallCount == 2); // Initial failed, then succeeded on fallback retry
        assert(device.lastCreatedHeapProps.Type == D3D12_HEAP_TYPE_DEFAULT);
        std::cout << "  [PASS] Test 4: Custom heap failure successfully retried with HEAP_TYPE_DEFAULT\n";
    }

    // Test 5: Rejection of invalid dimensions
    {
        MockD3D12Device device;
        MockD3D12Resource source;
        source.desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D; // Reject 3D
        source.desc.Width = 1920;
        source.desc.Height = 1080;
        source.desc.DepthOrArraySize = 1;
        source.desc.MipLevels = 1;
        source.desc.SampleDesc = { 1, 0 };

        void* outBuffer = nullptr;
        bool ok = SimulateCreateBufferResource(&device, &source, &outBuffer);
        assert(!ok);

        source.desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        source.desc.SampleDesc = { 4, 0 }; // Reject MSAA
        ok = SimulateCreateBufferResource(&device, &source, &outBuffer);
        assert(!ok);

        std::cout << "  [PASS] Test 5: Graceful rejection of non-2D or MSAA resources\n";
    }

    // Test 6: Multi-mip source input (MipLevels > 1) successfully normalized to single-mip scratch buffer
    {
        MockD3D12Device device;
        MockD3D12Resource source;
        source.desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        source.desc.Width = 2560;
        source.desc.Height = 1440;
        source.desc.DepthOrArraySize = 1;
        source.desc.MipLevels = 4; // Multi-mip input texture
        source.desc.SampleDesc = { 1, 0 };
        source.desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        source.desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        void* outBuffer = nullptr;
        bool ok = SimulateCreateBufferResource(&device, &source, &outBuffer);
        assert(ok);
        assert(device.lastCreatedDesc.MipLevels == 1);
        assert(device.lastCreatedDesc.Width == 2560);
        assert(device.lastCreatedDesc.Height == 1440);
        assert((device.lastCreatedDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0);
        std::cout << "  [PASS] Test 6: Multi-mip source input successfully normalized to single-mip scratch buffer\n";
    }

    std::cout << "\nAll DLSS-NR Buffer Resource Creation Unit Tests passed successfully!\n";
    return 0;
}

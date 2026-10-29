#include "pch.h"
#include "DR_Dx12.h"

#include "DR_Common.h"

#include <State.h>
#include "precompiled/DR_Shader.h"

bool DR_Dx12::CreateBufferResource(ID3D12Device* InDevice, ID3D12Resource* InSource, uint32_t InWidth,
                                   uint32_t InHeight, D3D12_RESOURCE_STATES InState)
{
    if (InDevice == nullptr || InSource == nullptr || InWidth == 0 || InHeight == 0)
        return false;

    if (_buffer != nullptr)
    {
        const auto bufDesc = _buffer->GetDesc();
        if (bufDesc.Width == InWidth && bufDesc.Height == InHeight)
            return true;

        LOG_DEBUG("[{0}] Release {1}x{2}, new one: {3}x{4}", _name, bufDesc.Width, bufDesc.Height, InWidth, InHeight);
        SAFE_RELEASE(_buffer);
    }

    // A fresh description rather than a copy of the depth source: a depth-stencil source would carry
    // ALLOW_DEPTH_STENCIL / DENY_SHADER_RESOURCE, which cannot be combined with UAV access.
    const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_FLOAT, InWidth, InHeight, 1, 1, 1, 0,
                                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    D3D12_HEAP_PROPERTIES heapProperties {};
    D3D12_HEAP_FLAGS heapFlags {};
    if (InSource->GetHeapProperties(&heapProperties, &heapFlags) != S_OK)
    {
        LOG_DEBUG("[{0}] GetHeapProperties failed, using the default heap", _name);
        heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    }

    const auto hr = InDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &desc, InState, nullptr,
                                                      IID_PPV_ARGS(&_buffer));
    if (hr != S_OK)
    {
        LOG_ERROR("[{0}] CreateCommittedResource result: {1:X}", _name, (UINT64) hr);
        _buffer = nullptr;
        return false;
    }

    _buffer->SetName(L"Resampled_Depth_Buffer");
    _bufferState = InState;
    LOG_DEBUG("[{0}] Created {1}x{2}", _name, InWidth, InHeight);
    return true;
}

void DR_Dx12::SetBufferState(ID3D12GraphicsCommandList* InCommandList, D3D12_RESOURCE_STATES InState)
{
    return Shader_Dx12::SetBufferState(InCommandList, InState, _buffer, &_bufferState);
}

bool DR_Dx12::Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InResource, ID3D12Resource* OutResource,
                       uint32_t SourceWidth, uint32_t SourceHeight, uint32_t TargetWidth, uint32_t TargetHeight)
{
    if (!_init || _device == nullptr || InCmdList == nullptr || InResource == nullptr || OutResource == nullptr ||
        SourceWidth == 0 || SourceHeight == 0 || TargetWidth == 0 || TargetHeight == 0)
        return false;

    LOG_DEBUG("[{0}] {1}x{2} -> {3}x{4}", _name, SourceWidth, SourceHeight, TargetWidth, TargetHeight);

    _counter++;
    _counter = _counter % DR_NUM_OF_HEAPS;
    FrameDescriptorHeap& currentHeap = _frameHeaps[_counter];

    CreateShaderResourceView(_device, InResource, currentHeap.GetSrvCPU(0));
    CreateUnorderedAccessView(_device, OutResource, currentHeap.GetUavCPU(0), 0);

    DRConstants constants {};
    constants.SourceWidth = SourceWidth;
    constants.SourceHeight = SourceHeight;
    constants.TargetWidth = TargetWidth;
    constants.TargetHeight = TargetHeight;

    if (!CreateConstantsBuffer(_device, _constantBuffer, constants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);

    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(_pipelineState);

    InCmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    const UINT dispatchWidth = (TargetWidth + InNumThreadsX - 1) / InNumThreadsX;
    const UINT dispatchHeight = (TargetHeight + InNumThreadsY - 1) / InNumThreadsY;
    InCmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    return true;
}

DR_Dx12::DR_Dx12(std::string InName, ID3D12Device* InDevice) : Shader_Dx12(InName, InDevice)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    LOG_DEBUG("{0} start!", _name);

    if (!SetupRootSignature(InDevice, 1, 1, 1))
    {
        LOG_ERROR("Failed to setup root signature");
        return;
    }

    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(DRConstants));
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    auto result =
        InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                          nullptr, IID_PPV_ARGS(&_constantBuffer));

    if (result != S_OK)
    {
        LOG_ERROR("[{0}] CreateCommittedResource error {1:x}", _name, (unsigned int) result);
        return;
    }

    if (!CreateComputePipeline(InDevice, &_pipelineState, DR_cso, sizeof(DR_cso), drShaderCode.c_str()))
    {
        LOG_ERROR("[{0}] Failed to create compute pipeline", _name);
        return;
    }

    _init = InitHeaps(InDevice, _frameHeaps, DR_NUM_OF_HEAPS);
}

DR_Dx12::~DR_Dx12()
{
    if (!_init || State::Instance().isShuttingDown)
        return;

    for (int i = 0; i < DR_NUM_OF_HEAPS; i++)
    {
        _frameHeaps[i].ReleaseHeaps();
    }

    SAFE_RELEASE(_buffer);
}

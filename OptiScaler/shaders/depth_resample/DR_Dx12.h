#pragma once

#include "SysUtils.h"

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12Utils.h>
#include <shaders/Shader_Dx12.h>

#define DR_NUM_OF_HEAPS 2

// Copies depth to another resolution with nearest sampling (see DR_Common.h). Used to hand XeFG depth at the
// resolution of display-resolution motion vectors without touching the depth values.
class DR_Dx12 : public Shader_Dx12
{
  private:
    FrameDescriptorHeap _frameHeaps[DR_NUM_OF_HEAPS];

    ID3D12Resource* _buffer = nullptr;
    D3D12_RESOURCE_STATES _bufferState = D3D12_RESOURCE_STATE_COMMON;

    uint32_t InNumThreadsX = 16;
    uint32_t InNumThreadsY = 16;

  public:
    // R32_FLOAT, UAV-capable target of the given size; recreated only when the size changes.
    bool CreateBufferResource(ID3D12Device* InDevice, ID3D12Resource* InSource, uint32_t InWidth, uint32_t InHeight,
                              D3D12_RESOURCE_STATES InState);
    void SetBufferState(ID3D12GraphicsCommandList* InCommandList, D3D12_RESOURCE_STATES InState);

    // Reads the SourceWidth x SourceHeight region of InResource and fills TargetWidth x TargetHeight of OutResource.
    bool Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InResource, ID3D12Resource* OutResource,
                  uint32_t SourceWidth, uint32_t SourceHeight, uint32_t TargetWidth, uint32_t TargetHeight);

    ID3D12Resource* Buffer() { return _buffer; }
    bool CanRender() const { return _init && _buffer != nullptr; }

    DR_Dx12(std::string InName, ID3D12Device* InDevice);

    ~DR_Dx12();
};

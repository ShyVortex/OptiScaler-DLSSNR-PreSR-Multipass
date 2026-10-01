#pragma once
// CPU-only interfaces: no D3D12 library is linked and no D3D12 device is created.
#include "pch.h"

enum D3D12_FENCE_FLAGS
{
    D3D12_FENCE_FLAG_NONE = 0
};
struct __declspec(uuid("71111111-1111-1111-1111-111111111111")) ID3D12Fence : IUnknown
{
    virtual UINT64 GetCompletedValue() = 0;
};
struct __declspec(uuid("72222222-2222-2222-2222-222222222222")) ID3D12Device : IUnknown
{
    virtual HRESULT CreateFence(UINT64, D3D12_FENCE_FLAGS, REFIID, void**) = 0;
};
struct __declspec(uuid("73333333-3333-3333-3333-333333333333")) ID3D12CommandQueue : IUnknown
{
    virtual HRESULT GetDevice(REFIID, void**) = 0;
    virtual HRESULT Signal(ID3D12Fence*, UINT64) = 0;
};
struct ID3D12CommandList : IUnknown
{
};
struct ID3D12GraphicsCommandList : ID3D12CommandList
{
    virtual HRESULT SetPrivateDataInterface(REFGUID, const IUnknown*) = 0;
};

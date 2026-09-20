#pragma once
// Narrow CPU-only boundary for the production timer header. No D3D12 library is linked.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <unknwn.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

enum D3D12_QUERY_HEAP_TYPE
{
    D3D12_QUERY_HEAP_TYPE_TIMESTAMP
};
enum D3D12_QUERY_TYPE
{
    D3D12_QUERY_TYPE_TIMESTAMP
};
enum D3D12_HEAP_TYPE
{
    D3D12_HEAP_TYPE_READBACK
};
enum D3D12_HEAP_FLAGS
{
    D3D12_HEAP_FLAG_NONE
};
enum D3D12_RESOURCE_STATES
{
    D3D12_RESOURCE_STATE_COPY_DEST
};
enum D3D12_FENCE_FLAGS
{
    D3D12_FENCE_FLAG_NONE
};
struct D3D12_QUERY_HEAP_DESC
{
    D3D12_QUERY_HEAP_TYPE Type;
    UINT Count;
    UINT NodeMask;
};
struct D3D12_RANGE
{
    SIZE_T Begin;
    SIZE_T End;
};
struct CD3DX12_HEAP_PROPERTIES
{
    D3D12_HEAP_TYPE Type;
    explicit CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE type) : Type(type) {}
};
struct CD3DX12_RESOURCE_DESC
{
    UINT64 Width;
    static CD3DX12_RESOURCE_DESC Buffer(UINT64 bytes) { return { bytes }; }
};
struct __declspec(uuid("81111111-1111-1111-1111-111111111111")) ID3D12Fence : IUnknown
{
    virtual UINT64 GetCompletedValue() = 0;
    virtual HRESULT SetEventOnCompletion(UINT64, HANDLE) { return E_NOTIMPL; }
};
struct __declspec(uuid("82222222-2222-2222-2222-222222222222")) ID3D12QueryHeap : IUnknown
{
};
struct __declspec(uuid("83333333-3333-3333-3333-333333333333")) ID3D12Resource : IUnknown
{
    virtual HRESULT Map(UINT, const D3D12_RANGE*, void**) = 0;
    virtual void Unmap(UINT, const D3D12_RANGE*) = 0;
};
struct __declspec(uuid("84444444-4444-4444-4444-444444444444")) ID3D12Device : IUnknown
{
    virtual HRESULT CreateQueryHeap(const D3D12_QUERY_HEAP_DESC*, REFIID, void**) = 0;
    virtual HRESULT CreateCommittedResource(const CD3DX12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                            const CD3DX12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const void*, REFIID,
                                            void**) = 0;
    virtual HRESULT CreateFence(UINT64, D3D12_FENCE_FLAGS, REFIID, void**) = 0;
};
struct ID3D12CommandList : IUnknown
{
};
struct ID3D12GraphicsCommandList : ID3D12CommandList
{
    virtual HRESULT SetPrivateDataInterface(REFGUID, const IUnknown*) = 0;
    virtual void EndQuery(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT) = 0;
    virtual void ResolveQueryData(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT, UINT, ID3D12Resource*, UINT64) = 0;
};
struct ID3D12CommandQueue : IUnknown
{
    virtual HRESULT GetDevice(REFIID, void**) = 0;
    virtual HRESULT GetTimestampFrequency(UINT64*) = 0;
    virtual HRESULT Signal(ID3D12Fence*, UINT64) = 0;
};
namespace Util
{
inline bool CheckForRealObject(const char*, IUnknown*, IUnknown**) { return false; }
} // namespace Util

// Compiles both production getter bodies. D3D allocation/reset/submission are
// controlled CPU seams, so failures do not create a device or execute GPU work.
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstddef>
#include <initializer_list>

#define LOG_DEBUG(...) ((void) 0)
#define LOG_ERROR(...) ((void) 0)
constexpr int BUFFER_COUNT = 4;
struct ID3D12Device
{
};
struct ID3D12CommandAllocator
{
    HRESULT result = S_OK;
    unsigned calls = 0;
    HRESULT Reset()
    {
        ++calls;
        return result;
    }
};
struct ID3D12GraphicsCommandList
{
    HRESULT result = S_OK;
    unsigned calls = 0;
    HRESULT Reset(ID3D12CommandAllocator*, void*)
    {
        ++calls;
        return result;
    }
    HRESULT Close() { return S_OK; }
};
struct State
{
    ID3D12Device* currentD3D12Device = nullptr;
    static State& Instance()
    {
        static State state;
        return state;
    }
};
struct IFGFeature_Dx12
{
    ID3D12Device* _device = nullptr;
    ID3D12CommandAllocator* _uiCommandAllocator[BUFFER_COUNT] {};
    ID3D12CommandAllocator* _scCommandAllocator[BUFFER_COUNT] {};
    ID3D12GraphicsCommandList* _uiCommandList[BUFFER_COUNT] {};
    ID3D12GraphicsCommandList* _scCommandList[BUFFER_COUNT] {};
    bool _uiCommandListResetted[BUFFER_COUNT] {};
    bool _scCommandListResetted[BUFFER_COUNT] {};
    UINT64 _uiAllocatorFenceValues[BUFFER_COUNT] {}, _uiFenceValue = 0;
    unsigned creates = 0;
    int GetIndex() { return 0; }
    void CreateObjects(ID3D12Device*) { ++creates; } // Inject failed/partial creation.
    bool SubmitUICommandList(UINT) { return true; }
    bool WaitForUIAllocator(UINT) { return true; }
    ID3D12GraphicsCommandList* GetUICommandList(int);
    ID3D12GraphicsCommandList* GetSCCommandList(int);
};
#include "production-getters.inc"

static unsigned checks = 0, failures = 0;
static void Expect(bool condition, const char* message)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}
static bool GetWithoutFault(IFGFeature_Dx12* feature, bool sc, ID3D12GraphicsCommandList** output)
{
    __try
    {
        *output = sc ? feature->GetSCCommandList(2) : feature->GetUICommandList(2);
        return true;
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        return false;
    }
}
int main()
{
    ID3D12Device device;
    for (const bool sc : { false, true })
    {
        for (const auto scenario : { 0, 1, 2, 3, 4, 5 })
        {
            IFGFeature_Dx12 feature;
            feature._device = &device;
            ID3D12CommandAllocator allocator;
            ID3D12GraphicsCommandList list;
            for (int index = 0; index < BUFFER_COUNT; ++index)
            {
                feature._uiCommandAllocator[index] = feature._scCommandAllocator[index] = &allocator;
                feature._uiCommandList[index] = feature._scCommandList[index] = &list;
            }
            auto& allocators = sc ? feature._scCommandAllocator : feature._uiCommandAllocator;
            auto& lists = sc ? feature._scCommandList : feature._uiCommandList;
            // Missing selected allocator/list, incomplete creation, each Reset failure, success.
            if (scenario == 0)
                allocators[2] = nullptr;
            if (scenario == 1)
                lists[2] = nullptr;
            if (scenario == 2)
                allocators[0] = allocators[2] = nullptr;
            if (scenario == 3)
                allocator.result = E_FAIL;
            if (scenario == 4)
                list.result = E_FAIL;
            ID3D12GraphicsCommandList* output = nullptr;
            const bool safe = GetWithoutFault(&feature, sc, &output);
            const bool recording = sc ? feature._scCommandListResetted[2] : feature._uiCommandListResetted[2];
            if (scenario < 5)
            {
                Expect(safe && output == nullptr && !recording,
                       "unavailable/reset-failed slot must return null without a fault or recording state");
                if (scenario < 3)
                    Expect(allocator.calls == 0 && list.calls == 0, "incomplete slot must not attempt either reset");
                if (scenario == 2)
                    Expect(feature.creates == 1, "missing initial slot must attempt creation before failing safely");
                if (scenario == 3)
                    Expect(list.calls == 0, "failed allocator reset must not reset its command list");
            }
            else
            {
                Expect(safe && output == &list && recording, "successful reset must return a recording list");
                Expect(allocator.calls == 1 && list.calls == 1, "fresh recording must reset allocator and list once");
                output = nullptr;
                Expect(GetWithoutFault(&feature, sc, &output) && output == &list && allocator.calls == 1 &&
                           list.calls == 1,
                       "reusing the open recording must not reset either object again");
            }
        }
    }
    std::printf("%u checks, %u failures; CPU only\n", checks, failures);
    return failures ? 1 : 0;
}

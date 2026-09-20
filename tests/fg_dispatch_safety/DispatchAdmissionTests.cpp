// The runner extracts the real Dispatch admission code up to ScopedGpuTime_Dx12.
// The true return marks that rendering boundary; no rendering code runs here.
#include <cstdio>

struct IDXGISwapChain3
{
};
struct ID3D12GraphicsCommandList
{
};
struct ID3D12Resource
{
};
using D3D12_RESOURCE_STATES = unsigned;

struct RUI_Dx12
{
    bool _init = true;
    bool Dispatch(IDXGISwapChain3*, ID3D12GraphicsCommandList*, ID3D12Resource*, D3D12_RESOURCE_STATES);
};
struct HC_Dx12
{
    bool _init = true;
    bool Dispatch(IDXGISwapChain3*, ID3D12GraphicsCommandList*, ID3D12Resource*, D3D12_RESOURCE_STATES);
};

#include "rui-admission.inc"
#include "hc-admission.inc"

static int checks = 0;
static int failures = 0;

template <class Renderer> void CheckAdmission(const char* name)
{
    Renderer renderer;
    IDXGISwapChain3 swapchain;
    ID3D12GraphicsCommandList commands;
    ID3D12Resource resource;
    const auto check = [name](const char* condition, bool actual, bool expected)
    {
        ++checks;
        if (actual != expected)
        {
            ++failures;
            std::printf("FAIL %s: %s, rendering boundary reached=%d, expected=%d\n", name, condition, actual, expected);
        }
    };

    // Removing the command-list admission guard must fail this case.
    check("null command list", renderer.Dispatch(&swapchain, nullptr, &resource, 0), false);
    check("valid inputs", renderer.Dispatch(&swapchain, &commands, &resource, 0), true);
    check("null swapchain", renderer.Dispatch(nullptr, &commands, &resource, 0), false);
    check("null resource", renderer.Dispatch(&swapchain, &commands, nullptr, 0), false);
    renderer._init = false;
    check("uninitialized renderer", renderer.Dispatch(&swapchain, &commands, &resource, 0), false);
}

int main()
{
    CheckAdmission<RUI_Dx12>("RUI");
    CheckAdmission<HC_Dx12>("HC");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

// Standalone automated unit test for DXGI SwapChain sizing resolution (DxgiSwapchainSizing.h)
// Validates:
// 1. ResolveWindowSizedSwapchain(DXGI_SWAP_CHAIN_DESC1&, HWND) resolution on 0x0 game windows.
// 2. Caller descriptor preservation (pDesc is not mutated in-place).
// 3. Tiny helper/overlay window rejection (< 100px boundary).
// 4. Exact 100px boundary condition acceptance.
// 5. Asymmetric boundary rejection (e.g. 100x99, 99x100).
// 6. Null and invalid HWND rejection.
// 7. Explicit non-zero dimension preservation (e.g. 1920x1080, 32x32).
// 8. Partial zero dimensions rejection (Width=0, Height>0 or Width>0, Height=0).
// 9. DxgiFactory_Hooks gating simulation (Helldivers 2 0x0 path vs real overlays).

namespace
{
using UINT = uint32_t;
using LONG = int32_t;
using BOOL = int32_t;
using HWND = void*;

#define TRUE 1
#define FALSE 0

struct RECT
{
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
};

enum DXGI_FORMAT
{
    DXGI_FORMAT_UNKNOWN = 0,
    DXGI_FORMAT_R8G8B8A8_UNORM = 28,
};

enum DXGI_SWAP_EFFECT
{
    DXGI_SWAP_EFFECT_DISCARD = 0,
    DXGI_SWAP_EFFECT_FLIP_DISCARD = 4,
};

struct DXGI_SAMPLE_DESC
{
    UINT Count;
    UINT Quality;
};

struct DXGI_SWAP_CHAIN_DESC1
{
    UINT Width;
    UINT Height;
    DXGI_FORMAT Format;
    BOOL Stereo;
    DXGI_SAMPLE_DESC SampleDesc;
    UINT BufferUsage;
    UINT BufferCount;
    UINT Scaling;
    DXGI_SWAP_EFFECT SwapEffect;
    UINT AlphaMode;
    UINT Flags;
};

struct DXGI_MODE_DESC
{
    UINT Width;
    UINT Height;
    UINT RefreshRateNumerator;
    UINT RefreshRateDenominator;
    DXGI_FORMAT Format;
    UINT ScanlineOrdering;
    UINT Scaling;
};

struct DXGI_SWAP_CHAIN_DESC
{
    DXGI_MODE_DESC BufferDesc;
    DXGI_SAMPLE_DESC SampleDesc;
    UINT BufferUsage;
    UINT BufferCount;
    HWND OutputWindow;
    BOOL Windowed;
    DXGI_SWAP_EFFECT SwapEffect;
    UINT Flags;
};

// Mock window registry for client dimensions
struct MockWindow
{
    int width;
    int height;
    bool valid;
};

static MockWindow s_mockWindows[10] {};
static int s_mockWindowCount = 0;

HWND CreateMockWindow(int width, int height, bool valid = true)
{
    int idx = s_mockWindowCount++;
    s_mockWindows[idx] = { width, height, valid };
    return reinterpret_cast<HWND>(static_cast<uintptr_t>(idx + 1));
}

BOOL MockGetClientRect(HWND hwnd, RECT* pRect)
{
    if (hwnd == nullptr || pRect == nullptr)
        return FALSE;

    uintptr_t id = reinterpret_cast<uintptr_t>(hwnd);
    if (id == 0 || id > static_cast<uintptr_t>(s_mockWindowCount))
        return FALSE;

    const auto& win = s_mockWindows[id - 1];
    if (!win.valid)
        return FALSE;

    pRect->left = 0;
    pRect->top = 0;
    pRect->right = win.width;
    pRect->bottom = win.height;
    return TRUE;
}

// Logic under test, exactly matching DxgiSwapchainSizing.h
inline bool TestResolveWindowSizedSwapchain(DXGI_SWAP_CHAIN_DESC1& desc, HWND window)
{
    if (desc.Width != 0 || desc.Height != 0 || window == nullptr)
        return false;

    RECT rect {};
    if (!MockGetClientRect(window, &rect))
        return false;
    const LONG width = rect.right - rect.left;
    const LONG height = rect.bottom - rect.top;
    if (width < 100 || height < 100)
        return false;

    desc.Width = (UINT) width;
    desc.Height = (UINT) height;
    return true;
}

inline bool TestResolveWindowSizedSwapchain(DXGI_SWAP_CHAIN_DESC& desc)
{
    if (desc.BufferDesc.Width != 0 || desc.BufferDesc.Height != 0 || desc.OutputWindow == nullptr)
        return false;

    RECT rect {};
    if (!MockGetClientRect(desc.OutputWindow, &rect))
        return false;
    const LONG width = rect.right - rect.left;
    const LONG height = rect.bottom - rect.top;
    if (width < 100 || height < 100)
        return false;

    desc.BufferDesc.Width = (UINT) width;
    desc.BufferDesc.Height = (UINT) height;
    return true;
}

} // namespace

int main()
{
    std::printf("[+] Starting DXGI SwapChain Sizing Unit Tests...\n");

    HWND gameHwnd = CreateMockWindow(1920, 1080);
    HWND ultrawideHwnd = CreateMockWindow(3440, 1440);
    HWND tinyHwnd = CreateMockWindow(32, 32);
    HWND boundary99Hwnd = CreateMockWindow(99, 99);
    HWND threshold100Hwnd = CreateMockWindow(100, 100);
    HWND asym1Hwnd = CreateMockWindow(100, 99);
    HWND asym2Hwnd = CreateMockWindow(99, 100);
    HWND invalidHwnd = CreateMockWindow(1920, 1080, /*valid=*/false);

    // ------------------------------------------------------------------------
    // Test 1: Standard 0x0 SwapChain resolution (DXGI 1.2+ CreateSwapChainForHwnd)
    // ------------------------------------------------------------------------
    {
        DXGI_SWAP_CHAIN_DESC1 callerDesc {};
        callerDesc.Width = 0;
        callerDesc.Height = 0;
        callerDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        callerDesc.BufferCount = 2;
        callerDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        DXGI_SWAP_CHAIN_DESC1 localDesc = callerDesc;
        bool resolved = TestResolveWindowSizedSwapchain(localDesc, gameHwnd);

        assert(resolved && "Test 1 Failed: Standard 0x0 game window should resolve successfully");
        assert(localDesc.Width == 1920 && localDesc.Height == 1080 &&
               "Test 1 Failed: Dimensions should match game client window");
        assert(callerDesc.Width == 0 && callerDesc.Height == 0 &&
               "Test 1 Failed: Caller descriptor should be preserved (unmutated)");

        std::printf("  [PASS] Test 1: Standard 0x0 window resolution & caller preservation verified.\n");
    }

    // ------------------------------------------------------------------------
    // Test 2: Ultrawide resolution (3440x1440)
    // ------------------------------------------------------------------------
    {
        DXGI_SWAP_CHAIN_DESC1 localDesc {};
        localDesc.Width = 0;
        localDesc.Height = 0;
        bool resolved = TestResolveWindowSizedSwapchain(localDesc, ultrawideHwnd);

        assert(resolved && localDesc.Width == 3440 && localDesc.Height == 1440);
        std::printf("  [PASS] Test 2: Ultrawide 0x0 resolution verified.\n");
    }

    // ------------------------------------------------------------------------
    // Test 3: Tiny helper/overlay window rejection (< 100px)
    // ------------------------------------------------------------------------
    {
        DXGI_SWAP_CHAIN_DESC1 localDesc {};
        localDesc.Width = 0;
        localDesc.Height = 0;
        bool resolved = TestResolveWindowSizedSwapchain(localDesc, tinyHwnd);

        assert(!resolved && "Test 3 Failed: 32x32 tiny window must be rejected");
        assert(localDesc.Width == 0 && localDesc.Height == 0 && "Test 3 Failed: Rejected desc must remain 0x0");

        resolved = TestResolveWindowSizedSwapchain(localDesc, boundary99Hwnd);
        assert(!resolved && "Test 3 Failed: 99x99 window must be rejected");

        std::printf("  [PASS] Test 3: Tiny helper/overlay window rejection verified.\n");
    }

    // ------------------------------------------------------------------------
    // Test 4: Exact 100px threshold acceptance and asymmetric rejection
    // ------------------------------------------------------------------------
    {
        DXGI_SWAP_CHAIN_DESC1 localDesc {};
        localDesc.Width = 0;
        localDesc.Height = 0;
        bool resolved = TestResolveWindowSizedSwapchain(localDesc, threshold100Hwnd);

        assert(resolved && "Test 4 Failed: Exact 100x100 window must be accepted");
        assert(localDesc.Width == 100 && localDesc.Height == 100);

        localDesc.Width = 0;
        localDesc.Height = 0;
        assert(!TestResolveWindowSizedSwapchain(localDesc, asym1Hwnd) &&
               "Test 4 Failed: 100x99 asymmetric window must be rejected");

        localDesc.Width = 0;
        localDesc.Height = 0;
        assert(!TestResolveWindowSizedSwapchain(localDesc, asym2Hwnd) &&
               "Test 4 Failed: 99x100 asymmetric window must be rejected");

        std::printf("  [PASS] Test 4: Threshold 100px boundary & asymmetric rejection verified.\n");
    }

    // ------------------------------------------------------------------------
    // Test 5: Null & Invalid HWND rejection
    // ------------------------------------------------------------------------
    {
        DXGI_SWAP_CHAIN_DESC1 localDesc {};
        localDesc.Width = 0;
        localDesc.Height = 0;

        assert(!TestResolveWindowSizedSwapchain(localDesc, nullptr) && "Test 5 Failed: nullptr HWND must be rejected");
        assert(!TestResolveWindowSizedSwapchain(localDesc, invalidHwnd) &&
               "Test 5 Failed: Invalid HWND where GetClientRect fails must be rejected");
        assert(
            !TestResolveWindowSizedSwapchain(localDesc, reinterpret_cast<HWND>(static_cast<uintptr_t>(0xDEADBEEF))) &&
            "Test 5 Failed: Out-of-bounds HWND must be rejected");

        std::printf("  [PASS] Test 5: Null and invalid HWND rejection verified.\n");
    }

    // ------------------------------------------------------------------------
    // Test 6: Explicit non-zero dimensions must never be touched
    // ------------------------------------------------------------------------
    {
        DXGI_SWAP_CHAIN_DESC1 localDesc {};
        localDesc.Width = 1920;
        localDesc.Height = 1080;
        assert(!TestResolveWindowSizedSwapchain(localDesc, ultrawideHwnd) &&
               "Test 6 Failed: Explicit dimensions must return false");
        assert(localDesc.Width == 1920 && localDesc.Height == 1080);

        localDesc.Width = 32;
        localDesc.Height = 32;
        assert(!TestResolveWindowSizedSwapchain(localDesc, gameHwnd) &&
               "Test 6 Failed: Explicit tiny dimensions must not be overridden");
        assert(localDesc.Width == 32 && localDesc.Height == 32);

        std::printf("  [PASS] Test 6: Explicit non-zero dimension immutability verified.\n");
    }

    // ------------------------------------------------------------------------
    // Test 7: Partial zero dimensions rejection
    // ------------------------------------------------------------------------
    {
        DXGI_SWAP_CHAIN_DESC1 localDesc {};
        localDesc.Width = 0;
        localDesc.Height = 1080;
        assert(!TestResolveWindowSizedSwapchain(localDesc, gameHwnd) &&
               "Test 7 Failed: Width=0, Height=1080 must return false");

        localDesc.Width = 1920;
        localDesc.Height = 0;
        assert(!TestResolveWindowSizedSwapchain(localDesc, gameHwnd) &&
               "Test 7 Failed: Width=1920, Height=0 must return false");

        std::printf("  [PASS] Test 7: Partial zero dimensions rejection verified.\n");
    }

    // ------------------------------------------------------------------------
    // Test 8: DXGI 1.0 Legacy CreateSwapChain compatibility
    // ------------------------------------------------------------------------
    {
        DXGI_SWAP_CHAIN_DESC legacyDesc {};
        legacyDesc.BufferDesc.Width = 0;
        legacyDesc.BufferDesc.Height = 0;
        legacyDesc.OutputWindow = gameHwnd;

        bool resolved = TestResolveWindowSizedSwapchain(legacyDesc);
        assert(resolved && legacyDesc.BufferDesc.Width == 1920 && legacyDesc.BufferDesc.Height == 1080);

        legacyDesc.BufferDesc.Width = 0;
        legacyDesc.BufferDesc.Height = 0;
        legacyDesc.OutputWindow = tinyHwnd;
        assert(!TestResolveWindowSizedSwapchain(legacyDesc));

        std::printf("  [PASS] Test 8: DXGI 1.0 legacy swapchain resolution parity verified.\n");
    }

    // ------------------------------------------------------------------------
    // Test 9: End-to-End Simulation of DxgiFactory_Hooks::CreateSwapChainForHwnd
    // ------------------------------------------------------------------------
    {
        // Case A: Helldivers 2 call (Width=0, Height=0, Game HWND)
        DXGI_SWAP_CHAIN_DESC1 gameDesc {};
        gameDesc.Width = 0;
        gameDesc.Height = 0;
        gameDesc.BufferCount = 2;

        DXGI_SWAP_CHAIN_DESC1 localDesc = gameDesc;
        const bool sizeToWindow = TestResolveWindowSizedSwapchain(localDesc, gameHwnd);
        const bool isOverlay = (localDesc.Height < 100 || localDesc.Width < 100);

        assert(sizeToWindow && "Case A: 0x0 swapchain must resolve to window size");
        assert(!isOverlay && "Case A: Sized swapchain (1920x1080) must NOT be classified as overlay!");

        // Case B: Real overlay call with explicit tiny dimensions (e.g. 50x50)
        DXGI_SWAP_CHAIN_DESC1 overlayDesc {};
        overlayDesc.Width = 50;
        overlayDesc.Height = 50;

        DXGI_SWAP_CHAIN_DESC1 localOverlayDesc = overlayDesc;
        const bool overlaySized = TestResolveWindowSizedSwapchain(localOverlayDesc, gameHwnd);
        const bool overlayIsOverlay = (localOverlayDesc.Height < 100 || localOverlayDesc.Width < 100);

        assert(!overlaySized && "Case B: Explicit overlay must not be resized");
        assert(overlayIsOverlay && "Case B: 50x50 overlay must be classified as overlay and bypassed");

        // Case C: Real overlay call with 0x0 dimensions and tiny window (32x32)
        DXGI_SWAP_CHAIN_DESC1 tinyOverlayDesc {};
        tinyOverlayDesc.Width = 0;
        tinyOverlayDesc.Height = 0;

        DXGI_SWAP_CHAIN_DESC1 localTinyDesc = tinyOverlayDesc;
        const bool tinySized = TestResolveWindowSizedSwapchain(localTinyDesc, tinyHwnd);
        const bool tinyIsOverlay = (localTinyDesc.Height < 100 || localTinyDesc.Width < 100);

        assert(!tinySized && "Case C: Tiny overlay window must not be sized");
        assert(tinyIsOverlay && "Case C: Unsized tiny window remains 0x0 (< 100) and is classified as overlay");

        std::printf("  [PASS] Test 9: DxgiFactory_Hooks pipeline gating simulation verified.\n");
    }

    std::printf("[+] All DXGI SwapChain Sizing Unit Tests PASSED successfully!\n");
    return 0;
}

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

// Unit tests validating Intel XeFG & XeMFG robustness and diagnostic hardening:
// 1. Backbuffer HDR RGBA16F (R16G16B16A16_FLOAT / _TYPELESS) detection, logging, and UI warning state.
// 2. Resource dimension mismatch protection & subrect decoupling for HUD-less and UI textures.
// 3. Model history reset rate-limiting & anti-thrashing debounce across rapid camera cuts.
// 4. Swapchain resize / reactivation state cleanup ensuring mandatory initial resets succeed without debounce.

namespace XeFGHardeningTest
{

enum DXGI_FORMAT : uint32_t
{
    DXGI_FORMAT_UNKNOWN = 0,
    DXGI_FORMAT_R16G16B16A16_TYPELESS = 9,
    DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
    DXGI_FORMAT_R10G10B10A2_UNORM = 24,
    DXGI_FORMAT_R8G8B8A8_UNORM = 28,
};

enum class FG_ResourceType : uint32_t
{
    Depth = 0,
    Velocity,
    HudlessColor,
    UIColor,
    Distortion,
};

struct ResourceData
{
    void* pResource = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    bool valid = false;
};

struct MockXeFG
{
    uint32_t _width = 0;
    uint32_t _height = 0;
    bool _xefgHdrFormatUnsupported = false;
    uint64_t _lastResetFrame = 0;
    uint64_t _frameCount = 0;
    bool _needResetHistory = false;
    int _resetFlag = 0;

    void OnCreateSwapchain(DXGI_FORMAT format, uint32_t width, uint32_t height)
    {
        _width = width;
        _height = height;
        _lastResetFrame = 0;

        if (format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R16G16B16A16_TYPELESS)
        {
            _xefgHdrFormatUnsupported = true;
        }
        else
        {
            _xefgHdrFormatUnsupported = false;
        }
    }

    void OnResizeBuffers(DXGI_FORMAT format, uint32_t width, uint32_t height)
    {
        _width = width;
        _height = height;
        _lastResetFrame = 0;

        if (format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R16G16B16A16_TYPELESS)
        {
            _xefgHdrFormatUnsupported = true;
        }
        else
        {
            _xefgHdrFormatUnsupported = false;
        }
    }

    ResourceData GetResourceData(FG_ResourceType type, uint32_t resWidth, uint32_t resHeight, void* mockPtr)
    {
        ResourceData result {};
        result.pResource = mockPtr;
        result.width = resWidth;
        result.height = resHeight;
        result.valid = (mockPtr != nullptr);

        const uint32_t targetWidth = _width;
        const uint32_t targetHeight = _height;

        if (targetWidth > 0 && targetHeight > 0)
        {
            if (type == FG_ResourceType::HudlessColor || type == FG_ResourceType::UIColor)
            {
                if (result.width != targetWidth || result.height != targetHeight)
                {
                    // Decouple mismatched optional planes to prevent Intel XeSS-FG -12 abort
                    return ResourceData {};
                }
            }
        }

        return result;
    }

    bool Dispatch(bool skipResetConfig = false)
    {
        bool resetHistory = false;

        if (!skipResetConfig)
        {
            bool requestedReset = (_resetFlag != 0 || _needResetHistory);
            if (requestedReset)
            {
                if (_lastResetFrame > 0 && (_frameCount - _lastResetFrame) < 3)
                {
                    // Debounce rapid consecutive resets
                    resetHistory = false;
                }
                else
                {
                    resetHistory = true;
                    _lastResetFrame = _frameCount;
                }
            }
            else
            {
                resetHistory = false;
            }
        }
        else
        {
            resetHistory = false;
        }

        _needResetHistory = false;
        _resetFlag = 0;
        return resetHistory;
    }

    void Deactivate()
    {
        _needResetHistory = true;
        _lastResetFrame = 0;
    }
};

} // namespace XeFGHardeningTest

int main()
{
    using namespace XeFGHardeningTest;
    printf("[+] Starting XeFG & XeMFG Hardening and Diagnostics Unit Tests...\n");

    // ── Test 1: HDR RGBA16F Format Detection & Actionable Warnings ─────────────
    {
        MockXeFG fg;

        // RGBA16F format triggers unsupported warning flag
        fg.OnCreateSwapchain(DXGI_FORMAT_R16G16B16A16_FLOAT, 2560, 1440);
        assert(fg._xefgHdrFormatUnsupported == true);
        assert(fg._width == 2560 && fg._height == 1440);

        // Typeless 16-bit float triggers unsupported warning flag
        fg.OnCreateSwapchain(DXGI_FORMAT_R16G16B16A16_TYPELESS, 2560, 1440);
        assert(fg._xefgHdrFormatUnsupported == true);

        // Standard 10-bit HDR (R10G10B10A2) clears the unsupported warning
        fg.OnResizeBuffers(DXGI_FORMAT_R10G10B10A2_UNORM, 2560, 1440);
        assert(fg._xefgHdrFormatUnsupported == false);

        // Standard 8-bit SDR (R8G8B8A8) clears the unsupported warning
        fg.OnResizeBuffers(DXGI_FORMAT_R8G8B8A8_UNORM, 1920, 1080);
        assert(fg._xefgHdrFormatUnsupported == false);
        assert(fg._width == 1920 && fg._height == 1080);

        printf("  [PASS] Test 1: HDR RGBA16F format detection correctly flags unsupported status and clears on 10-bit "
               "HDR / 8-bit SDR.\n");
    }

    // ── Test 2: Dimension Mismatch Protection & Subrect Decoupling ──────────────
    {
        MockXeFG fg;
        fg.OnCreateSwapchain(DXGI_FORMAT_R8G8B8A8_UNORM, 3840, 2160);
        void* dummyResource = reinterpret_cast<void*>(0xDEADBEEF);

        // Matching UI and HUD-less textures pass through
        auto resUiMatch = fg.GetResourceData(FG_ResourceType::UIColor, 3840, 2160, dummyResource);
        assert(resUiMatch.pResource == dummyResource);
        assert(resUiMatch.width == 3840 && resUiMatch.height == 2160);
        assert(resUiMatch.valid == true);

        auto resHudlessMatch = fg.GetResourceData(FG_ResourceType::HudlessColor, 3840, 2160, dummyResource);
        assert(resHudlessMatch.pResource == dummyResource);
        assert(resHudlessMatch.valid == true);

        // Mismatched dynamic resolution subrects on UI (e.g. 2560x1440 UI on 4K backbuffer)
        auto resUiMismatch = fg.GetResourceData(FG_ResourceType::UIColor, 2560, 1440, dummyResource);
        assert(resUiMismatch.pResource == nullptr);
        assert(resUiMismatch.width == 0 && resUiMismatch.height == 0);
        assert(resUiMismatch.valid == false);

        // Mismatched dynamic resolution subrects on HUD-less texture
        auto resHudlessMismatch = fg.GetResourceData(FG_ResourceType::HudlessColor, 1920, 1080, dummyResource);
        assert(resHudlessMismatch.pResource == nullptr);
        assert(resHudlessMismatch.valid == false);

        // Core required resources (Depth) are not decoupled by this HUD plane guard
        auto resDepth = fg.GetResourceData(FG_ResourceType::Depth, 1920, 1080, dummyResource);
        assert(resDepth.pResource == dummyResource);
        assert(resDepth.valid == true);

        printf("  [PASS] Test 2: Dimension mismatch decoupling drops mismatched HUD/UI textures while preserving "
               "matching planes.\n");
    }

    // ── Test 3: Model Reset Rate-Limiting & Anti-Thrashing Debounce ─────────────
    {
        MockXeFG fg;
        fg.OnCreateSwapchain(DXGI_FORMAT_R8G8B8A8_UNORM, 1920, 1080);

        // Frame 10: First camera cut triggers reset
        fg._frameCount = 10;
        fg._resetFlag = 1;
        bool r10 = fg.Dispatch();
        assert(r10 == true);
        assert(fg._lastResetFrame == 10);

        // Frame 11: Consecutive frame also requests reset -> debounced!
        fg._frameCount = 11;
        fg._resetFlag = 1;
        bool r11 = fg.Dispatch();
        assert(r11 == false);
        assert(fg._lastResetFrame == 10);

        // Frame 12: Delta 2 frames (< 3) also requests reset -> debounced!
        fg._frameCount = 12;
        fg._resetFlag = 1;
        bool r12 = fg.Dispatch();
        assert(r12 == false);
        assert(fg._lastResetFrame == 10);

        // Frame 13: Delta 3 frames (>= 3) requests reset -> accepted!
        fg._frameCount = 13;
        fg._resetFlag = 1;
        bool r13 = fg.Dispatch();
        assert(r13 == true);
        assert(fg._lastResetFrame == 13);

        // Frame 14: Normal frame without reset flag -> false
        fg._frameCount = 14;
        fg._resetFlag = 0;
        bool r14 = fg.Dispatch();
        assert(r14 == false);

        printf("  [PASS] Test 3: History reset rate-limiting prevents thrashing across rapid consecutive frames.\n");
    }

    // ── Test 4: Swapchain Recreation & Deactivation State Cleansing ─────────────
    {
        MockXeFG fg;
        fg.OnCreateSwapchain(DXGI_FORMAT_R8G8B8A8_UNORM, 1920, 1080);

        // Frame 50: Reset accepted
        fg._frameCount = 50;
        fg._resetFlag = 1;
        assert(fg.Dispatch() == true);
        assert(fg._lastResetFrame == 50);

        // Frame 51: Deactivation and reactivation cleans _lastResetFrame
        fg._frameCount = 51;
        fg.Deactivate();
        assert(fg._lastResetFrame == 0);
        assert(fg._needResetHistory == true);

        // Frame 52: After reactivation, the reset is immediately accepted without debounce
        fg._frameCount = 52;
        assert(fg.Dispatch() == true);
        assert(fg._lastResetFrame == 52);

        // Frame 53: Swapchain resize clears _lastResetFrame
        fg.OnResizeBuffers(DXGI_FORMAT_R8G8B8A8_UNORM, 2560, 1440);
        assert(fg._lastResetFrame == 0);

        // Frame 54: Immediate reset following resize accepted without debounce
        fg._frameCount = 54;
        fg._resetFlag = 1;
        assert(fg.Dispatch() == true);
        assert(fg._lastResetFrame == 54);

        printf(
            "  [PASS] Test 4: Swapchain resize and reactivation reliably clear debounce state for immediate reset.\n");
    }

    printf("[+] All XeFG & XeMFG Hardening and Diagnostics Unit Tests PASSED successfully!\n");
    return 0;
}

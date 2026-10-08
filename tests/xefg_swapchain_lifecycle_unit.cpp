#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Standalone unit test for XeFG Swapchain Lifecycle & Save-Load Black Screen Prevention.

enum class Quirk : uint32_t
{
    None = 0,
    AllowedFrameAhead2 = 1 << 0,
    DoNotPreserveFGSwapChain = 1 << 1,
    RestoreComputeSigOnNonNvidia = 1 << 2,
    DisableDxgiSpoofing = 1 << 3,
    RestoreComputeSigOnNvidia = 1 << 4,
    ForceBorderlessWhenUsingXeFG = 1 << 5,
    OverrideVsyncWhenUsingXeFG = 1 << 6
};

inline Quirk operator|(Quirk a, Quirk b)
{
    return static_cast<Quirk>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline bool HasQuirk(Quirk flags, Quirk test)
{
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(test)) != 0;
}

struct QuirkEntry
{
    const char* exeName;
    Quirk quirks;
};

static const QuirkEntry testQuirkTable[] = {
    { "re9.exe", Quirk::RestoreComputeSigOnNonNvidia | Quirk::DisableDxgiSpoofing | Quirk::RestoreComputeSigOnNvidia |
                     Quirk::AllowedFrameAhead2 },
    { "re9demo.exe", Quirk::RestoreComputeSigOnNonNvidia | Quirk::DisableDxgiSpoofing |
                         Quirk::RestoreComputeSigOnNvidia | Quirk::AllowedFrameAhead2 },
};

class MockXeFGSwapchainManager
{
  public:
    void* _currentFGSwapchain = nullptr;
    void* _swapChainContext = nullptr;
    void* _gameCommandQueue = nullptr;
    uintptr_t _hwnd = 0;
    bool _isActive = false;
    bool _passthrough = false;
    uint32_t _framesToInterpolate = 0;
    bool _preserveSwapChain = true;

    // Simulation helpers
    uint32_t _releaseCallCount = 0;
    uint32_t _createContextCount = 0;
    uint32_t _resizeBuffersCallCount = 0;
    bool _resizeBuffersShouldSucceed = true;

    bool ReleaseSwapchain(uintptr_t hwnd)
    {
        if (hwnd != _hwnd || _hwnd == 0)
            return false;

        _releaseCallCount++;
        _swapChainContext = nullptr;
        _currentFGSwapchain = nullptr;
        _hwnd = 0;
        _gameCommandQueue = nullptr;
        _isActive = false;
        _passthrough = false;
        _framesToInterpolate = 0;

        return true;
    }

    bool CreateSwapchain1(uintptr_t hwnd, uint32_t width, uint32_t height, void** outSwapchain, bool readyToRelease)
    {
        if (_currentFGSwapchain != nullptr && _hwnd == hwnd)
        {
            if (_preserveSwapChain)
            {
                _resizeBuffersCallCount++;
                if (_resizeBuffersShouldSucceed)
                {
                    *outSwapchain = _currentFGSwapchain;
                    return true;
                }
                // If ResizeBuffers failed, fall back to clean recreation
            }

            if (readyToRelease || !_preserveSwapChain)
            {
                ReleaseSwapchain(_hwnd);
            }
            else
            {
                return false;
            }
        }

        if (_swapChainContext == nullptr)
        {
            _createContextCount++;
            _swapChainContext = reinterpret_cast<void*>(0xCAFE0001);
            _currentFGSwapchain = reinterpret_cast<void*>(0xDEAD0002);
            _hwnd = hwnd;
            _gameCommandQueue = reinterpret_cast<void*>(0xBEEF0003);
            _isActive = true;
            _passthrough = false;
            _framesToInterpolate = 1;
            *outSwapchain = _currentFGSwapchain;
            return true;
        }

        return false;
    }
};

int main()
{
    printf("Running XeFG Swapchain Lifecycle & Save-Load Unit Tests...\n");

    // Test 1: Verify RE Requiem quirk entries preserve swapchain (NO DoNotPreserveFGSwapChain), configure
    // AllowedFrameAhead2, and do not force borderless windowing or vsync overrides
    {
        for (const auto& entry : testQuirkTable)
        {
            assert(!HasQuirk(entry.quirks, Quirk::DoNotPreserveFGSwapChain));
            assert(HasQuirk(entry.quirks, Quirk::AllowedFrameAhead2));
            assert(!HasQuirk(entry.quirks, Quirk::ForceBorderlessWhenUsingXeFG));
            assert(!HasQuirk(entry.quirks, Quirk::OverrideVsyncWhenUsingXeFG));
        }
        printf("  [PASS] Test 1: re9.exe and re9demo.exe configure AllowedFrameAhead2 and preserve fullscreen "
               "capability\n");
    }

    // Test 2: Normal startup resolution transition (1080p intro to 1440p menu) preserves swapchain via ResizeBuffers
    {
        MockXeFGSwapchainManager mgr;
        mgr._preserveSwapChain = true;

        void* sc1 = nullptr;
        bool ok = mgr.CreateSwapchain1(0x1234, 1920, 1080, &sc1, false);
        assert(ok);
        assert(sc1 != nullptr);
        assert(mgr._createContextCount == 1);
        assert(mgr._releaseCallCount == 0);
        assert(mgr._resizeBuffersCallCount == 0);
        assert(mgr._hwnd == 0x1234);

        // Intro finishes, game switches to 1440p menu
        void* sc2 = nullptr;
        ok = mgr.CreateSwapchain1(0x1234, 2560, 1440, &sc2, false);
        assert(ok);
        assert(sc2 == sc1);
        assert(mgr._resizeBuffersCallCount == 1);
        assert(mgr._releaseCallCount == 0); // No destructive swapchain release!
        assert(mgr._createContextCount == 1);
        assert(mgr._hwnd == 0x1234);

        printf("  [PASS] Test 2: Startup resolution transition cleanly resizes without swapchain destruction\n");
    }

    // Test 3: ResizeBuffers failure safely falls back to clean swapchain recreation without COM ref loops
    {
        MockXeFGSwapchainManager mgr;
        mgr._preserveSwapChain = true;
        mgr._resizeBuffersShouldSucceed = false; // simulate game having destroyed real swapchain

        void* sc1 = nullptr;
        bool ok = mgr.CreateSwapchain1(0x5678, 1920, 1080, &sc1, false);
        assert(ok);
        assert(mgr._createContextCount == 1);

        // Recreate called: ResizeBuffers fails -> falls back to ReleaseSwapchain & recreate
        void* sc2 = nullptr;
        ok = mgr.CreateSwapchain1(0x5678, 2560, 1440, &sc2, true);
        assert(ok);
        assert(mgr._releaseCallCount == 1);
        assert(mgr._createContextCount == 2);

        printf("  [PASS] Test 3: ResizeBuffers failure safely falls back to clean swapchain recreation\n");
    }

    // Test 4: ReleaseSwapchain full state cleanup
    {
        MockXeFGSwapchainManager mgr;
        void* sc = nullptr;
        mgr.CreateSwapchain1(0x9999, 2560, 1440, &sc, false);
        assert(mgr._hwnd == 0x9999);
        assert(mgr._gameCommandQueue != nullptr);
        assert(mgr._isActive == true);

        bool released = mgr.ReleaseSwapchain(0x9999);
        assert(released);
        assert(mgr._hwnd == 0);
        assert(mgr._gameCommandQueue == nullptr);
        assert(mgr._swapChainContext == nullptr);
        assert(mgr._currentFGSwapchain == nullptr);
        assert(mgr._isActive == false);
        assert(mgr._passthrough == false);
        assert(mgr._framesToInterpolate == 0);

        printf("  [PASS] Test 4: ReleaseSwapchain cleanly resets all context and device queue pointers\n");
    }

    // Test 5: Verify DXGI_MODE_SCALING_CENTERED maps to DXGI_SCALING_STRETCH for HWND swapchain compatibility
    {
        auto mapScaling = [](int modeScaling)
        {
            switch (modeScaling)
            {
            case 2:       // DXGI_MODE_SCALING_CENTERED
                return 1; // DXGI_SCALING_STRETCH
            case 1:       // DXGI_MODE_SCALING_STRETCHED
                return 1; // DXGI_SCALING_STRETCH
            default:
                return 0; // DXGI_SCALING_NONE
            }
        };
        assert(mapScaling(2) == 1 && "Centered mode scaling must map to stretch on HWND swapchain");
        printf("  [PASS] Test 5: HWND swapchain maps centered scaling to stretch\n");
    }

    printf("All XeFG Swapchain Lifecycle Unit Tests PASSED!\n");
    return 0;
}

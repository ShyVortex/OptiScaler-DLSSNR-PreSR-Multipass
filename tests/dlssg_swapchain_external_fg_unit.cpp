#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// Buffer types and resource state constants (mimicking Streamline & DX12)
namespace sl
{
    using Feature = uint32_t;
    constexpr Feature kFeatureDLSS_G = 1000;
    constexpr uint32_t kBufferTypeHUDLessColor = 1;
    constexpr uint32_t kBufferTypeDepth = 2;

    struct Resource
    {
        void* native = nullptr;
        uint32_t state = 0;
    };

    struct ResourceTag
    {
        uint32_t type = 0;
        Resource* resource = nullptr;
    };
}

constexpr uint32_t D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE = 0x40;
constexpr uint32_t D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE = 0x80;
constexpr uint32_t D3D12_RESOURCE_STATE_UNORDERED_ACCESS = 0x8;
constexpr uint32_t DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING = 0x800;
constexpr uint32_t DXGI_PRESENT_ALLOW_TEARING = 0x00000200;

enum class FGOutput
{
    NoFG,
    FSRFG,
    XeFG,
    DLSSG
};

enum class FGNvngxReplacement
{
    None,
    OptiScaler,
    Nukem,
    Custom
};

// ---------------------------------------------------------------------------
// 1. Simulation of Streamline hkslSetTag CyberpunkHudlessState quirk
// ---------------------------------------------------------------------------
bool SimulateHkslSetTagCyberpunkQuirk(
    bool externalFrameGeneration,
    FGOutput activeFgOutput,
    bool hasCyberpunkHudlessStateQuirk,
    sl::ResourceTag& tag)
{
    const uint32_t originalState = tag.resource->state;

    // Fixed logic from Streamline_Hooks.cpp
    if (!externalFrameGeneration &&
        activeFgOutput == FGOutput::FSRFG &&
        hasCyberpunkHudlessStateQuirk &&
        tag.resource->state ==
            (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) &&
        tag.type == sl::kBufferTypeHUDLessColor)
    {
        tag.resource->state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    return tag.resource->state != originalState;
}

// ---------------------------------------------------------------------------
// 2. Simulation of wrapped_swapchain _localMutex recursion check
// ---------------------------------------------------------------------------
bool ShouldAcquireLocalMutex(
    uint32_t currentOwner,
    bool externalFrameGeneration,
    FGNvngxReplacement activeFgNvngx,
    FGOutput activeFgOutput)
{
    // Fixed logic from wrapped_swapchain.cpp
    const bool presentOwnsLock = (currentOwner == 4 || currentOwner == 5);
    const bool isDlssgMod = externalFrameGeneration ||
                            activeFgNvngx != FGNvngxReplacement::None ||
                            activeFgOutput == FGOutput::DLSSG;
    if (!(presentOwnsLock && isDlssgMod))
    {
        return true; // Safe to acquire
    }
    return false; // Skip locking to prevent recursive shared_mutex deadlock
}

// ---------------------------------------------------------------------------
// 3. Simulation of LocalPresent V-Sync / ALLOW_TEARING fallback
// ---------------------------------------------------------------------------
struct PresentFlagsResult
{
    uint32_t syncInterval = 1;
    uint32_t flags = 0;
};

PresentFlagsResult SimulateLocalPresentVsyncFallback(
    bool externalFrameGeneration,
    bool forceVsyncConfigured,
    bool forceVsyncValue,
    bool scAllowTearing,
    bool isExclusiveFullscreen,
    uint32_t initialSyncInterval,
    uint32_t initialFlags)
{
    PresentFlagsResult res;
    res.syncInterval = initialSyncInterval;
    res.flags = initialFlags;

    // Fixed logic from wrapped_swapchain.cpp LocalPresent
    if (forceVsyncConfigured && !externalFrameGeneration)
    {
        if (!forceVsyncValue)
        {
            res.syncInterval = 0;
            if (scAllowTearing && !isExclusiveFullscreen)
            {
                res.flags |= DXGI_PRESENT_ALLOW_TEARING;
            }
        }
        else
        {
            res.syncInterval = 1;
            res.flags &= ~DXGI_PRESENT_ALLOW_TEARING;
        }
    }

    return res;
}

// ---------------------------------------------------------------------------
// 4. Simulation of ResizeBuffers SwapChainFlags override
// ---------------------------------------------------------------------------
uint32_t SimulateResizeBuffersFlags(
    bool externalFrameGeneration,
    bool overrideVsyncConfig,
    bool isExclusiveFullscreen,
    void* currentFG,
    uint32_t swapChainFlags)
{
    if (overrideVsyncConfig && !isExclusiveFullscreen && currentFG == nullptr && !externalFrameGeneration)
    {
        swapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    return swapChainFlags;
}

// ---------------------------------------------------------------------------
// Main Unit Test Driver
// ---------------------------------------------------------------------------
int main()
{
    printf("[TEST] Running DLSS-G Swapchain & External FG safety unit tests...\n");

    // Test 1: Cyberpunk HUDless tag state mutation is strictly disabled on External FG (DLSS-G / SM86 mod)
    {
        sl::Resource res;
        res.native = reinterpret_cast<void*>(0x1234);
        res.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        sl::ResourceTag tag;
        tag.type = sl::kBufferTypeHUDLessColor;
        tag.resource = &res;

        bool mutated = SimulateHkslSetTagCyberpunkQuirk(
            /*externalFrameGeneration=*/true,
            /*activeFgOutput=*/FGOutput::NoFG,
            /*hasCyberpunkHudlessStateQuirk=*/true,
            tag);

        assert(!mutated);
        assert(tag.resource->state == (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
        printf("  [PASS] Case 1: Cyberpunk hudless quirk does NOT mutate tags under External FG\n");
    }

    // Test 2: Cyberpunk HUDless tag state mutation IS applied for internal FSR-FG on non-external FG
    {
        sl::Resource res;
        res.native = reinterpret_cast<void*>(0x1234);
        res.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        sl::ResourceTag tag;
        tag.type = sl::kBufferTypeHUDLessColor;
        tag.resource = &res;

        bool mutated = SimulateHkslSetTagCyberpunkQuirk(
            /*externalFrameGeneration=*/false,
            /*activeFgOutput=*/FGOutput::FSRFG,
            /*hasCyberpunkHudlessStateQuirk=*/true,
            tag);

        assert(mutated);
        assert(tag.resource->state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        printf("  [PASS] Case 2: Cyberpunk hudless quirk operates correctly for internal FSR-FG\n");
    }

    // Test 3: _localMutex recursion check correctly detects Present1 (owner 5) under External FG
    {
        // Inside Present1 (owner 5), External FG calls GetBuffer / ResizeBuffers1:
        bool acquire = ShouldAcquireLocalMutex(
            /*currentOwner=*/5, // Present1
            /*externalFrameGeneration=*/true,
            /*activeFgNvngx=*/FGNvngxReplacement::None,
            /*activeFgOutput=*/FGOutput::NoFG);

        assert(!acquire); // Must skip to prevent recursive lock deadlock
        printf("  [PASS] Case 3: Present1 (owner 5) skips recursive lock during External FG\n");
    }

    // Test 4: _localMutex recursion check correctly detects Present (owner 4) under External FG
    {
        bool acquire = ShouldAcquireLocalMutex(
            /*currentOwner=*/4, // Present
            /*externalFrameGeneration=*/true,
            /*activeFgNvngx=*/FGNvngxReplacement::None,
            /*activeFgOutput=*/FGOutput::NoFG);

        assert(!acquire); // Must skip to prevent recursive lock deadlock
        printf("  [PASS] Case 4: Present (owner 4) skips recursive lock during External FG\n");
    }

    // Test 5: _localMutex acquires normally when lock is free (owner 0)
    {
        bool acquire = ShouldAcquireLocalMutex(
            /*currentOwner=*/0, // Free
            /*externalFrameGeneration=*/true,
            /*activeFgNvngx=*/FGNvngxReplacement::None,
            /*activeFgOutput=*/FGOutput::NoFG);

        assert(acquire); // Safe to acquire
        printf("  [PASS] Case 5: Normal un-held mutex acquires without issue\n");
    }

    // Test 6: LocalPresent V-Sync / tearing overrides are bypassed when externalFrameGeneration is active
    {
        // Even if ForceVsync is configured to false and SCAllowTearing is true (post-Alt-Tab):
        PresentFlagsResult res = SimulateLocalPresentVsyncFallback(
            /*externalFrameGeneration=*/true,
            /*forceVsyncConfigured=*/true,
            /*forceVsyncValue=*/false,
            /*scAllowTearing=*/true,
            /*isExclusiveFullscreen=*/false,
            /*initialSyncInterval=*/1,
            /*initialFlags=*/0);

        // Flags must NOT have DXGI_PRESENT_ALLOW_TEARING injected; sync interval preserved
        assert((res.flags & DXGI_PRESENT_ALLOW_TEARING) == 0);
        assert(res.syncInterval == 1);
        printf("  [PASS] Case 6: External FG bypasses V-Sync override and ALLOW_TEARING injection in Present\n");
    }

    // Test 7: Non-external FG obeys ForceVsync=false and injects ALLOW_TEARING as intended
    {
        PresentFlagsResult res = SimulateLocalPresentVsyncFallback(
            /*externalFrameGeneration=*/false,
            /*forceVsyncConfigured=*/true,
            /*forceVsyncValue=*/false,
            /*scAllowTearing=*/true,
            /*isExclusiveFullscreen=*/false,
            /*initialSyncInterval=*/1,
            /*initialFlags=*/0);

        assert((res.flags & DXGI_PRESENT_ALLOW_TEARING) != 0);
        assert(res.syncInterval == 0);
        printf("  [PASS] Case 7: Non-external FG respects ForceVsync=false tearing fallback\n");
    }

    // Test 8: ResizeBuffers does not force ALLOW_TEARING onto external FG swapchain
    {
        uint32_t flags = SimulateResizeBuffersFlags(
            /*externalFrameGeneration=*/true,
            /*overrideVsyncConfig=*/true,
            /*isExclusiveFullscreen=*/false,
            /*currentFG=*/nullptr,
            /*swapChainFlags=*/0);

        assert((flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) == 0);
        printf("  [PASS] Case 8: ResizeBuffers does not inject ALLOW_TEARING into External FG swapchain\n");
    }

    printf("[TEST] All DLSS-G Swapchain & External FG safety unit tests passed successfully!\n");
    return 0;
}

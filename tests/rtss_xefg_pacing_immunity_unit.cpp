#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Standalone unit test for RTSS Reflex Injection filtering, frame counter protection,
// and XeFG presentation pacing immunity.

static const int BUFFER_COUNT = 4;

enum NV_LATENCY_MARKER_TYPE
{
    SIMULATION_START = 0,
    SIMULATION_END = 1,
    RENDERSUBMIT_START = 2,
    RENDERSUBMIT_END = 3,
    PRESENT_START = 4,
    PRESENT_END = 5,
};

struct NV_LATENCY_MARKER_PARAMS
{
    uint32_t version;
    NV_LATENCY_MARKER_TYPE markerType;
    uint64_t frameID;
    uint32_t flags;
};

class MockFGFeature
{
  public:
    uint64_t _frameCount = 0;
    uint64_t _lastDispatchedFrame = 0;
    uint32_t _dispatchedCount = 0;

    void SetFrameCount(uint64_t frameId)
    {
        if (frameId > _frameCount)
        {
            _frameCount = frameId;
        }
        else if (_frameCount - frameId > 4)
        {
            _frameCount = frameId;
            _lastDispatchedFrame = (frameId > 0 ? frameId - 1 : 0);
        }
    }

    uint64_t FrameCount() const { return _frameCount; }

    int GetDispatchIndex(uint64_t& willDispatchFrame)
    {
        if (_frameCount == _lastDispatchedFrame)
            return -1;

        willDispatchFrame = _lastDispatchedFrame + 1;
        _lastDispatchedFrame = willDispatchFrame;
        _dispatchedCount++;
        return static_cast<int>(willDispatchFrame % BUFFER_COUNT);
    }
};

class MockReflexHook
{
  public:
    uint64_t _lastAsyncMarkerFrameId = 0;
    uint8_t _FgNumFramesToGenerate = 1;
    uint64_t _reflexFrameId = 0;
    bool _rtssReflexInjection = false;
    MockFGFeature* _fg = nullptr;

    void hkNvAPI_D3D_SetLatencyMarker(NV_LATENCY_MARKER_PARAMS* pSetLatencyMarkerParams)
    {
        const bool isRtssMarker = (pSetLatencyMarkerParams->frameID >> 32) != 0;

        // Ensure RTSS synthetic markers do not trigger premature FG reset
        if (!isRtssMarker && _lastAsyncMarkerFrameId + 10 < pSetLatencyMarkerParams->frameID)
        {
            _FgNumFramesToGenerate = 0;
        }

        if (isRtssMarker)
        {
            _rtssReflexInjection = true;
        }

        // Sanitize reflexFrameId to 32 bits if RTSS high bits are set
        _reflexFrameId =
            isRtssMarker ? (pSetLatencyMarkerParams->frameID & 0xFFFFFFFF) : pSetLatencyMarkerParams->frameID;

        if (pSetLatencyMarkerParams->markerType == PRESENT_START && _fg != nullptr)
        {
            if (!isRtssMarker)
            {
                auto frameCount = _fg->FrameCount();
                if (pSetLatencyMarkerParams->frameID != frameCount)
                    _fg->SetFrameCount(pSetLatencyMarkerParams->frameID);

                _reflexFrameId = pSetLatencyMarkerParams->frameID;
            }
            // else: RTSS synthetic markers are ignored for FG frame counting
        }
    }
};

int main()
{
    printf("Running RTSS + XeFG Pacing & Marker Immunity Unit Tests...\n");

    // Test 1: RTSS Synthetic Marker Detection and Filtering
    {
        MockFGFeature fg;
        fg._frameCount = 100;
        fg._lastDispatchedFrame = 99;

        MockReflexHook reflex;
        reflex._fg = &fg;
        reflex._FgNumFramesToGenerate = 1;
        reflex._lastAsyncMarkerFrameId = 95;

        // RTSS injects marker with upper bits set (e.g. 0x100000042)
        NV_LATENCY_MARKER_PARAMS rtssMarker = {};
        rtssMarker.markerType = PRESENT_START;
        rtssMarker.frameID = 0x0000000100000042ULL; // 4,294,967,362

        reflex.hkNvAPI_D3D_SetLatencyMarker(&rtssMarker);

        // Verify RTSS was detected
        assert(reflex._rtssReflexInjection == true);
        // Verify reflexFrameId was sanitized to lower 32 bits (0x42 = 66)
        assert(reflex._reflexFrameId == 0x42ULL);
        // CRITICAL: FG frameCount must NOT have been corrupted to 4+ billion!
        assert(fg._frameCount == 100);
        // CRITICAL: _FgNumFramesToGenerate must NOT have been reset to 0!
        assert(reflex._FgNumFramesToGenerate == 1);

        printf("  [PASS] Test 1: RTSS synthetic marker filtered from SetFrameCount and ID sanitized\n");
    }

    // Test 2: Legitimate Game Markers Synchronize Correctly
    {
        MockFGFeature fg;
        fg._frameCount = 100;

        MockReflexHook reflex;
        reflex._fg = &fg;

        // Legitimate game marker (32-bit frameID)
        NV_LATENCY_MARKER_PARAMS gameMarker = {};
        gameMarker.markerType = PRESENT_START;
        gameMarker.frameID = 105;

        reflex.hkNvAPI_D3D_SetLatencyMarker(&gameMarker);

        assert(reflex._reflexFrameId == 105);
        assert(fg._frameCount == 105);

        printf("  [PASS] Test 2: Legitimate game-native marker updates frame counter normally\n");
    }

    // Test 3: Interleaved RTSS Injection and Game Rendering Pacing Stability
    {
        MockFGFeature fg;
        fg._frameCount = 0;
        fg._lastDispatchedFrame = 0;

        MockReflexHook reflex;
        reflex._fg = &fg;

        // Simulate 50 frames of gameplay where RTSS constantly injects markers
        for (uint64_t f = 1; f <= 50; ++f)
        {
            // Game advances frame
            fg._frameCount = f;

            // RTSS injects synthetic marker before game present
            NV_LATENCY_MARKER_PARAMS rtssMarker = {};
            rtssMarker.markerType = PRESENT_START;
            rtssMarker.frameID = 0x0000000200000000ULL | f;
            reflex.hkNvAPI_D3D_SetLatencyMarker(&rtssMarker);

            // Game presents native marker
            NV_LATENCY_MARKER_PARAMS gameMarker = {};
            gameMarker.markerType = PRESENT_START;
            gameMarker.frameID = f;
            reflex.hkNvAPI_D3D_SetLatencyMarker(&gameMarker);

            // XeFG dispatches frame
            uint64_t willDispatch = 0;
            int idx = fg.GetDispatchIndex(willDispatch);

            assert(idx >= 0);
            assert(willDispatch == f);
            assert(fg._lastDispatchedFrame == f);
        }

        assert(fg._dispatchedCount == 50);
        printf("  [PASS] Test 3: Interleaved RTSS injection maintains flawless 1:1 frame dispatch pacing\n");
    }

    printf("All RTSS + XeFG Pacing & Marker Immunity Unit Tests PASSED!\n");
    return 0;
}

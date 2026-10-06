#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Standalone unit test for XeFG Ring Buffer Pacing, StartNewFrame Discontinuity Logic,
// and Motion Vector Scale Safety.

static const int BUFFER_COUNT = 4;

class MockFGFeature
{
  public:
    uint64_t _frameCount = 0;
    uint64_t _lastDispatchedFrame = 0;
    uint32_t _jumpCount = 0;
    int _allowedFrameAhead = 1;
    bool _resourcesReady[BUFFER_COUNT] = { false, false, false, false };

    int GetIndex() const { return static_cast<int>(_frameCount % BUFFER_COUNT); }

    uint64_t StartNewFrame()
    {
        _frameCount++;

        if (_lastDispatchedFrame == 0 || (_frameCount - _lastDispatchedFrame) > 2)
        {
            _jumpCount++;
            _lastDispatchedFrame = _frameCount - 1;
        }

        auto idx = GetIndex();
        _resourcesReady[idx] = false; // Ring buffer slot cleared for incoming frame

        return _frameCount;
    }

    int GetDispatchIndex(uint64_t& willDispatchFrame)
    {
        if (_frameCount == _lastDispatchedFrame)
            return -1;

        willDispatchFrame = _lastDispatchedFrame + 1;

        auto diff = _frameCount - _lastDispatchedFrame;
        if (diff > _allowedFrameAhead || diff < 0 || _lastDispatchedFrame == 0)
        {
            auto idx = GetIndex();
            if (_resourcesReady[idx])
            {
                willDispatchFrame = _frameCount;
            }
        }

        _lastDispatchedFrame = willDispatchFrame;
        return static_cast<int>(willDispatchFrame % BUFFER_COUNT);
    }
};

struct MockFrameConstantData
{
    float motionVectorScaleX = 0.0f;
    float motionVectorScaleY = 0.0f;
};

static void PopulateFrameConstants(MockFrameConstantData& constData, float inputMvScaleX, float inputMvScaleY)
{
    constData.motionVectorScaleX = (inputMvScaleX != 0.0f) ? inputMvScaleX : 1.0f;
    constData.motionVectorScaleY = (inputMvScaleY != 0.0f) ? inputMvScaleY : 1.0f;
}

int main()
{
    printf("Running XeFG Ring Buffer & Motion Vector Pacing Unit Tests...\n");

    // Test 1: Pipelining up to 2 frames ahead operates smoothly without premature jumps
    {
        MockFGFeature fg;
        fg._allowedFrameAhead = 2; // Capcom RE Engine / TLOU quirk

        // Frame 1
        fg.StartNewFrame(); // frame 1, lastDispatched = 0 -> sets to 0
        assert(fg._frameCount == 1);
        assert(fg._lastDispatchedFrame == 0);
        assert(fg._jumpCount == 1);

        uint64_t df = 0;
        fg._resourcesReady[fg.GetIndex()] = true;
        int idx = fg.GetDispatchIndex(df);
        assert(df == 1);
        assert(idx == 1);
        assert(fg._lastDispatchedFrame == 1);

        // Frame 2
        fg.StartNewFrame();
        assert(fg._jumpCount == 1); // diff = 2 - 1 = 1 <= 2, no extra jump
        fg._resourcesReady[fg.GetIndex()] = true;
        fg.GetDispatchIndex(df);
        assert(df == 2);
        assert(fg._lastDispatchedFrame == 2);

        // Frame 3 (pipeline 1 frame ahead)
        fg.StartNewFrame(); // diff = 3 - 2 = 1 <= 2
        assert(fg._jumpCount == 1);

        // Frame 4 (pipeline 2 frames ahead, within AllowedFrameAhead2)
        fg.StartNewFrame(); // diff = 4 - 2 = 2 <= 2
        assert(fg._jumpCount == 1);
        assert(fg._lastDispatchedFrame == 2);

        printf("  [PASS] Test 1: Pipelining up to 2 frames ahead operates smoothly\n");
    }

    // Test 2: Gaps > 2 immediately resynchronize to avoid stale ring buffer starvation (TLOU Part II fix)
    {
        MockFGFeature fg;
        fg._allowedFrameAhead = 2;

        fg.StartNewFrame(); // frame 1
        fg._resourcesReady[fg.GetIndex()] = true;
        uint64_t df = 0;
        fg.GetDispatchIndex(df); // dispatched 1, _lastDispatchedFrame = 1

        // Cutscene or skipped frames: frames 2 and 3 render without dispatch
        fg.StartNewFrame(); // frame 2, diff = 2 - 1 = 1 <= 2
        fg.StartNewFrame(); // frame 3, diff = 3 - 1 = 2 <= 2

        // Frame 4 begins: diff = 4 - 1 = 3 > 2 -> MUST resynchronize to frame 3!
        fg.StartNewFrame();
        assert(fg._frameCount == 4);
        assert(fg._lastDispatchedFrame == 3); // Snapped to _frameCount - 1

        // Next dispatch now requests frame 4 (current valid frame) rather than stale frame 2!
        fg._resourcesReady[fg.GetIndex()] = true;
        fg.GetDispatchIndex(df);
        assert(df == 4);
        assert(fg._lastDispatchedFrame == 4);

        printf("  [PASS] Test 2: Gaps > 2 immediately resynchronize to current valid frame\n");
    }

    // Test 3: Large discontinuity (scene change or reload) clamps and reports correctly
    {
        MockFGFeature fg;
        fg.StartNewFrame(); // frame 1
        fg._resourcesReady[fg.GetIndex()] = true;
        uint64_t df = 0;
        fg.GetDispatchIndex(df); // dispatched 1, _lastDispatchedFrame = 1

        // Level reload gap
        fg._frameCount = 100;
        fg.StartNewFrame(); // frame 101, diff = 101 - 1 = 100 > 2
        assert(fg._lastDispatchedFrame == 100);

        printf("  [PASS] Test 3: Large discontinuity correctly clamps _lastDispatchedFrame\n");
    }

    // Test 4: Motion vector scale zero fallback
    {
        MockFrameConstantData constData {};

        // Case A: 0.0f inputs must fallback to 1.0f
        PopulateFrameConstants(constData, 0.0f, 0.0f);
        assert(constData.motionVectorScaleX == 1.0f);
        assert(constData.motionVectorScaleY == 1.0f);

        // Case B: Non-zero inputs preserved
        PopulateFrameConstants(constData, 2.5f, -1.0f);
        assert(constData.motionVectorScaleX == 2.5f);
        assert(constData.motionVectorScaleY == -1.0f);

        // Case C: Single-axis zero input fallback
        PopulateFrameConstants(constData, 1.5f, 0.0f);
        assert(constData.motionVectorScaleX == 1.5f);
        assert(constData.motionVectorScaleY == 1.0f);

        printf("  [PASS] Test 4: Motion vector scale zero inputs safely default to 1.0f\n");
    }

    printf("All XeFG Ring Buffer & Motion Vector Pacing Unit Tests PASSED!\n");
    return 0;
}

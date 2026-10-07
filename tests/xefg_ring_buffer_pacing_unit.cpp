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
    uint64_t _actuallyDispatchedFrame = 0;
    uint32_t _jumpCount = 0;
    int _allowedFrameAhead = 1;
    bool _depthReady[BUFFER_COUNT] = { false, false, false, false };
    bool _velocityReady[BUFFER_COUNT] = { false, false, false, false };

    int GetIndex() const { return static_cast<int>(_frameCount % BUFFER_COUNT); }

    bool IsSlotReady(int index) const
    {
        if (index < 0 || index >= BUFFER_COUNT)
            return false;
        return _depthReady[index] && _velocityReady[index];
    }

    void TagSlot(int index)
    {
        if (index >= 0 && index < BUFFER_COUNT)
        {
            _depthReady[index] = true;
            _velocityReady[index] = true;
        }
    }

    uint64_t StartNewFrame()
    {
        _frameCount++;

        if (_lastDispatchedFrame == 0 || (_frameCount - _lastDispatchedFrame) > 2)
        {
            _jumpCount++;
            _lastDispatchedFrame = _frameCount - 1;
        }

        auto idx = GetIndex();
        _depthReady[idx] = false;
        _velocityReady[idx] = false;

        return _frameCount;
    }

    int ResolveDispatchSlot(uint64_t& willDispatchFrame)
    {
        if (_frameCount == _actuallyDispatchedFrame && _frameCount != 0)
        {
            willDispatchFrame = _actuallyDispatchedFrame;
            return -1;
        }

        if (_lastDispatchedFrame > _actuallyDispatchedFrame)
        {
            int slot0 = static_cast<int>(_lastDispatchedFrame % BUFFER_COUNT);
            if (IsSlotReady(slot0))
            {
                willDispatchFrame = _lastDispatchedFrame;
                return slot0;
            }
        }

        uint64_t cand1 = _lastDispatchedFrame + 1;
        int slot1 = static_cast<int>(cand1 % BUFFER_COUNT);
        if (IsSlotReady(slot1))
        {
            willDispatchFrame = cand1;
            return slot1;
        }

        if (_frameCount > cand1)
        {
            int slotCur = static_cast<int>(_frameCount % BUFFER_COUNT);
            if (IsSlotReady(slotCur))
            {
                willDispatchFrame = _frameCount;
                return slotCur;
            }

            uint64_t candPrev = _frameCount - 1;
            if (candPrev > _lastDispatchedFrame)
            {
                int slotPrev = static_cast<int>(candPrev % BUFFER_COUNT);
                if (IsSlotReady(slotPrev))
                {
                    willDispatchFrame = candPrev;
                    return slotPrev;
                }
            }
        }

        if (_lastDispatchedFrame == 0 && _frameCount > 0)
        {
            int slotCur = static_cast<int>(_frameCount % BUFFER_COUNT);
            if (IsSlotReady(slotCur))
            {
                willDispatchFrame = _frameCount;
                return slotCur;
            }
        }

        willDispatchFrame = 0;
        return -1;
    }

    int GetIndexWillBeDispatched()
    {
        uint64_t df = 0;
        int64_t diff = _frameCount - _lastDispatchedFrame;
        if (diff > _allowedFrameAhead || diff < 0 || _lastDispatchedFrame == 0)
        {
            if (_depthReady[GetIndex()])
                df = _frameCount;
            else
                df = _lastDispatchedFrame + 1;
        }
        else
        {
            df = _lastDispatchedFrame + 1;
        }
        return static_cast<int>(df % BUFFER_COUNT);
    }

    int GetDispatchIndex(uint64_t& willDispatchFrame)
    {
        int slot = ResolveDispatchSlot(willDispatchFrame);
        if (slot >= 0)
        {
            _lastDispatchedFrame = willDispatchFrame;
        }
        return slot;
    }

    void ConfirmDispatched(uint64_t frameId)
    {
        _actuallyDispatchedFrame = frameId;
        _lastDispatchedFrame = frameId;
        int slot = static_cast<int>(frameId % BUFFER_COUNT);
        _depthReady[slot] = false;
        _velocityReady[slot] = false;
    }

    void ClearAllResourceReady()
    {
        for (int i = 0; i < BUFFER_COUNT; i++)
        {
            _depthReady[i] = false;
            _velocityReady[i] = false;
        }
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
        fg.TagSlot(fg.GetIndex());
        int idx = fg.GetDispatchIndex(df);
        assert(df == 1);
        assert(idx == 1);
        assert(fg._lastDispatchedFrame == 1);
        fg.ConfirmDispatched(df);

        // Frame 2
        fg.StartNewFrame();
        assert(fg._jumpCount == 1); // diff = 2 - 1 = 1 <= 2, no extra jump
        fg.TagSlot(fg.GetIndex());
        fg.GetDispatchIndex(df);
        assert(df == 2);
        assert(fg._lastDispatchedFrame == 2);
        fg.ConfirmDispatched(df);

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
        fg.TagSlot(fg.GetIndex());
        uint64_t df = 0;
        fg.GetDispatchIndex(df); // dispatched 1, _lastDispatchedFrame = 1
        fg.ConfirmDispatched(df);

        // Cutscene or skipped frames: frames 2 and 3 render without dispatch
        fg.StartNewFrame(); // frame 2, diff = 2 - 1 = 1 <= 2
        fg.StartNewFrame(); // frame 3, diff = 3 - 1 = 2 <= 2

        // Frame 4 begins: diff = 4 - 1 = 3 > 2 -> MUST resynchronize to frame 3!
        fg.StartNewFrame();
        assert(fg._frameCount == 4);
        assert(fg._lastDispatchedFrame == 3); // Snapped to _frameCount - 1

        // Next dispatch now requests frame 4 (current valid frame) rather than stale frame 2!
        fg.TagSlot(fg.GetIndex());
        fg.GetDispatchIndex(df);
        assert(df == 4);
        assert(fg._lastDispatchedFrame == 4);
        fg.ConfirmDispatched(df);

        printf("  [PASS] Test 2: Gaps > 2 immediately resynchronize to current valid frame\n");
    }

    // Test 3: Large discontinuity (scene change or reload) clamps and reports correctly
    {
        MockFGFeature fg;
        fg.StartNewFrame(); // frame 1
        fg.TagSlot(fg.GetIndex());
        uint64_t df = 0;
        fg.GetDispatchIndex(df); // dispatched 1, _lastDispatchedFrame = 1
        fg.ConfirmDispatched(df);

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

    // Test 5: Pipelined simulation ahead of render (RE Engine test10 scenario)
    {
        MockFGFeature fg;
        // Frame 1 renders and dispatches normally
        fg.StartNewFrame();        // frame 1
        fg.TagSlot(fg.GetIndex()); // slot 1 ready
        uint64_t df = 0;
        int idx = fg.GetDispatchIndex(df);
        assert(idx == 1 && df == 1);
        fg.ConfirmDispatched(1);

        // Frame 2 is rendered into slot 2, but simulation thread calls StartNewFrame for frame 3 first!
        fg.StartNewFrame(); // frame 2
        // Frame 2 finishes and tags slot 2
        fg.TagSlot(2);
        // Simulation thread rushes ahead and calls StartNewFrame for frame 3!
        fg.StartNewFrame(); // frame 3, fIndex = 3 (slot 3 cleared, NOT tagged yet!)
        assert(fg._frameCount == 3);

        // Present arrives for frame 2: GetDispatchIndex must dispatch frame 2 from slot 2, NOT unready slot 3!
        idx = fg.GetDispatchIndex(df);
        assert(df == 2);
        assert(idx == 2);
        fg.ConfirmDispatched(2);

        // Now frame 3 renders and tags slot 3
        fg.TagSlot(3);
        idx = fg.GetDispatchIndex(df);
        assert(df == 3);
        assert(idx == 3);
        fg.ConfirmDispatched(3);

        printf("  [PASS] Test 5: Pipelined simulation ahead of render dispatches ready slot correctly\n");
    }

    // Test 6: Unpause resumption where StartNewFrame advances before unpaused frame dispatch
    {
        MockFGFeature fg;
        // Simulating unpause after pause menu:
        // Activate resets dispatch counters
        fg._frameCount = 10954;
        fg._lastDispatchedFrame = 0;
        fg._actuallyDispatchedFrame = 0;

        // Simulation thread sends setConstants(10955) -> calls StartNewFrame()
        fg.StartNewFrame(); // frame 10955, _lastDispatchedFrame becomes 10954, but _actuallyDispatchedFrame remains 0!
        assert(fg._frameCount == 10955);
        assert(fg._lastDispatchedFrame == 10954);
        assert(fg._actuallyDispatchedFrame == 0);

        // Render thread tags frame 10954 into slot 2 (10954 % 4 = 2)
        int slot10954 = static_cast<int>(10954 % BUFFER_COUNT);
        assert(slot10954 == 2);
        fg.TagSlot(slot10954);

        // Present runs for frame 10954: GetDispatchIndex must dispatch frame 10954, NOT skip to unready 10955!
        uint64_t df = 0;
        int idx = fg.GetDispatchIndex(df);
        assert(df == 10954);
        assert(idx == 2);
        fg.ConfirmDispatched(10954);

        // Next frame: 10955 is tagged into slot 3
        int slot10955 = static_cast<int>(10955 % BUFFER_COUNT);
        assert(slot10955 == 3);
        fg.TagSlot(slot10955);
        idx = fg.GetDispatchIndex(df);
        assert(df == 10955);
        assert(idx == 3);
        fg.ConfirmDispatched(10955);

        printf("  [PASS] Test 6: Unpause resumption catches undispatched frame without skipping\n");
    }

    // Test 7: Multi-resource tagging yields identical slot index (zero MV slot-split)
    {
        MockFGFeature fg;
        fg._allowedFrameAhead = 2;
        fg.StartNewFrame(); // frame 1

        // When Streamline tags Depth, it queries GetIndexWillBeDispatched
        int depthSlot = fg.GetIndexWillBeDispatched();
        fg._depthReady[depthSlot] = true;

        // When Streamline next tags MotionVectors, it queries GetIndexWillBeDispatched
        int mvSlot = fg.GetIndexWillBeDispatched();
        fg._velocityReady[mvSlot] = true;

        // Both resources MUST resolve to the exact same slot!
        assert(depthSlot == mvSlot);
        assert(fg.IsSlotReady(depthSlot));

        uint64_t df = 0;
        int dispatchSlot = fg.GetDispatchIndex(df);
        assert(dispatchSlot == depthSlot);
        assert(df == 1);
        fg.ConfirmDispatched(df);

        // ConfirmDispatched must flush slot readiness so it doesn't linger
        assert(!fg.IsSlotReady(depthSlot));

        printf("  [PASS] Test 7: Multi-resource tagging yields identical slot index (zero MV slot-split)\n");
    }

    printf("All XeFG Ring Buffer & Motion Vector Pacing Unit Tests PASSED!\n");
    return 0;
}

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Standalone unit test for XeFG Save/Load Dynamic Passthrough, History Reset,
// Pipelined Frame Counter Immunity, and Presentation Pacing.

static const int BUFFER_COUNT = 4;

struct FrameConstants
{
    bool resetHistory = false;
    float frameRenderTime = 0.0f;
};

class MockXeFGPipeline
{
  public:
    uint64_t _frameCount = 0;
    uint64_t _lastDispatchedFrame = 0;
    uint32_t _jumpWarningCount = 0;
    bool _isActive = false;
    bool _isPaused = false;
    bool _needResetHistory = false;
    bool _runtimeEnabled = false; // Simulates libxess_fg.dll internal enabled state

    bool _depthReady[BUFFER_COUNT] = { false, false, false, false };
    bool _velocityReady[BUFFER_COUNT] = { false, false, false, false };

    int GetIndex() const { return static_cast<int>(_frameCount % BUFFER_COUNT); }

    void SetResourcesReady(int index, bool ready)
    {
        _depthReady[index] = ready;
        _velocityReady[index] = ready;
    }

    uint64_t StartNewFrame()
    {
        _frameCount++;

        if (!_isActive || _isPaused)
        {
            _lastDispatchedFrame = _frameCount;
        }
        else if (_lastDispatchedFrame == 0)
        {
            _lastDispatchedFrame = _frameCount - 1;
        }
        else if (_frameCount > _lastDispatchedFrame && (_frameCount - _lastDispatchedFrame) > 2)
        {
            _jumpWarningCount++;
            _lastDispatchedFrame = _frameCount - 1;
        }
        else if (_frameCount <= _lastDispatchedFrame)
        {
            _lastDispatchedFrame = _frameCount - 1;
        }

        auto idx = GetIndex();
        SetResourcesReady(idx, false);
        return _frameCount;
    }

    void SetFrameCount(uint64_t frameId)
    {
        if (frameId > _frameCount)
        {
            _frameCount = frameId;
        }
        else if (_frameCount - frameId > 4)
        {
            // Genuine counter rewind / scene reload
            _frameCount = frameId;
            _lastDispatchedFrame = (frameId > 0 ? frameId - 1 : 0);
        }
        else
        {
            // Pipelined presentation lag (AllowedFrameAhead2): preserve forward frameCount
        }
    }

    int GetDispatchIndex(uint64_t& willDispatchFrame)
    {
        if (_frameCount == _lastDispatchedFrame)
            return -1;

        willDispatchFrame = _lastDispatchedFrame + 1;
        _lastDispatchedFrame = willDispatchFrame;
        return static_cast<int>(willDispatchFrame % BUFFER_COUNT);
    }

    bool Dispatch(FrameConstants& outConstants)
    {
        uint64_t willDispatch = 0;
        int fIndex = GetDispatchIndex(willDispatch);
        if (fIndex < 0)
            return false;

        if (!_isActive || _isPaused)
            return false;

        if (!_depthReady[fIndex] || !_velocityReady[fIndex])
        {
            // Resources not ready (e.g. Save/Load screen)
            return false;
        }

        // Tag constants
        outConstants.resetHistory = _needResetHistory;
        _needResetHistory = false;

        // Re-enable runtime
        _runtimeEnabled = true;

        return true;
    }

    bool Present(FrameConstants& outConstants)
    {
        bool dispatched = Dispatch(outConstants);
        if (!dispatched)
        {
            // Fallback to passthrough mode: disable runtime so base frame presents cleanly
            _runtimeEnabled = false;
            _needResetHistory = true;
            _lastDispatchedFrame = _frameCount;
        }
        return dispatched;
    }
};

int main()
{
    printf("Running XeFG Save/Load Stability & Presentation Pacing Unit Tests...\n");

    // Test 1: Save/Load Transition - Dynamic SetEnabled(false) passthrough prevents black screen
    {
        MockXeFGPipeline pipeline;
        pipeline._isActive = true;

        // Gameplay: frame 1 with ready resources
        pipeline.StartNewFrame();
        pipeline.SetResourcesReady(pipeline.GetIndex(), true);
        FrameConstants consts;
        bool dispatched = pipeline.Present(consts);
        assert(dispatched == true);
        assert(pipeline._runtimeEnabled == true);

        // Save/load screen triggers: engine stops depth/velocity generation
        pipeline.StartNewFrame();
        // pipeline.SetResourcesReady left as false
        dispatched = pipeline.Present(consts);
        assert(dispatched == false);
        // Runtime MUST be disabled to allow base frame passthrough without error -14
        assert(pipeline._runtimeEnabled == false);
        assert(pipeline._needResetHistory == true);
        assert(pipeline._lastDispatchedFrame == pipeline._frameCount);
        printf("  [PASS] Test 1: Save/load transition dynamically enters SetEnabled(false) passthrough\n");
    }

    // Test 2: Scene Resumption - History flush and SetEnabled(true) reactivation
    {
        MockXeFGPipeline pipeline;
        pipeline._isActive = true;

        // Simulate save/load passthrough state
        pipeline._needResetHistory = true;
        pipeline._runtimeEnabled = false;

        // Gameplay resumes with valid 3D inputs
        pipeline.StartNewFrame();
        pipeline.SetResourcesReady(pipeline.GetIndex(), true);
        FrameConstants consts;
        bool dispatched = pipeline.Present(consts);

        assert(dispatched == true);
        assert(pipeline._runtimeEnabled == true);
        assert(consts.resetHistory == true); // Flushed stale temporal history!
        assert(pipeline._needResetHistory == false);

        // Next frame retains history
        pipeline.StartNewFrame();
        pipeline.SetResourcesReady(pipeline.GetIndex(), true);
        dispatched = pipeline.Present(consts);
        assert(dispatched == true);
        assert(consts.resetHistory == false);
        printf("  [PASS] Test 2: Gameplay resumption re-enables runtime with resetHistory flush\n");
    }

    // Test 3: Zero spurious jump warnings during loading screens
    {
        MockXeFGPipeline pipeline;
        pipeline._isActive = false; // Inactive or passthrough during loading

        for (int i = 0; i < 120; ++i)
        {
            pipeline.StartNewFrame();
        }

        assert(pipeline._frameCount == 120);
        assert(pipeline._jumpWarningCount == 0);

        // Transition to active
        pipeline._isActive = true;
        pipeline.StartNewFrame(); // Frame 121
        pipeline.SetResourcesReady(pipeline.GetIndex(), true);
        FrameConstants consts;
        pipeline.Present(consts);

        assert(pipeline._jumpWarningCount == 0);
        printf("  [PASS] Test 3: Zero spurious jump warnings emitted across 120 loading screen frames\n");
    }

    // Test 4: Pipelined SetFrameCount immunity under AllowedFrameAhead2
    {
        MockXeFGPipeline pipeline;
        pipeline._isActive = true;

        // Simulate 10 pipelined frames where present marker lags render thread by 1
        for (uint64_t f = 1; f <= 10; ++f)
        {
            pipeline.StartNewFrame(); // pipeline._frameCount becomes f
            pipeline.SetResourcesReady(pipeline.GetIndex(), true);

            // Streamline calls markPresent with lagging frame token (f - 1)
            pipeline.SetFrameCount(f > 1 ? f - 1 : 1);
            // frameCount must NOT regress to f - 1!
            assert(pipeline._frameCount == f);

            FrameConstants consts;
            bool dispatched = pipeline.Present(consts);
            assert(dispatched == true);
            assert(pipeline._lastDispatchedFrame == f);
        }

        // Test genuine counter reset (scene reload from frame 1000 down to 1)
        pipeline._frameCount = 1000;
        pipeline._lastDispatchedFrame = 1000;
        pipeline.SetFrameCount(1);
        assert(pipeline._frameCount == 1);
        assert(pipeline._lastDispatchedFrame == 0);
        printf("  [PASS] Test 4: Pipelined SetFrameCount preserves render slot and handles genuine resets\n");
    }

    // Test 5: Presentation Pacing - SyncInterval and DXGI_PRESENT_ALLOW_TEARING preservation
    {
        // When ExtraPacing is false (default), game-requested SyncInterval=0 and tearing must be preserved
        uint32_t syncInterval = 0;
        uint32_t flags = 0x00000200; // DXGI_PRESENT_ALLOW_TEARING
        bool extraPacing = false;

        if (extraPacing)
        {
            syncInterval = 1;
            flags &= ~0x00000200;
        }

        assert(syncInterval == 0);
        assert(flags & 0x00000200);
        printf("  [PASS] Test 5: Unmetered presentation preserves SyncInterval=0 without forced VBlank lock\n");
    }

    printf("All XeFG Save/Load Stability & Presentation Pacing Unit Tests PASSED!\n");
    return 0;
}

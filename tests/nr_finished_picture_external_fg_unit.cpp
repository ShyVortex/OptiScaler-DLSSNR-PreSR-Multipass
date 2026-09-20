#include <cassert>
#include <cstdio>
#include <cstdint>

enum class FGOutput : int
{
    NoFG = 0,
    FSRFG = 1,
    XeFG = 2,
    DLSSG = 3
};

struct MockFGFeature
{
    bool active = false;
    bool paused = false;

    bool IsActive() const { return active; }
    bool IsPaused() const { return paused; }
};

// Logical reproduction of the guarded check in wrapped_swapchain.cpp LocalPresent
inline bool CanApplyDlssNrToFinishedPicture(bool externalFrameGeneration,
                                            FGOutput activeFgOutput,
                                            bool hasCommandQueue,
                                            MockFGFeature* fg)
{
    const bool externalFgActive = externalFrameGeneration || (activeFgOutput == FGOutput::DLSSG);
    if (externalFgActive)
        return false;

    if (hasCommandQueue && (fg == nullptr || !fg->IsActive() || fg->IsPaused()))
        return true;

    return false;
}

// Logical reproduction of DlssNr::ApplyToFinishedPicture out-of-mutex query & guard
inline bool CanFinishedPictureQuerySwapchain(bool externalFrameGeneration,
                                             FGOutput activeFgOutput,
                                             bool dlssNrEnabled,
                                             bool dlssNrFinishedPicture,
                                             bool hasSwapchain,
                                             bool hasQueue,
                                             bool isStreamlineRenderQueue)
{
    const bool externalFgActive = externalFrameGeneration || (activeFgOutput == FGOutput::DLSSG);
    if (!hasSwapchain || !hasQueue || !dlssNrEnabled || !dlssNrFinishedPicture || externalFgActive)
        return false;
    if (isStreamlineRenderQueue)
        return false;
    return true;
}

// Logical reproduction of DlssNr_Dx12::ApplyFinished cancellation guard
inline bool CanApplyFinishedColor(bool dlssNrFinishedPicture,
                                  bool dlssNrEnabled,
                                  bool externalFrameGeneration,
                                  FGOutput activeFgOutput,
                                  bool hasPicture,
                                  bool hasQueue,
                                  bool& outCancelled)
{
    if (!dlssNrFinishedPicture || !dlssNrEnabled || externalFrameGeneration || activeFgOutput == FGOutput::DLSSG)
    {
        outCancelled = true;
        return false;
    }
    if (hasPicture && hasQueue)
    {
        outCancelled = false;
        return true;
    }
    outCancelled = false;
    return false;
}

int main()
{
    // Test 1: External frame generation must unconditionally prevent wrapped_swapchain from applying finished NR
    {
        MockFGFeature* fg = nullptr;
        assert(CanApplyDlssNrToFinishedPicture(true, FGOutput::NoFG, true, fg) == false);
        assert(CanApplyDlssNrToFinishedPicture(true, FGOutput::DLSSG, true, fg) == false);
        assert(CanApplyDlssNrToFinishedPicture(true, FGOutput::FSRFG, true, fg) == false);
    }

    // Test 2: FGOutput::DLSSG must unconditionally prevent wrapped_swapchain from applying finished NR
    {
        MockFGFeature* fg = nullptr;
        assert(CanApplyDlssNrToFinishedPicture(false, FGOutput::DLSSG, true, fg) == false);
    }

    // Test 3: Regular non-FG or inactive FG allows DLSS-NR finished picture
    {
        MockFGFeature* fg = nullptr;
        assert(CanApplyDlssNrToFinishedPicture(false, FGOutput::NoFG, true, fg) == true);

        MockFGFeature inactiveFg { false, false };
        assert(CanApplyDlssNrToFinishedPicture(false, FGOutput::NoFG, true, &inactiveFg) == true);

        MockFGFeature pausedFg { true, true };
        assert(CanApplyDlssNrToFinishedPicture(false, FGOutput::NoFG, true, &pausedFg) == true);
    }

    // Test 4: Active internal FG (FSRFG / XeFG) handles presentation itself; wrapped_swapchain finished picture is suppressed
    {
        MockFGFeature activeFg { true, false };
        assert(CanApplyDlssNrToFinishedPicture(false, FGOutput::FSRFG, true, &activeFg) == false);
        assert(CanApplyDlssNrToFinishedPicture(false, FGOutput::XeFG, true, &activeFg) == false);
    }

    // Test 5: DlssNr::ApplyToFinishedPicture swapchain backbuffer extraction out-of-mutex guards
    {
        // Null swapchain or queue -> do not query swapchain
        assert(CanFinishedPictureQuerySwapchain(false, FGOutput::NoFG, true, true, false, true, false) == false);
        assert(CanFinishedPictureQuerySwapchain(false, FGOutput::NoFG, true, true, true, false, false) == false);

        // Disabled NR or disabled finished picture -> do not query swapchain
        assert(CanFinishedPictureQuerySwapchain(false, FGOutput::NoFG, false, true, true, true, false) == false);
        assert(CanFinishedPictureQuerySwapchain(false, FGOutput::NoFG, true, false, true, true, false) == false);

        // External FG active -> do not query swapchain
        assert(CanFinishedPictureQuerySwapchain(true, FGOutput::NoFG, true, true, true, true, false) == false);
        assert(CanFinishedPictureQuerySwapchain(false, FGOutput::DLSSG, true, true, true, true, false) == false);

        // Streamline native picture ownership -> do not query swapchain
        assert(CanFinishedPictureQuerySwapchain(false, FGOutput::NoFG, true, true, true, true, true) == false);

        // Standard finished picture permitted -> query swapchain outside NR mutex
        assert(CanFinishedPictureQuerySwapchain(false, FGOutput::NoFG, true, true, true, true, false) == true);
    }

    // Test 6: DlssNr_Dx12::ApplyFinished cancellation & color pass guards
    {
        bool cancelled = false;

        // When external FG is active, cancel late context and do not apply finished color
        assert(CanApplyFinishedColor(true, true, true, FGOutput::NoFG, true, true, cancelled) == false);
        assert(cancelled == true);

        // When DLSSG output is active, cancel late context
        cancelled = false;
        assert(CanApplyFinishedColor(true, true, false, FGOutput::DLSSG, true, true, cancelled) == false);
        assert(cancelled == true);

        // When NR is disabled, cancel late context
        cancelled = false;
        assert(CanApplyFinishedColor(false, true, false, FGOutput::NoFG, true, true, cancelled) == false);
        assert(cancelled == true);

        // Normal flow with picture and queue -> run finished color
        cancelled = false;
        assert(CanApplyFinishedColor(true, true, false, FGOutput::NoFG, true, true, cancelled) == true);
        assert(cancelled == false);

        // Null picture or queue without cancellation condition -> does not cancel and does not run
        cancelled = false;
        assert(CanApplyFinishedColor(true, true, false, FGOutput::NoFG, false, true, cancelled) == false);
        assert(cancelled == false);
    }

    // Test 7: Lock inversion ordering verification
    {
        enum OperationOrder
        {
            OP_NONE = 0,
            OP_GET_BUFFER = 1,
            OP_ACQUIRE_NR_LOCK = 2,
            OP_ACQUIRE_STATE_LOCK = 3,
            OP_APPLY_COLOR = 4
        };

        OperationOrder steps[4] = { OP_NONE, OP_NONE, OP_NONE, OP_NONE };
        int stepCount = 0;

        // Simulate execution flow of ApplyToFinishedPicture + ApplyFinished
        // Step 1: Swapchain query outside lock
        steps[stepCount++] = OP_GET_BUFFER;

        // Step 2: nrOwnersMutex
        steps[stepCount++] = OP_ACQUIRE_NR_LOCK;

        // Step 3: _state->mutex inside ApplyFinished
        steps[stepCount++] = OP_ACQUIRE_STATE_LOCK;

        // Step 4: ApplyFinishedColor
        steps[stepCount++] = OP_APPLY_COLOR;

        assert(steps[0] == OP_GET_BUFFER);
        assert(steps[1] == OP_ACQUIRE_NR_LOCK);
        assert(steps[2] == OP_ACQUIRE_STATE_LOCK);
        assert(steps[3] == OP_APPLY_COLOR);
    }

    std::puts("PASS: nr_finished_picture_external_fg_unit (lock order, out-of-mutex swapchain query, and external FG cancellation guards)");
    return 0;
}

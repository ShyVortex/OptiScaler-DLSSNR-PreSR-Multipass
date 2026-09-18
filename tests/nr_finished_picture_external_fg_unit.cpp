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

// Logical reproduction of DlssNr_Dx12_FinishedQueue.cpp early return guard
inline bool CanFinishedQueueApply(bool externalFrameGeneration,
                                  FGOutput activeFgOutput,
                                  bool hasSwapchain,
                                  bool hasQueue,
                                  bool streamlineRenderQueueSet)
{
    if (!hasSwapchain || !hasQueue)
        return false;

    if (externalFrameGeneration || activeFgOutput == FGOutput::DLSSG)
        return false;

    if (streamlineRenderQueueSet)
        return false;

    return true;
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

    // Test 5: FinishedQueue defense-in-depth early returns
    {
        // Null pointers rejected
        assert(CanFinishedQueueApply(false, FGOutput::NoFG, false, true, false) == false);
        assert(CanFinishedQueueApply(false, FGOutput::NoFG, true, false, false) == false);

        // External FG rejected
        assert(CanFinishedQueueApply(true, FGOutput::NoFG, true, true, false) == false);
        assert(CanFinishedQueueApply(false, FGOutput::DLSSG, true, true, false) == false);

        // Streamline native picture ownership rejected
        assert(CanFinishedQueueApply(false, FGOutput::NoFG, true, true, true) == false);

        // Standard finished picture permitted
        assert(CanFinishedQueueApply(false, FGOutput::NoFG, true, true, false) == true);
    }

    std::puts("PASS: nr_finished_picture_external_fg_unit (wrapped_swapchain and FinishedQueue external FG presentation guards)");
    return 0;
}

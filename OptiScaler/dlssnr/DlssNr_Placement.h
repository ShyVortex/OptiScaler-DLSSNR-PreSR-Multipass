#pragma once

namespace DlssNr
{
struct Placement
{
    bool beforeUpscale;
    bool deferred;
    bool finished;
};

// The old across-RR key is accepted as an alias for the unified private-upscaler route.
constexpr Placement ResolvePlacement(bool before, bool deferred, bool legacyAcrossRr, bool finished)
{
    deferred = deferred || (before && legacyAcrossRr) || (finished && before);
    return { before || deferred, deferred, finished };
}

// Finished-picture NR edits the swapchain image at presentation. With external frame generation or DLSSG
// that image is shared with generated frames, so the runtime skips it (674da5df) to avoid the race.
struct FinishedPictureGate
{
    bool blocked;    // the runtime will not apply NR to the finished picture
    bool canToggle;  // the checkbox stays usable: it can always be turned off, never on while blocked
    bool notApplied; // the option is on but blocked, so no NR edit reaches the screen
};

constexpr FinishedPictureGate ResolveFinishedPictureGate(bool finished, bool externalFrameGeneration, bool dlssgOutput)
{
    const bool blocked = externalFrameGeneration || dlssgOutput;
    return { blocked, !blocked || finished, blocked && finished };
}

inline constexpr const char* FinishedPictureBlockedWarning =
    "Apply NR to the finished picture is disabled while external frame generation or DLSS-G is active: "
    "editing the presented image would race the generated frames.";
inline constexpr const char* FinishedPictureNotAppliedWarning =
    "It is still on, so no NR edit reaches the screen. Turn it off to apply NR normally.";
} // namespace DlssNr

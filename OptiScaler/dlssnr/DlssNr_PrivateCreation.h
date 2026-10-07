#pragma once

#include "DlssNr_GpuLifetime.h"
#include <mutex>

namespace DlssNr
{
// One helper per private feature generation. Record once after successful NGX
// creation and after the owner's resource-lifetime Record; never record evaluation
// lists here. Resource ownership remains the generation's separate responsibility.
// Methods are serialized internally. The caller must keep the helper alive during
// calls; captured submission callbacks retain shared state after owner destruction.
class PrivateFeatureCreation
{
    struct State
    {
        std::mutex mutex;
        GpuLifetime lifetime;
        std::function<bool()> completed;
    };
    std::shared_ptr<State> state = std::make_shared<State>();

  public:
    PrivateFeatureCreation() = default;
    PrivateFeatureCreation(const PrivateFeatureCreation&) = delete;
    PrivateFeatureCreation& operator=(const PrivateFeatureCreation&) = delete;

    void Record(ID3D12GraphicsCommandList* commands)
    {
        const auto captured = state;
        std::lock_guard lock(captured->mutex);
        if (!commands || captured->completed)
            return;
        captured->lifetime.Record(commands);
        captured->completed = captured->lifetime.CompletionProbe(commands);
    }

    GpuSubmission BeginSubmission(UINT count, ID3D12CommandList* const* lists)
    {
        const auto captured = state;
        std::lock_guard lock(captured->mutex);
        auto submission = captured->lifetime.BeginSubmission(count, lists);
        if (!submission)
            return {};
        // The production token pins the exact recording before Execute. Retaining
        // State avoids callbacks referencing a destroyed helper or reused address.
        auto token = std::make_shared<GpuSubmission>(std::move(submission));
        return GpuSubmission([captured, token](ID3D12CommandQueue* queue) { token->Complete(queue); });
    }

    void ResetRecording(ID3D12CommandList* commands)
    {
        const auto captured = state;
        std::lock_guard lock(captured->mutex);
        captured->lifetime.ResetRecording(commands);
    }

    void QuarantineSubmission(UINT count, ID3D12CommandList* const* lists)
    {
        const auto captured = state;
        std::lock_guard lock(captured->mutex);
        captured->lifetime.QuarantineSubmission(count, lists);
    }

    bool Ready()
    {
        const auto captured = state;
        std::lock_guard lock(captured->mutex);
        return captured->completed && captured->completed();
    }

    bool Discarded()
    {
        const auto captured = state;
        std::lock_guard lock(captured->mutex);
        // Closed, never-submitted recordings collect immediately. Pending tokens,
        // failed signals and removed-device fences remain unresolved/quarantined.
        // Collection may observe GPU progress; inspect the retained probe afterward.
        return captured->completed && captured->lifetime.Idle() && !captured->completed();
    }

    bool Idle()
    {
        const auto captured = state;
        std::lock_guard lock(captured->mutex);
        return captured->lifetime.Idle();
    }

    // Retired generations only: the owner must have excluded further replay.
    void FinishSubmitted()
    {
        const auto captured = state;
        std::lock_guard lock(captured->mutex);
        captured->lifetime.FinishSubmitted();
    }
};
} // namespace DlssNr

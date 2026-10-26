#pragma once

#include <d3d12.h>
#include <functional>
#include <memory>
#include "DlssNr_GpuSubmission.h"

namespace DlssNr
{
// NR-local lifetime tracking. Record/submit/reset/retire/collect calls are serialized internally.
// Owners must still guard their resources and outlive all calls. Lock order when owned by NR:
// owner registry -> NR state -> tracker; retirement callbacks may re-enter on the calling thread.
class GpuLifetime
{
    struct Impl;
    std::shared_ptr<Impl> impl;

  public:
    GpuLifetime();
    ~GpuLifetime();
    GpuLifetime(const GpuLifetime&) = delete;
    GpuLifetime& operator=(const GpuLifetime&) = delete;
    void Record(ID3D12GraphicsCommandList* commands);
    // The probe retains the captured tracker state; discarded work never satisfies it.
    std::function<bool()> CompletionProbe(ID3D12GraphicsCommandList* commands);
    // One reusable monotonic fence per queue; aliases are normalized at every notification.
    // Capture before the real ExecuteCommandLists; complete after it, including on replay.
    GpuSubmission BeginSubmission(UINT count, ID3D12CommandList* const* lists);
    // Allocation-free failure path, while the real application batch will still
    // execute. Matching generations cannot retire/reuse storage after Reset.
    bool QuarantineSubmission(UINT count, ID3D12CommandList* const* lists);
    // Convenience for callers which already serialize Execute and Reset themselves.
    void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
    void ResetRecording(ID3D12CommandList* commands);
    // Start tracking a replacement resource set. Older recordings still receive submit/reset
    // notifications, but only recordings used again belong to the new generation.
    void BeginGeneration();
    // Unresolved callbacks, including their captured ownership, are retained at destruction.
    // The caller supplies a live object, never a stored identity that may be destroyed.
    bool HasOpenRecording(ID3D12CommandList* commands);
    void Retire(std::function<void()> destroy);
    void Collect();
    // All captured executions completed; recordings may still be replayable.
    bool GpuComplete();
    bool Idle();
    // Retired owners only: completed submissions can no longer be replayed by this owner.
    // Unsubmitted recordings and failed/removed-device fences remain unresolved.
    void FinishSubmitted();
};
} // namespace DlssNr

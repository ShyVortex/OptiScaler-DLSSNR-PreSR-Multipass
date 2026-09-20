#pragma once
#include "DlssNr_GpuLifetime.h"
#include <Util.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace DlssNr
{
// A recording owns all of its descriptor/upload slots until reset/destruction and
// every captured execution completes. Recording trackers are reused, so private
// data keys and queue fences do not grow with the number of frames.
template <unsigned Capacity> class DescriptorSlots
{
    static_assert(Capacity > 0 && Capacity <= 65536);
    static constexpr unsigned SlotBits = std::max(1, std::bit_width(Capacity - 1u));
    static constexpr uint32_t SlotMask = (1u << SlotBits) - 1u;
    static constexpr uint32_t MaxGeneration = (UINT32_MAX >> SlotBits) - 1u;
    struct Recording
    {
        GpuLifetime lifetime;
        ID3D12CommandList* commands = nullptr;
        bool active = false, open = false;
    };
    struct Slot
    {
        std::shared_ptr<Recording> recording;
        uint32_t generation = 0;
    };
    std::array<Slot, Capacity> slots;
    std::vector<std::shared_ptr<Recording>> recordings;
    std::mutex mutex;
    uint32_t next = 0;
    template <class T> static T* Identity(T* object)
    {
        T* real = nullptr;
        return object && Util::CheckForRealObject(__FUNCTION__, object, (IUnknown**) &real) ? real : object;
    }
    void Collect()
    {
        for (auto& recording : recordings)
            if (recording->active && recording->lifetime.Idle())
            {
                recording->active = recording->open = false;
                recording->commands = nullptr;
            }
        for (auto& slot : slots)
            if (slot.recording && !slot.recording->active)
                slot.recording.reset();
    }

  public:
    std::optional<uint32_t> Acquire(ID3D12GraphicsCommandList* commands, const uint32_t* immutable = nullptr)
    {
        if (!commands)
            return {};
        commands = Identity(commands);
        std::lock_guard lock(mutex);
        Collect();
        if (immutable && *immutable != UINT32_MAX)
        {
            const auto index = *immutable & SlotMask, generation = *immutable >> SlotBits;
            if (index >= Capacity || generation == 0)
                return {};
            const auto& slot = slots[index];
            if (slot.generation != generation || !slot.recording || !slot.recording->open ||
                slot.recording->commands != commands)
                return {};
            if (!slot.recording->lifetime.HasOpenRecording(commands))
            {
                slot.recording->open = false;
                return {};
            }
            return index;
        }
        for (unsigned n = 0; n < Capacity; ++n)
        {
            const auto index = (next + n) % Capacity;
            auto& slot = slots[index];
            if (slot.recording || slot.generation == MaxGeneration)
                continue;
            std::shared_ptr<Recording> recording;
            for (auto& candidate : recordings)
                if (candidate->active && candidate->open && candidate->commands == commands)
                {
                    // Stored pointers are identities only. Query with this live caller
                    // object so destruction/address reuse cannot reopen an old generation.
                    if (candidate->lifetime.HasOpenRecording(commands))
                    {
                        recording = candidate;
                        break;
                    }
                    candidate->open = false;
                }
            if (!recording)
            {
                for (auto& candidate : recordings)
                    if (!candidate->active)
                    {
                        recording = candidate;
                        break;
                    }
                if (!recording)
                {
                    if (recordings.size() == Capacity)
                        return {};
                    recording = std::make_shared<Recording>();
                    recordings.push_back(recording);
                }
                recording->commands = commands;
                recording->active = recording->open = true;
                recording->lifetime.Record(commands);
            }
            slot.recording = std::move(recording);
            ++slot.generation; // Never wrap: an exhausted slot stays unavailable.
            next = (index + 1) % Capacity;
            return index;
        }
        return {}; // Bounded exhaustion skips the affected NR frame; no GPU wait.
    }
    bool PublishImmutable(ID3D12GraphicsCommandList* commands, uint32_t index, uint32_t& immutable)
    {
        commands = Identity(commands);
        std::lock_guard lock(mutex);
        if (index >= Capacity)
            return false;
        const auto& slot = slots[index];
        if (!slot.recording || !slot.recording->active || !slot.recording->open ||
            slot.recording->commands != commands || !slot.recording->lifetime.HasOpenRecording(commands))
            return false;
        immutable = (slot.generation << SlotBits) | index;
        return true;
    }
    void ResetRecording(ID3D12CommandList* commands)
    {
        commands = Identity(commands);
        std::lock_guard lock(mutex);
        for (auto& recording : recordings)
            if (recording->active && recording->commands == commands)
            {
                recording->open = false;
                recording->lifetime.ResetRecording(commands);
            }
        Collect();
    }
    GpuSubmission BeginSubmission(UINT count, ID3D12CommandList* const* lists)
    {
        std::lock_guard lock(mutex);
        std::shared_ptr<std::vector<GpuSubmission>> pending;
        for (auto& recording : recordings)
            if (recording->active && recording->open)
            {
                auto child = recording->lifetime.BeginSubmission(count, lists);
                if (child)
                {
                    if (!pending)
                    {
                        pending = std::make_shared<std::vector<GpuSubmission>>();
                        pending->reserve(recordings.size());
                    }
                    pending->push_back(std::move(child));
                }
            }
        if (!pending)
            return {};
        // Children own the tracker implementations, independent of the codec's lifetime.
        return GpuSubmission(
            [pending](ID3D12CommandQueue* queue)
            {
                try
                {
                    for (auto& child : *pending)
                        child.Complete(queue);
                }
                catch (...)
                {
                    for (auto& child : *pending)
                        try
                        {
                            child.Complete(nullptr);
                        }
                        catch (...)
                        {
                        }
                    throw;
                }
            });
    }
    void FinishSubmitted()
    {
        std::lock_guard lock(mutex);
        for (auto& recording : recordings)
            if (recording->active)
                recording->lifetime.FinishSubmitted();
        Collect();
    }
    bool Idle()
    {
        std::lock_guard lock(mutex);
        Collect();
        return std::none_of(recordings.begin(), recordings.end(), [](const auto& r) { return r->active; });
    }
};
} // namespace DlssNr

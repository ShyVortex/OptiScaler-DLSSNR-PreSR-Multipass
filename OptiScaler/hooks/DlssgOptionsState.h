#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

// UI intent is copied independently of the caller-owned Streamline structures.
// A UI edit never calls Streamline; the game retains ownership of API ordering.
class DlssgOptionsState
{
  public:
    struct Values
    {
        std::optional<int> generatedFrames;
        bool forceDynamic = false;
        std::optional<float> dynamicTarget;
    };
    struct Snapshot
    {
        Values values;
        uint64_t generation;
    };

    void Initialize(Values values)
    {
        std::lock_guard lock(mutex_);
        if (!initialized_)
        {
            values_ = values;
            initialized_ = true;
        }
    }

    void Queue(Values values)
    {
        std::lock_guard lock(mutex_);
        values_ = values;
        initialized_ = true;
        ++generation_;
    }

    Snapshot Read() const
    {
        std::lock_guard lock(mutex_);
        return { values_, generation_ };
    }

    void Accepted(uint64_t generation)
    {
        std::lock_guard lock(mutex_);
        // An older in-flight API result cannot acknowledge a newer UI edit.
        if (generation == generation_)
            accepted_ = generation;
    }

    bool Pending() const
    {
        std::lock_guard lock(mutex_);
        return accepted_ != generation_;
    }

  private:
    mutable std::mutex mutex_;
    Values values_ {};
    uint64_t generation_ = 0;
    uint64_t accepted_ = 0;
    bool initialized_ = false;
};

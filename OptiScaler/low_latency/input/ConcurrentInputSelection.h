#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

template <typename Input, typename Output, size_t InputCount> class ConcurrentInputSelection
{
    static_assert(std::is_enum_v<Input> && std::is_enum_v<Output>);
    static_assert(InputCount > 0 && InputCount <= 64);

    std::atomic<uint64_t> available_ { 0 };
    std::atomic<uint64_t> selected_ { 0 };

    static constexpr uint64_t Pack(Input input, Output output) noexcept
    {
        return static_cast<uint32_t>(input) | (static_cast<uint64_t>(static_cast<uint32_t>(output)) << 32);
    }

  public:
    struct Selection
    {
        Input input;
        Output output;
    };

    void MarkAvailable(Input input) noexcept
    {
        const auto index = static_cast<size_t>(input);
        if (index < InputCount)
            available_.fetch_or(uint64_t { 1 } << index, std::memory_order_release);
    }

    bool IsAvailable(Input input) const noexcept
    {
        const auto index = static_cast<size_t>(input);
        return index < InputCount && (available_.load(std::memory_order_acquire) & (uint64_t { 1 } << index)) != 0;
    }

    size_t AvailableCount() const noexcept { return std::popcount(available_.load(std::memory_order_acquire)); }

    uint64_t AvailableMask() const noexcept { return available_.load(std::memory_order_acquire); }

    Selection ReadSelection() const noexcept
    {
        const uint64_t value = selected_.load(std::memory_order_acquire);
        return { static_cast<Input>(static_cast<uint32_t>(value)), static_cast<Output>(value >> 32) };
    }

    void SetInput(Input input) noexcept
    {
        uint64_t old = selected_.load(std::memory_order_relaxed);
        uint64_t next;
        do
        {
            next = Pack(input, static_cast<Output>(old >> 32));
        } while (!selected_.compare_exchange_weak(old, next, std::memory_order_release, std::memory_order_relaxed));
    }

    void SetOutput(Output output) noexcept
    {
        uint64_t old = selected_.load(std::memory_order_relaxed);
        uint64_t next;
        do
        {
            next = Pack(static_cast<Input>(static_cast<uint32_t>(old)), output);
        } while (!selected_.compare_exchange_weak(old, next, std::memory_order_release, std::memory_order_relaxed));
    }
};

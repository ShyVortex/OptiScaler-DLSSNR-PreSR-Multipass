#pragma once
#include <d3d12.h>
#include <functional>
#include <utility>

namespace DlssNr
{
// Captures recording identity BEFORE ExecuteCommandLists; Complete attaches its fence AFTER it.
// No mutex is held by the token. An abandoned token completes with nullptr (fail closed).
class GpuSubmission
{
    std::function<void(ID3D12CommandQueue*)> complete;

  public:
    GpuSubmission() = default;
    explicit GpuSubmission(std::function<void(ID3D12CommandQueue*)> callback) : complete(std::move(callback)) {}
    GpuSubmission(const GpuSubmission&) = delete;
    GpuSubmission& operator=(const GpuSubmission&) = delete;
    GpuSubmission(GpuSubmission&& other) noexcept { complete.swap(other.complete); }
    GpuSubmission& operator=(GpuSubmission&& other) noexcept
    {
        if (this != &other)
        {
            Abandon();
            complete.swap(other.complete);
        }
        return *this;
    }
    ~GpuSubmission() { Abandon(); }
    explicit operator bool() const { return bool(complete); }
    void Complete(ID3D12CommandQueue* queue)
    {
        auto callback = std::move(complete);
        complete = {};
        if (callback)
            callback(queue);
    }

  private:
    void Abandon() noexcept
    {
        // If a callback cannot finish, its pending pins remain set. Never turn an
        // exception during stack unwinding into evidence that GPU work completed.
        try
        {
            Complete(nullptr);
        }
        catch (...)
        {
        }
    }
};
} // namespace DlssNr

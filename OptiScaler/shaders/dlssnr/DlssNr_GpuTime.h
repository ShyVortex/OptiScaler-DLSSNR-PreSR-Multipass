#pragma once
#include <dlssnr/DlssNr_GpuLifetime.h>
#include <vector>

// NR calls are serialized by g_nrMutex, including submission/reset notifications.
// Associate every query pair with its actual submitting queue and GPU completion.
class DlssNrGpuTime
{
    using Resource = Microsoft::WRL::ComPtr<ID3D12Resource>;
    struct Sample
    {
        DlssNr::GpuLifetime lifetime;
        ID3D12CommandList* commands = nullptr; // identity only
        UINT64 frequency = 0, sequence = 0;
        bool occupied = false, ended = false, submitted = false, readBack = false;
    };
    static constexpr unsigned Count = 8;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
    Resource readback;
    using Samples = std::array<Sample, Count>;
    std::shared_ptr<Samples> samples = std::make_shared<Samples>();
    int recording = -1;
    UINT64 sequence = 0, lastSequence = 0;
    std::optional<double> last;

    void Collect()
    {
        for (unsigned i = 0; i < Count; ++i)
        {
            auto& s = (*samples)[i];
            if (!s.occupied || !s.lifetime.GpuComplete())
                continue;
            const bool reusable = s.lifetime.Idle();
            if (!s.submitted && !reusable)
                continue;
            if (!s.submitted)
            {
                s.occupied = false;
                continue;
            }
            if (!s.readBack)
            {
                UINT64* data = nullptr;
                D3D12_RANGE range { i * 2 * sizeof(UINT64), (i * 2 + 2) * sizeof(UINT64) };
                if (SUCCEEDED(readback->Map(0, &range, (void**) &data)))
                {
                    const auto begin = data[i * 2], end = data[i * 2 + 1];
                    if (s.sequence > lastSequence && s.frequency && begin && end >= begin)
                    {
                        last = double(end - begin) * 1000.0 / double(s.frequency);
                        lastSequence = s.sequence;
                    }
                    D3D12_RANGE written { 0, 0 };
                    readback->Unmap(0, &written);
                    s.readBack = true;
                }
            }
            s.occupied = !reusable;
        }
    }

  public:
    explicit DlssNrGpuTime(ID3D12Device* device)
    {
        D3D12_QUERY_HEAP_DESC queryDesc { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, Count * 2, 0 };
        if (FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&queries))))
            return;
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(Count * 2 * sizeof(UINT64));
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&readback))))
            return;
    }

    ~DlssNrGpuTime()
    {
        // A recorded/replayable list still borrows this storage, even if the
        // timing owner is retired. Failed/abandoned submissions are quarantined.
        for (auto& s : *samples)
            if (!s.lifetime.Idle())
            {
                queries.Detach();
                readback.Detach();
                break;
            }
    }

    void Start(ID3D12GraphicsCommandList* cmd)
    {
        recording = -1;
        if (!readback)
            return;
        Collect();
        for (unsigned i = 0; i < Count; ++i)
        {
            auto& s = (*samples)[i];
            if (s.occupied)
                continue;
            s.commands = cmd;
            ID3D12GraphicsCommandList* real = nullptr;
            if (Util::CheckForRealObject(__FUNCTION__, cmd, (IUnknown**) &real))
                s.commands = real;
            s.occupied = true;
            s.ended = s.submitted = s.readBack = false;
            s.sequence = ++sequence;
            s.lifetime.Record(cmd);
            recording = (int) i;
            cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i * 2);
            break;
        }
    }

    void End(ID3D12GraphicsCommandList* cmd)
    {
        if (recording < 0)
            return;
        const auto i = (unsigned) recording;
        cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i * 2 + 1);
        cmd->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i * 2, 2, readback.Get(),
                              i * 2 * sizeof(UINT64));
        (*samples)[i].ended = true;
        recording = -1;
    }

    DlssNr::GpuSubmission BeginSubmission(UINT count, ID3D12CommandList* const* lists)
    {
        struct Pending
        {
            unsigned index;
            DlssNr::GpuSubmission submission;
        };
        std::shared_ptr<std::vector<Pending>> pending;
        for (unsigned i = 0; i < Count; ++i)
        {
            auto& s = (*samples)[i];
            if (!s.occupied)
                continue;
            auto submission = s.lifetime.BeginSubmission(count, lists);
            if (submission)
            {
                // Replay writes the same query pair again. Read it once after
                // all captured executions finish, while preserving display order.
                s.readBack = false;
                if (!pending)
                    pending = std::make_shared<std::vector<Pending>>();
                pending->push_back({ i, std::move(submission) });
            }
        }
        if (!pending)
            return {};
        return DlssNr::GpuSubmission(
            [storage = samples, pending](ID3D12CommandQueue* queue)
            {
                for (auto& item : *pending)
                {
                    auto& s = (*storage)[item.index];
                    s.submitted = true;
                    if (!queue || !s.ended || FAILED(queue->GetTimestampFrequency(&s.frequency)))
                        s.frequency = 0;
                    item.submission.Complete(queue);
                }
            });
    }

    void QuarantineSubmission(UINT count, ID3D12CommandList* const* lists)
    {
        for (auto& s : *samples)
            if (s.occupied)
                s.lifetime.QuarantineSubmission(count, lists);
    }

    void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
    {
        BeginSubmission(count, lists).Complete(queue);
    }

    void ResetRecording(ID3D12CommandList* cmd)
    {
        for (auto& s : *samples)
            if (s.occupied)
                s.lifetime.ResetRecording(cmd);
    }

    void ClearLast()
    {
        last.reset();
        lastSequence = sequence; // older, in-flight samples must not repopulate the display
    }

    // Retired owners only: stop replay tracking without declaring an in-flight sample complete.
    void FinishSubmitted()
    {
        for (auto& s : *samples)
            if (s.occupied)
                s.lifetime.FinishSubmitted();
    }

    std::optional<double> ReadGpuTime()
    {
        Collect();
        return last;
    }
};

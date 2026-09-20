#include "pch.h"
#include "DlssNr_Dx12_State.h"
#include <dlssnr/DlssNr_StreamlinePicture.h>

auto DlssNr_Dx12::State::FinishedPictureResetCommandList(ID3D12CommandList* cmd) -> void
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    lifetime.ResetRecording(cmd);
    deferredSr.lifetime.ResetRecording(cmd);
    captureFrames.ResetRecording(cmd);
    if (enlarger)
        enlarger->lifetime.ResetRecording(cmd);
    for (auto& old : retiredEnlargers)
        old->lifetime.ResetRecording(cmd);
    CollectEnlargers();
    ID3D12CommandList* real = nullptr;
    auto* identity = Util::CheckForRealObject(__FUNCTION__, cmd, (IUnknown**) &real) ? real : cmd;
    if (enlarger && !enlarger->submitted && !enlarger->pendingSubmissions && enlarger->creation == identity)
    {
        enlarger->failed = true;
        enlargementStatus = "DLSS enlargement initialization was discarded; use Retry.";
    }
    for (auto& model : nr.models)
        model.ResetRecording(cmd);
    if (inputHold.captureCommands == identity && std::find(pendingHoldGenerations.begin(), pendingHoldGenerations.end(),
                                                           inputHold.generation) == pendingHoldGenerations.end())
    {
        inputHold.active = false; // recording was discarded before submission
        inputHold.captureCommands = nullptr;
        nr.heldActive = false;
    }
    if (gpuTime)
        gpuTime->ResetRecording(cmd);
    if (ngxTime)
        ngxTime->ResetRecording(cmd);
    if (!late.tracking.load())
        return;
    for (auto& slot : late.slots)
    {
        slot.producerLifetime.ResetRecording(cmd);
        late.DiscardUnsubmitted(slot);
    }
}

auto DlssNr_Dx12::State::WaitForFinishedPicture() -> bool
{
    if (!late.tracking.load())
        return true;
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (pendingSubmissions)
        return false;
    late.Cancel();
    for (auto& slot : late.slots)
    {
        // Do not wait for game-owned recording resets or unfinished replay queues.
        // In particular, the original ready/done signal cannot prove a replay finished.
        if (!slot.producerLifetime.Idle())
            return false;
        if (!slot.submitted || late.Finished(slot))
            continue;
        if (slot.fence->GetCompletedValue() == UINT64_MAX)
            return false; // Device removal is not a completed submission.
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event)
            return false;
        const auto hr = slot.fence->SetEventOnCompletion(slot.done, event);
        const bool finished = SUCCEEDED(hr) && WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
        CloseHandle(event);
        if (!finished || !late.Finished(slot))
            return false;
    }
    return late.dx11.Drain();
}

auto DlssNr_Dx12::State::FinishedPictureStatus() -> std::string
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return late.status;
}

auto DlssNr_Dx12::State::FinishedPictureSubmitted(ID3D12CommandQueue* queue, UINT count,
                                                  ID3D12CommandList* const* lists) -> void
{
    BeginFinishedPictureSubmission(count, lists).Complete(queue);
}

auto DlssNr_Dx12::State::BeginFinishedPictureSubmission(UINT count, ID3D12CommandList* const* lists)
    -> DlssNr::GpuSubmission
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!lists || !count)
        return {};
    auto matches = [&](ID3D12CommandList* commands)
    {
        if (!commands)
            return false;
        for (UINT i = 0; i < count; ++i)
        {
            ID3D12CommandList* realList = nullptr;
            auto* list = Util::CheckForRealObject(__FUNCTION__, lists[i], (IUnknown**) &realList) ? realList : lists[i];
            if (list == commands)
                return true;
        }
        return false;
    };
    std::shared_ptr<std::vector<DlssNr::GpuSubmission>> pending;
    auto add = [&](DlssNr::GpuSubmission submission)
    {
        if (!submission)
            return;
        if (!pending)
            pending = std::make_shared<std::vector<DlssNr::GpuSubmission>>();
        pending->push_back(std::move(submission));
    };
    // Parent ownership is completed LAST: its retirement callbacks can destroy
    // the timers/enlargers whose exact generations we capture here.
    auto parent = lifetime.BeginSubmission(count, lists);
    add(deferredSr.lifetime.BeginSubmission(count, lists));
    add(captureFrames.BeginSubmission(count, lists));
    if (gpuTime)
        add(gpuTime->BeginSubmission(count, lists));
    if (ngxTime)
        add(ngxTime->BeginSubmission(count, lists));
    for (auto& model : nr.models)
        add(model.BeginSubmission(count, lists));
    bool enlargerRecorded = false;
    if (enlarger)
    {
        auto work = enlarger->lifetime.BeginSubmission(count, lists);
        enlargerRecorded = bool(work);
        add(std::move(work));
    }
    for (auto& old : retiredEnlargers)
        add(old->lifetime.BeginSubmission(count, lists));
    // A discarded creation can leave the same command-list pointer behind.
    // Only a captured, still-valid creation may receive the delayed notification.
    auto* initialization = enlargerRecorded && !enlarger->submitted && !enlarger->failed && matches(enlarger->creation)
                               ? enlarger.get()
                               : nullptr;
    const bool hold = matches(inputHold.captureCommands);
    const auto holdGeneration = inputHold.generation;
    std::vector<LateContext::Slot*> copies;
    if (late.tracking.load())
        for (auto& slot : late.slots)
        {
            // Presentation/cancellation does not close a replayable producer recording.
            auto work = slot.producerLifetime.BeginSubmission(count, lists);
            const bool recorded = bool(work);
            add(std::move(work));
            // An address reused after destruction is not the captured producer generation.
            if (recorded && slot.pending && !slot.submitted && !slot.quarantined)
                copies.push_back(&slot);
        }
    add(std::move(parent));
    if (!pending && !initialization && !hold && copies.empty())
        return {};
    // Finish all allocation before committing owner pins. Reserving here makes
    // the later uint64_t insertion nonthrowing, even after the token is armed.
    if (hold)
        pendingHoldGenerations.reserve(pendingHoldGenerations.size() + 1);
    DlssNr::GpuSubmission submission(
        [this, pending, initialization, hold, holdGeneration, copies](ID3D12CommandQueue* queue)
        {
            std::lock_guard<std::recursive_mutex> lock(mutex);
            ID3D12CommandQueue* real = nullptr;
            auto* identity = queue && Util::CheckForRealObject(__FUNCTION__, queue, (IUnknown**) &real) ? real : queue;
            // These pointers/generations were selected before Execute; never select
            // new recordings from a command-list pointer that Reset may have reused.
            if (initialization)
            {
                initialization->queue = identity;
                initialization->submitted = true;
                initialization->failed |= !queue;
                --initialization->pendingSubmissions;
            }
            if (hold)
            {
                auto it = std::find(pendingHoldGenerations.begin(), pendingHoldGenerations.end(), holdGeneration);
                if (it != pendingHoldGenerations.end())
                    pendingHoldGenerations.erase(it);
                if (inputHold.generation == holdGeneration)
                {
                    inputHold.captureCommands = nullptr;
                    if (!queue)
                    {
                        inputHold.active = false;
                        nr.heldActive = false;
                    }
                }
            }
            for (auto* slot : copies)
            {
                slot->submitted = true;
                late.producerQueue = queue;
                slot->producerQueue = identity;
                if (!queue || FAILED(queue->Signal(slot->fence.Get(), slot->ready)))
                {
                    slot->pending = false;
                    late.Say("The graphics queue stopped. Restart the game to retry.");
                }
                --slot->pendingSubmissions;
            }
            if (pending)
                for (auto& item : *pending)
                    item.Complete(queue);
            CollectEnlargers();
            --pendingSubmissions;
        });
    if (hold)
        pendingHoldGenerations.push_back(holdGeneration);
    if (initialization)
        ++initialization->pendingSubmissions;
    for (auto* slot : copies)
        ++slot->pendingSubmissions;
    ++pendingSubmissions;
    return submission;
}

auto DlssNr_Dx12::State::FinishedColorSpace(IDXGISwapChain* swapchain, DXGI_FORMAT format) -> DXGI_COLOR_SPACE_TYPE
{
    auto space = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                                          : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    UINT size = sizeof(space);
    swapchain->GetPrivateData(DlssNr::FinishedColorSpaceKey, &size, &space);
    return space;
}

auto DlssNr_Dx12::State::ApplyToFinishedPictureDx11(IDXGISwapChain* swapchain) -> void
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!swapchain || !late.device || !late.producerQueue ||
        !Config::Instance()->DlssNrFinishedPicture.value_or_default() ||
        !Config::Instance()->DlssNrEnabled.value_or_default())
        return;
    const bool heldPicture = Config::Instance()->DlssNrHoldFrame.value_or_default() && inputHold.active &&
                             late.heldValid && late.heldGeneration == inputHold.generation;
    if (!heldPicture && std::none_of(late.slots.begin(), late.slots.end(),
                                     [](const auto& slot) { return slot.pending && slot.submitted; }))
        return;
    LateContext::ComPtr<IDXGISwapChain3> sc;
    LateContext::ComPtr<ID3D11Texture2D> picture;
    DXGI_SWAP_CHAIN_DESC desc {};
    if (FAILED(swapchain->GetDesc(&desc)))
        return;
    // Blt-model chains expose buffer zero; flip-model chains rotate the current index.
    UINT index = 0;
    if ((desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD || desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL) &&
        SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&sc))))
        index = sc->GetCurrentBackBufferIndex();
    if (FAILED(swapchain->GetBuffer(index, IID_PPV_ARGS(&picture))))
        return;
    auto* color = late.dx11.Begin(picture.Get(), late.device.Get(), late.producerQueue.Get());
    if (!color)
    {
        late.Say("The DirectX 11 finished-picture bridge is unavailable for this device or screen format.");
        return;
    }
    const bool ran =
        ApplyFinishedColor(color, late.producerQueue.Get(), FinishedColorSpace(swapchain, color->GetDesc().Format));
    if (!late.dx11.End(picture.Get(), ran))
        late.Say("The DirectX 11 finished-picture transfer failed. Restart the game to retry.");
}

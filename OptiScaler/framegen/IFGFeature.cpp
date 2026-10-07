#include "pch.h"
#include "IFGFeature.h"
#include <Config.h>
#include <low_latency/input/input_common.h>

int IFGFeature::GetIndex() { return (_frameCount % BUFFER_COUNT); }

int IFGFeature::GetIndexWillBeDispatched()
{
    UINT64 df = 0;
    int slot = ResolveDispatchSlot(df);
    if (slot >= 0)
        return slot;

    return GetIndex();
}

UINT64 IFGFeature::StartNewFrame()
{
    _frameCount++;

    if (!IsActive() || IsPaused())
    {
        _lastDispatchedFrame = _frameCount;
        _actuallyDispatchedFrame = _frameCount;
    }
    else if (_lastDispatchedFrame == 0)
    {
        _lastDispatchedFrame = _frameCount - 1;
    }
    else if (_frameCount > _lastDispatchedFrame && (_frameCount - _lastDispatchedFrame) > 2)
    {
        LOG_WARN("Frame count jumped too much! _frameCount: {}, _lastDispatchedFrame: {}", _frameCount,
                 _lastDispatchedFrame);

        _lastDispatchedFrame = _frameCount - 1;
    }
    else if (_frameCount <= _lastDispatchedFrame)
    {
        _lastDispatchedFrame = _frameCount - 1;
    }

    auto fIndex = GetIndex();
    LOG_DEBUG("_frameCount: {}, fIndex: {}", _frameCount, fIndex);

    _resourceReady[fIndex].clear();
    _waitingExecute[fIndex] = false;

    _noUi[fIndex] = true;
    _noDistortionField[fIndex] = true;
    _noHudless[fIndex] = true;

    NewFrame();

    return _frameCount;
}

bool IFGFeature::IsResourceReady(FG_ResourceType type, int index)
{
    if (index < 0)
        index = GetIndex();

    return _resourceReady[index].contains(type);
}

bool IFGFeature::WaitingExecution(int index)
{
    if (index < 0)
        index = GetIndex();

    return _waitingExecute[index];
}
void IFGFeature::SetExecuted(int index)
{
    if (index < 0)
        index = GetIndex();

    _waitingExecute[index] = false;
}

bool IFGFeature::IsUsingUI() { return !_noUi[GetIndex()]; }
bool IFGFeature::IsUsingUIAny()
{
    for (const auto& value : _noUi)
        if (value == false)
            return true;

    return false;
}
bool IFGFeature::IsUsingDistortionField() { return !_noDistortionField[GetIndex()]; }
bool IFGFeature::IsUsingHudless(int index)
{
    if (index < 0)
        index = GetIndex();

    return !_noHudless[index];
}

bool IFGFeature::IsUsingHudlessAny()
{
    for (const auto& value : _noHudless)
        if (value == false)
            return true;

    return false;
}

bool IFGFeature::CheckForRealObject(std::string functionName, IUnknown* pObject, IUnknown** ppRealObject)
{
    if (streamlineRiid.Data1 == 0)
    {
        auto iidResult = IIDFromString(L"{ADEC44E2-61F0-45C3-AD9F-1B37379284FF}", &streamlineRiid);

        if (iidResult != S_OK)
            return false;
    }

    auto qResult = pObject->QueryInterface(streamlineRiid, (void**) ppRealObject);

    if (qResult == S_OK && *ppRealObject != nullptr)
    {
        LOG_INFO("{} Streamline proxy found!", functionName);
        (*ppRealObject)->Release();
        return true;
    }

    return false;
}

bool IFGFeature::IsSlotReady(int index) const
{
    if (index < 0 || index >= BUFFER_COUNT)
        return false;

    auto itDepth = _resourceReady[index].find(FG_ResourceType::Depth);
    if (itDepth == _resourceReady[index].end() || !itDepth->second)
        return false;

    auto itVelocity = _resourceReady[index].find(FG_ResourceType::Velocity);
    if (itVelocity == _resourceReady[index].end() || !itVelocity->second)
        return false;

    return true;
}

int IFGFeature::ResolveDispatchSlot(UINT64& willDispatchFrame)
{
    // Same frame already dispatched
    if (_frameCount == _actuallyDispatchedFrame && _frameCount != 0)
    {
        willDispatchFrame = _actuallyDispatchedFrame;
        return -1;
    }

    // 1. Check if _lastDispatchedFrame is an undispatched frame with ready resources
    // (e.g. unpause or activation where _lastDispatchedFrame was set by StartNewFrame before present)
    if (_lastDispatchedFrame > _actuallyDispatchedFrame)
    {
        int slot0 = static_cast<int>(_lastDispatchedFrame % BUFFER_COUNT);
        if (IsSlotReady(slot0))
        {
            willDispatchFrame = _lastDispatchedFrame;
            return slot0;
        }
    }

    // 2. Normal sequential candidate: _lastDispatchedFrame + 1
    UINT64 cand1 = _lastDispatchedFrame + 1;
    int slot1 = static_cast<int>(cand1 % BUFFER_COUNT);
    if (IsSlotReady(slot1))
    {
        willDispatchFrame = cand1;
        return slot1;
    }

    // 3. If candidate 1 is not ready, check if latest frame (_frameCount) has ready resources
    if (_frameCount > cand1)
    {
        int slotCur = static_cast<int>(_frameCount % BUFFER_COUNT);
        if (IsSlotReady(slotCur))
        {
            willDispatchFrame = _frameCount;
            return slotCur;
        }

        // Check intermediate candidate (_frameCount - 1)
        UINT64 candPrev = _frameCount - 1;
        if (candPrev > _lastDispatchedFrame)
        {
            int slotPrev = static_cast<int>(candPrev % BUFFER_COUNT);
            if (IsSlotReady(slotPrev))
            {
                willDispatchFrame = candPrev;
                return slotPrev;
            }
        }
    }

    // 4. Initial state fallback when _lastDispatchedFrame == 0: check any slot in ring buffer that has ready resources
    if (_lastDispatchedFrame == 0 && _frameCount > 0)
    {
        for (int i = 0; i < BUFFER_COUNT; i++)
        {
            if (IsSlotReady(i))
            {
                willDispatchFrame = _frameCount;
                return i;
            }
        }
    }

    // No slot ready for generation
    willDispatchFrame = 0;
    return -1;
}

int IFGFeature::GetDispatchIndex(UINT64& willDispatchFrame)
{
    LOG_DEBUG("_lastDispatchedFrame: {}, _actuallyDispatchedFrame: {}, _frameCount: {}", _lastDispatchedFrame,
              _actuallyDispatchedFrame, _frameCount);

    int slot = ResolveDispatchSlot(willDispatchFrame);
    if (slot >= 0)
    {
        _lastDispatchedFrame = willDispatchFrame;
        _lastFGFrame = State::Instance().fgLastFrame;
    }

    return slot;
}

bool IFGFeature::IsActive() { return _isActive || _waitingNewFrameData; }

bool IFGFeature::IsPaused() { return _targetFrame != 0 && _targetFrame >= _frameCount; }

bool IFGFeature::IsDispatched() { return _lastDispatchedFrame == _frameCount; }

bool IFGFeature::IsLowResMV() { return !_constants.flags[FG_Flags::DisplayResolutionMVs]; }

bool IFGFeature::IsAsync() { return _constants.flags[FG_Flags::Async]; }

bool IFGFeature::IsHdr() { return _constants.flags[FG_Flags::Hdr]; }

bool IFGFeature::IsJitteredMVs() { return _constants.flags[FG_Flags::JitteredMVs]; }

bool IFGFeature::IsInvertedDepth() { return _constants.flags[FG_Flags::InvertedDepth]; }

bool IFGFeature::IsInfiniteDepth() { return _constants.flags[FG_Flags::InfiniteDepth]; }

void IFGFeature::SetFrameCount(UINT64 frameId)
{
    if (frameId > _frameCount)
    {
        _frameCount = frameId;
    }
    else if (_frameCount - frameId > 4)
    {
        LOG_DEBUG("Frame counter rewind detected. Old: {}, New: {}", _frameCount, frameId);
        _frameCount = frameId;
        _lastDispatchedFrame = (frameId > 0 ? frameId - 1 : 0);
        _actuallyDispatchedFrame = (frameId > 0 ? frameId - 1 : 0);
    }
    else
    {
        LOG_TRACE("Preserving pipelined frameCount {}. Incoming marker: {}", _frameCount, frameId);
    }
}

void IFGFeature::SetJitter(float x, float y, int index)
{
    if (index < 0)
        index = GetIndex();

    _jitterX[index] = x;
    _jitterY[index] = y;
}

void IFGFeature::SetMVScale(float x, float y, int index)
{
    if (index < 0)
        index = GetIndex();

    _mvScaleX[index] = x;
    _mvScaleY[index] = y;
}

void IFGFeature::SetCameraValues(float nearValue, float farValue, float vFov, float aspectRatio, float meterFactor,
                                 int index)
{
    if (index < 0)
        index = GetIndex();

    _cameraFar[index] = farValue;
    _cameraNear[index] = nearValue;
    _cameraVFov[index] = vFov;
    _cameraAspectRatio[index] = aspectRatio;
    _meterFactor[index] = meterFactor;
}

void IFGFeature::SetCameraData(float cameraPosition[3], float cameraUp[3], float cameraRight[3], float cameraForward[3],
                               int index)
{
    if (index < 0)
        index = GetIndex();

    std::memcpy(_cameraPosition[index], cameraPosition, 3 * sizeof(float));
    std::memcpy(_cameraUp[index], cameraUp, 3 * sizeof(float));
    std::memcpy(_cameraRight[index], cameraRight, 3 * sizeof(float));
    std::memcpy(_cameraForward[index], cameraForward, 3 * sizeof(float));
}

void IFGFeature::SetFrameTimeDelta(double delta, int index)
{
    if (index < 0)
        index = GetIndex();

    _ftDelta[index] = delta;
}

void IFGFeature::SetReset(UINT reset, int index)
{
    if (index < 0)
        index = GetIndex();

    _reset[index] = reset;
}

void IFGFeature::SetInterpolationRect(UINT64 width, UINT height, int index)
{
    if (index < 0)
        index = GetIndex();

    _interpolationWidth[index] = width;
    _interpolationHeight[index] = height;
}

void IFGFeature::GetInterpolationRect(UINT64& width, UINT& height, int index)
{
    if (index < 0)
        index = GetIndex();

    width = _interpolationWidth[index];
    height = _interpolationHeight[index];
}

void IFGFeature::SetInterpolationPos(UINT left, UINT top, int index)
{
    if (index < 0)
        index = GetIndex();

    _interpolationLeft[index] = left;
    _interpolationTop[index] = top;
}

void IFGFeature::GetInterpolationPos(UINT& left, UINT& top, int index)
{
    if (index < 0)
        index = GetIndex();

    if (_interpolationLeft[index].has_value())
        left = _interpolationLeft[index].value();
    else
        left = 0;

    if (_interpolationTop[index].has_value())
        top = _interpolationTop[index].value();
    else
        top = 0;
}

void IFGFeature::ResetCounters()
{
    _targetFrame = _frameCount;
    _lastDispatchedFrame = 0;
    _actuallyDispatchedFrame = 0;
}

void IFGFeature::ConfirmDispatched(UINT64 frameId)
{
    _actuallyDispatchedFrame = frameId;
    _lastDispatchedFrame = frameId;
}

void IFGFeature::UpdateTarget()
{
    _targetFrame = _frameCount + 10;
    //_lastDispatchedFrame = 0;
    LOG_DEBUG("Current frame: {} target frame: {}", _frameCount, _targetFrame);
}

UINT64 IFGFeature::FrameCount() { return _frameCount; }

UINT64 IFGFeature::LastDispatchedFrame() { return _lastDispatchedFrame; }

UINT64 IFGFeature::TargetFrame() { return _targetFrame; }

void IFGFeature::SetResourceReady(FG_ResourceType type, int index)
{
    if (index < 0)
        index = GetIndex();

    _resourceReady[index][type] = true;
    _resourceFrame[type] = _frameCount;
}

UINT IFGFeature::GetInterpolatedFrameCount() const { return _framesToInterpolate < 0 ? 1 : _framesToInterpolate; }

int IFGFeature::GetMaxInterpolationCount() const { return _maxInterpolationCount; }

bool IFGFeature::GetDMFGSupport() const { return _supportsDMFG; }

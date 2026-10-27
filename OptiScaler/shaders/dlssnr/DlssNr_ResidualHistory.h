#pragma once
#include "DlssNr_Common.h"
#include <cmath>

// History bookkeeping for the RR residual accumulate pass (dlssnr_residual.hlsl mode 0).
// Kept free of D3D12 so the unit test exercises the same rules as DlssNr_Dx12_DeferredSr.cpp.
namespace DlssNr::ResidualHistory
{
struct State
{
    bool rejection = false;   // HistoryRejection the stored history was written with
    float preExposure = 1.0f; // pre-exposure of the frame that wrote it
};

// Pre-exposure the shader can divide by; games that omit it get 1.
inline float SafePreExposure(float value) { return std::isfinite(value) && value > 1e-6f ? value : 1.0f; }

// Fill the history fields of an accumulate dispatch. Toggling rejection drops the history: with it on,
// the shader stores input light in alpha, which the other mode neither writes nor understands.
inline void Fill(DlssNrConstants& c, const State& history, bool rejection, float preExposure, bool accumulationValid,
                 bool reset)
{
    const float current = SafePreExposure(preExposure);
    c.HistoryRejection = rejection ? 1u : 0u;
    c.ResidualScale = current;
    // The old signed edit is in the previous frame's pre-exposed units.
    c.PreExposure = current / SafePreExposure(history.preExposure);
    c.ResidualHistoryValid = accumulationValid && !reset && history.rejection == rejection;
}

// Call after the dispatch so the next frame compares against what this one wrote.
inline void Record(State& history, const DlssNrConstants& c)
{
    history.rejection = c.HistoryRejection != 0;
    history.preExposure = c.ResidualScale;
}
} // namespace DlssNr::ResidualHistory

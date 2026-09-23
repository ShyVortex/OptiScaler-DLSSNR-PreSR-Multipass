#pragma once
#include <shaders/dlssnr/DlssNr_Common.h>
#include <array>
#include <cmath>
#include <sstream>
#include <string>

namespace DlssNr
{
// Both backends consume the same bounded, sorted curve. Invalid entries are ignored.
template <typename ConfigType>
void ExposureConstants(DlssNrConstants& c, const ConfigType& config, unsigned source, float preExposure)
{
    c.ExposureMode = source;
    c.PreExposure = std::isfinite(preExposure) && preExposure > 1e-6f ? preExposure : 1.0f;
    const float trim =
        source == 3 ? config.DlssNrAutoExposureTrim.value_or_default() : config.DlssNrWhitePointTrim.value_or_default();
    c.ExposureTrim = std::isfinite(trim) ? std::clamp(trim, 0.001f, 1000.0f) : 1.0f;
    const float protection = config.DlssNrAutoExposureHighlightProtection.value_or_default();
    c.ExposureProtection = std::isfinite(protection) ? std::clamp(protection, 0.0f, 100.0f) : 0.0f;
    std::istringstream text(source == 3 ? config.DlssNrAutoExposureTrimAnchors.value_or_default()
                                        : config.DlssNrExposureTrimAnchors.value_or_default());
    std::array<std::pair<float, float>, 8> anchors {};
    unsigned count = 0;
    float point, value;
    char colon;
    while (count < anchors.size() && text >> point >> colon >> value)
        if (colon == ':' && std::isfinite(point) && point > 0 && std::isfinite(value) && value > 0)
            anchors[count++] = { point, std::clamp(value, 0.001f, 1000.0f) };
    std::stable_sort(anchors.begin(), anchors.begin() + count);
    c.ExposureAnchorCount = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        if (i + 1 < count && anchors[i].first == anchors[i + 1].first)
            continue;
        c.ExposureAnchors[2 * c.ExposureAnchorCount] = anchors[i].first;
        c.ExposureAnchors[2 * c.ExposureAnchorCount++ + 1] = anchors[i].second;
    }
}
} // namespace DlssNr

#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace OptiScaler
{
class RollingVitals
{
    std::array<double, 256> total {}, model {};
    size_t count = 0, next = 0;

  public:
    struct Summary
    {
        size_t samples;
        double totalMean, totalP99, modelMean, modelP99;
    };
    void Push(double elapsed, double neural)
    {
        if (!std::isfinite(elapsed) || !std::isfinite(neural) || elapsed < 0 || neural < 0)
            return;
        total[next] = elapsed;
        model[next] = neural;
        next = (next + 1) % total.size();
        count = std::min(count + 1, total.size());
    }
    Summary Read() const
    {
        if (!count)
            return {};
        auto a = total, b = model;
        std::sort(a.begin(), a.begin() + count);
        std::sort(b.begin(), b.begin() + count);
        const auto percentile = static_cast<size_t>(std::ceil(count * 0.99)) - 1;
        return { count, std::accumulate(a.begin(), a.begin() + count, 0.0) / count, a[percentile],
                 std::accumulate(b.begin(), b.begin() + count, 0.0) / count, b[percentile] };
    }
};
} // namespace OptiScaler

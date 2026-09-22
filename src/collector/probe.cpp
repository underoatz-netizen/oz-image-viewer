#include "oz/collector/probe.hpp"

#include <algorithm>
#include <cmath>

namespace oz::collector {

namespace {

// BT.601 luma from a BGRA pixel (B, G, R, A order).
inline double luma_of(const std::uint8_t* p) {
    return (0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2]) / 255.0;
}

}  // namespace

probe_metrics evaluate_probe(const std::uint8_t* bgra, int width, int height,
                             int stride, const probe_config& cfg) {
    probe_metrics m;
    if (!bgra || width <= 0 || height <= 0 || stride < width * 4) {
        return m;
    }
    const int step = std::max(1, std::min(width, height) / 64);
    double sum = 0;
    double sum_sq = 0;
    std::uint64_t n = 0;
    std::uint64_t edges = 0;
    for (int y = 0; y < height; y += step) {
        const std::uint8_t* row = bgra + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < width; x += step) {
            const std::uint8_t* p = row + static_cast<std::size_t>(x) * 4;
            const double l = luma_of(p);
            sum += l;
            sum_sq += l * l;
            ++n;
            if (x + step < width) {
                const double lq = luma_of(p + static_cast<std::size_t>(step) * 4);
                if (std::fabs(l - lq) > 0.08) {
                    ++edges;
                }
            }
            if (y + step < height) {
                // Note: offset from the buffer base, not from `row` (which
                // already includes y*stride).
                const double lq =
                    luma_of(bgra + static_cast<std::size_t>(y + step) * stride +
                            static_cast<std::size_t>(x) * 4);
                if (std::fabs(l - lq) > 0.08) {
                    ++edges;
                }
            }
        }
    }
    if (n == 0) {
        return m;
    }
    const double mean = sum / static_cast<double>(n);
    const double var = sum_sq / static_cast<double>(n) - mean * mean;
    m.mean_luma = mean;
    m.luma_variance = var < 0 ? 0 : var;
    m.edge_density = edges / static_cast<double>(2 * n);
    m.matched = m.edge_density >= cfg.min_edge_density &&
                m.luma_variance >= cfg.min_luma_variance &&
                mean >= cfg.min_mean_luma && mean <= cfg.max_mean_luma;
    return m;
}

consecutive_matcher::consecutive_matcher(int required)
    : required_(required > 0 ? required : 1) {}

bool consecutive_matcher::observe(bool frame_matched) {
    started_run_ = false;
    if (frame_matched) {
        if (run_ == 0) {
            started_run_ = true;  // first matching frame of a new run
        }
        ++run_;
    } else {
        run_ = 0;  // run broken
    }
    return run_ >= required_;
}

}  // namespace oz::collector
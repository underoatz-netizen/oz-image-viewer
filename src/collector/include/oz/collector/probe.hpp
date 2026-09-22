// oz/collector/probe.hpp - fixed client-area image probe for the
// external_visible_frame collector.
//
// The probe is deliberately simple and deterministic: it samples a BGRA
// region on a fixed grid and computes normalised luma statistics (mean,
// variance) and an edge density. A frame "matches" when the statistics clear
// the sealed thresholds. Because the statistics are normalised and the grid
// is fixed, the probe is robust to the viewer's fit-to-window resampling.
//
// Thresholds are sealed by the excluded visual pilot validation (see
// docs/reference-benchmark.md); they are never tuned per measurement.
#pragma once

#include <cstddef>
#include <cstdint>

namespace oz::collector {

struct probe_config {
    double min_edge_density = 0.02;    // fraction of sampled transitions that are edges
    double min_luma_variance = 0.001;  // normalised luma variance (0..1 scale)
    double min_mean_luma = 0.02;       // normalised mean luma lower bound
    double max_mean_luma = 0.98;       // normalised mean luma upper bound
    int consecutive_frames = 2;        // exactly two consecutive matching frames
};

struct probe_metrics {
    double mean_luma = 0;
    double luma_variance = 0;
    double edge_density = 0;
    bool matched = false;
};

// Evaluates the probe over a BGRA (32-bit) buffer. The region is sampled on a
// fixed grid (step = max(1, min(w,h)/64)) so the statistics are robust to
// resampling. `stride` is the row pitch in bytes and must be >= width*4.
probe_metrics evaluate_probe(const std::uint8_t* bgra, int width, int height,
                             int stride, const probe_config& cfg);

// Consecutive-frame rule: observe() returns true exactly when the last
// `required` observations were all matches. The ExternalFirstImageVisible
// endpoint is the *first* matching frame of a run (started_new_run()); the
// run-completing frame only confirms it and is never the endpoint.
class consecutive_matcher {
public:
    explicit consecutive_matcher(int required);

    // Returns true when the current consecutive run reaches `required`.
    bool observe(bool frame_matched);

    // True when the most recent observe() began a new matching run (the
    // observed frame is the first matching frame of the run — the detection
    // point). False on a run break or a continuation of an existing run.
    bool started_new_run() const noexcept { return started_run_; }

    // Length of the current consecutive matching run.
    int matched_frames() const noexcept { return run_; }

private:
    int required_;
    int run_ = 0;
    bool started_run_ = false;
};

}  // namespace oz::collector
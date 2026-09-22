// oz/runner/plan.hpp - benchmark plan model (schema oz.plan.v1).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oz::runner {

inline constexpr std::string_view kPlanSchema = "oz.plan.v1";

// Bounds applied when parsing plans and CLI overrides. Negative values are
// rejected before narrowing; these caps prevent pathological run loops.
inline constexpr std::int64_t kMaxTimeoutMs = 24 * 60 * 60 * 1000;  // 24 h
inline constexpr std::uint32_t kMaxRepetitions = 1'000'000;
inline constexpr std::uint32_t kMaxWarmups = 1'000'000;

// Fixed client-area probe thresholds for the external_visible_frame collector.
// Sealed by the excluded visual pilot validation (docs/reference-benchmark.md).
struct probe_config {
    double min_edge_density = 0.02;
    double min_luma_variance = 0.001;
    double min_mean_luma = 0.02;
    double max_mean_luma = 0.98;
    int consecutive_frames = 2;
};

// Collector configuration for external_visible_frame.
struct collector_config {
    int monitor_index = -1;  // -1 = use the subject window's monitor
    std::uint32_t window_wait_ms = 15000;
    std::uint32_t frame_timeout_ms = 30000;
    probe_config probe;
};

struct image_spec {
    std::string path;
    std::string alias;   // public alias used in results (redaction)
    std::string sha256;  // lowercase hex SHA-256 of the image file
    std::string format;
    std::optional<std::int64_t> width;
    std::optional<std::int64_t> height;
    std::optional<std::uint64_t> size_bytes;
    std::string provenance;
};

struct subject_spec {
    std::string name;
    std::string executable;  // may contain {{placeholders}}
    std::optional<std::string> version_hint;
    bool enabled = true;
    std::string skip_reason;
};

struct plan {
    std::string schema;
    std::string name;
    subject_spec subject;
    std::string collector = "markers";  // "markers" | "external_first_present" | "external_visible_frame"
    std::string adapter = "oz_markers";
    std::string endpoint;  // "ExternalFirstImageVisible" for external_visible_frame
    std::string cache = "cold";  // "cold" | "warm" - protocol label only
    std::optional<std::string> working_directory;
    std::optional<std::string> manifest;  // sealed private manifest path
    std::optional<collector_config> collector_config;
    std::uint32_t timeout_ms = 30000;
    std::uint32_t repetitions = 5;
    std::uint32_t warmups = 0;
    std::vector<std::string> args;  // may contain {{image_path}} / {{key}}
    std::vector<image_spec> images;
};

struct plan_parse_result {
    std::optional<plan> value;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

plan_parse_result parse_plan(std::string_view text);

// Scenario identity: subject|cache|image path (identical scenario = same key).
std::string scenario_key(const plan& p, const image_spec& img);

// True when the subject executable exists and is not a directory.
bool subject_available(const plan& p);

}  // namespace oz::runner
// oz/runner/record.hpp - canonical JSONL record model (schema oz.benchmark.v1).
#pragma once

#include "oz/markers/markers.hpp"
#include "oz/runner/json.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oz::runner {

inline constexpr std::string_view kSchema = "oz.benchmark.v1";

enum class record_kind : std::uint8_t {
    run_start,
    marker,
    run_end,
    summary,
    validation,
    import,
    external_visible,
};

struct image_info {
    std::string path;
    std::string alias;   // public alias (redaction)
    std::string sha256;  // lowercase hex SHA-256 of the image file
    std::string format;
    std::optional<std::int64_t> width;
    std::optional<std::int64_t> height;
    std::optional<std::uint64_t> size_bytes;
    std::string provenance;
};

struct record {
    record_kind kind{};
    std::string scenario;      // subject|cache|image alias (see docs/benchmark.md)
    std::string cache;         // "cold" | "warm" - protocol label only
    std::int64_t repetition = 0;
    bool warmup = false;
    std::string subject;
    std::string subject_path;  // resolved executable path (placeholders substituted)
    std::optional<std::string> subject_version;  // plan version_hint (human-provided, not auto-detected)
    std::string subject_sha256;  // lowercase hex SHA-256 of the resolved executable; empty when unreadable
    std::string collector;
    std::string adapter;

    // Capture linkage + provenance (external_visible_frame).
    std::string manifest_id;  // sealed private manifest id
    std::string run_id;       // unique id per capture invocation
    std::string endpoint;     // "ExternalFirstImageVisible" for external_visible_frame
    bool redacted = false;    // omit subject_path and image.path (private)
    std::string image_alias;  // public alias when redacted
    std::uint32_t pid = 0;
    std::uint64_t hwnd = 0;

    // marker / summary / import
    std::string marker;
    std::uint64_t raw_ticks = 0;
    std::int64_t relative_ns = 0;
    std::uint32_t thread_id = 0;

    // image
    std::optional<image_info> image;

    // run_end
    bool launched = true;  // false when CreateProcessW failed
    std::uint32_t error = 0;  // GetLastError() when !launched
    std::string error_message;  // short description when !launched
    std::optional<std::uint32_t> exit_code;
    std::optional<std::uint64_t> duration_ns;
    bool timed_out = false;
    std::vector<markers::validation_failure> validation;  // library kinds
    std::vector<std::string> issues;                      // runner-level issues

    // external_visible / capture status
    std::string status;   // "matched" | explicit non-success reason
    std::string reason;   // human-readable non-success reason
    std::uint64_t launch_ticks = 0;   // QPC immediately before CreateProcessW
    std::uint64_t detect_ticks = 0;   // QPC at the first matching frame (endpoint)
    std::uint64_t confirm_ticks = 0;  // QPC at the run-completing frame (provenance)
    int monitor_index = 0;
    std::string monitor_name;
    int monitor_width = 0;
    int monitor_height = 0;
    int monitor_refresh_rate = 0;
    double probe_mean_luma = 0;
    double probe_luma_variance = 0;
    double probe_edge_density = 0;
    double probe_threshold_edge_density = 0;
    double probe_threshold_luma_variance = 0;
    double probe_threshold_min_mean_luma = 0;
    double probe_threshold_max_mean_luma = 0;
    int probe_consecutive_frames = 0;
    int probe_matched_frames = 0;
    std::string cleanup_outcome;  // "exited" | "forced" | "failed" | "not_applicable"
    bool cleanup_forced = false;
    std::uint32_t cleanup_wait_ms = 0;

    // summary
    std::string metric;
    std::optional<std::int64_t> value_ns;
    std::optional<std::size_t> count;
    std::string convention;

    // validation
    std::string plan_path;
    bool valid = true;
    bool subject_available = true;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    // import
    std::string source;
    std::string imported_utc;

    // run_start
    std::string started_utc;
};

std::string_view kind_name(record_kind k) noexcept;
std::optional<record_kind> kind_from_name(std::string_view name) noexcept;
std::string_view validation_kind_name(markers::validation_kind k) noexcept;

// Serialize a record to one canonical JSONL line (UTF-8, trailing newline).
// Redacted records (r.redacted) omit subject_path and image.path so private
// source paths never appear in public results.
std::string to_jsonl(const record& r);

// Parse a canonical record from a JSON value. Returns nullopt for malformed
// or unknown-kind lines (summarize skips those).
std::optional<record> from_json(const json::value& v);

// Current UTC time as "YYYY-MM-DDTHH:MM:SSZ".
std::string utc_now_iso8601();

}  // namespace oz::runner
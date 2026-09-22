#pragma once

#include "oz/runner/json.hpp"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oz::viewer_core {

// Canonical schema for the navigation-benchmark JSONL stream.
inline constexpr std::string_view kNavigationBenchmarkSchema = "oz.navigation-benchmark.v1";

// The eight record types emitted in one navigation-benchmark run.
enum class bench_record_kind : std::uint8_t {
    run_start,
    corpus,
    navigation_request,
    task_event,
    successful_present,
    memory_sample,
    scenario_summary,
    run_end
};

std::string_view to_string(bench_record_kind k) noexcept;
std::optional<bench_record_kind> bench_record_kind_from_name(std::string_view name) noexcept;

// Result of validating a navigation-benchmark JSONL stream.
struct bench_jsonl_validation {
    bool ok = false;
    std::string error;
    std::size_t records = 0;
    std::size_t redaction_violations = 0;
    std::vector<bench_record_kind> kinds;  // in stream order when parsing succeeds
};

// Validates an in-memory navigation-benchmark JSONL stream: every non-empty
// line must parse as an object carrying schema == oz.navigation-benchmark.v1
// and a known record_type, and no string value may contain private path data
// (drive letters, UNC prefix, Users directory). Pixels are never emitted by
// the serializer and a "pixels"/"image_data" member is rejected here.
bench_jsonl_validation validate_navigation_jsonl(std::string_view content);

// File overload; a missing/unreadable file is an explicit validation error.
bench_jsonl_validation validate_navigation_jsonl_file(const std::filesystem::path& path);

// Create-new output safety for the canonical JSONL. When `overwrite` is false
// (the default at call sites) an existing file is refused rather than truncated;
// create-new is requested from the OS. Returns an empty string on success or a
// human-readable error. `out` must not already be open.
std::string open_benchmark_output(std::ofstream& out,
                                  const std::filesystem::path& path,
                                  bool overwrite = false);

enum class bench_cache_state {
    hit_at_request,
    miss_at_request
};

inline const char* to_string(bench_cache_state s) noexcept {
    switch (s) {
        case bench_cache_state::hit_at_request: return "hit_at_request";
        case bench_cache_state::miss_at_request: return "miss_at_request";
    }
    return "unknown";
}

inline const char* os_cache_condition_str(const std::string& cond) noexcept {
    if (cond == "cold_protocol") return "cold_protocol";
    if (cond == "warm_protocol") return "warm_protocol";
    return "uncontrolled";
}

// Memory sample record
struct bench_memory_sample {
    std::uint64_t raw_ticks = 0;
    std::int64_t relative_ns = 0;
    std::size_t private_bytes = 0;
    std::size_t working_set_bytes = 0;
    std::size_t peak_working_set_bytes = 0;
    std::size_t cache_used_bytes = 0;
    std::size_t cache_budget_bytes = 0;
    std::size_t cache_entry_count = 0;
    std::size_t resident_decoded_count = 0;
};

// Summary metrics computed over accepted (non-superseded, non-failed) present latencies
struct bench_scenario_summary {
    std::string scenario;
    std::string os_cache_condition;
    std::size_t requested_count = 0;
    std::size_t successful_count = 0;
    std::size_t superseded_count = 0;
    std::size_t failed_count = 0;
    std::size_t timed_out_count = 0;
    std::size_t no_op_count = 0;
    std::int64_t min_ns = 0;
    std::int64_t max_ns = 0;
    std::int64_t median_ns = 0;
    std::int64_t p95_ns = 0;
    std::int64_t p99_ns = 0;
    std::string convention = "nearest-rank";
};

// Computes nearest-rank percentile
std::int64_t bench_nearest_rank_percentile(const std::vector<std::int64_t>& sorted, double p);

// Computes summary from latency vector (ns) and total counts
bench_scenario_summary compute_scenario_summary(
    const std::string& scenario,
    const std::string& os_cache_condition,
    std::size_t requested,
    std::size_t superseded,
    std::size_t failed,
    std::size_t timed_out,
    std::size_t no_op,
    std::vector<std::int64_t> accepted_latencies_ns);

// Query current process memory safely via GetProcessMemoryInfo
bool query_process_memory(std::size_t& private_bytes, std::size_t& working_set, std::size_t& peak_working_set);

// Serializers to canonical json::value
json::value serialize_run_start(
    const std::string& run_id,
    const std::string& corpus_manifest_hash,
    std::size_t corpus_image_count,
    std::int32_t viewport_w,
    std::int32_t viewport_h,
    float dpi,
    bool warp);

json::value serialize_corpus_entry(
    const std::string& alias,
    std::size_t ordinal,
    const std::string& sha256,
    std::int32_t width,
    std::int32_t height,
    std::uint64_t file_bytes);

json::value serialize_navigation_request(
    const std::string& run_id,
    const std::string& scenario,
    const std::string& os_cache_condition,
    std::uint64_t request_id,
    std::uint64_t epoch,
    const std::string& direction,
    std::size_t requested_target_index,
    const std::string& requested_target_alias,
    std::size_t from_index,
    const std::string& from_alias,
    bench_cache_state app_cache_state,
    std::uint64_t request_ticks,
    std::int64_t relative_ns);

json::value serialize_successful_present(
    const std::string& run_id,
    const std::string& scenario,
    const std::string& os_cache_condition,
    std::uint64_t request_id,
    std::uint64_t epoch,
    std::size_t committed_index,
    const std::string& committed_alias,
    const std::string& committed_sha256,
    bench_cache_state app_cache_state,
    std::uint64_t request_ticks,
    std::uint64_t present_ticks,
    std::int64_t latency_ns,
    std::size_t cache_budget_bytes,
    std::size_t cache_used_bytes,
    std::size_t cache_entry_count,
    std::size_t resident_decoded_count);

json::value serialize_task_event(
    const std::string& run_id,
    const std::string& scenario,
    std::uint64_t request_id,
    std::uint64_t epoch,
    const std::string& event_type, // "superseded" | "failed" | "timed_out" | "no_op" |
                                   // "unmatched_present" | "duplicate_present" | "step"
    const std::string& status,
    const std::string& reason,
    std::int64_t relative_ns);

json::value serialize_memory_sample(
    const std::string& run_id,
    const std::string& phase,
    const bench_memory_sample& sample);

json::value serialize_scenario_summary(
    const std::string& run_id,
    const bench_scenario_summary& summary);

json::value serialize_run_end(
    const std::string& run_id,
    const std::string& status,
    const std::string& error_reason,
    std::uint64_t duration_ticks,
    std::int64_t duration_ns,
    std::size_t total_requests,
    std::size_t total_presents,
    std::size_t total_superseded,
    std::size_t total_failed,
    std::size_t total_timed_out,
    std::size_t total_no_ops);

// Redaction check: ensure no absolute paths or Windows drive letters in public string
bool contains_private_path_data(const std::string& str);

}  // namespace oz::viewer_core

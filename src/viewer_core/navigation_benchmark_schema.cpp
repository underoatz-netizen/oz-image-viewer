#include "oz/viewer_core/navigation_benchmark_schema.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <system_error>

#pragma comment(lib, "psapi.lib")

namespace oz::viewer_core {

std::string_view to_string(bench_record_kind k) noexcept {
    switch (k) {
        case bench_record_kind::run_start: return "run_start";
        case bench_record_kind::corpus: return "corpus";
        case bench_record_kind::navigation_request: return "navigation_request";
        case bench_record_kind::task_event: return "task_event";
        case bench_record_kind::successful_present: return "successful_present";
        case bench_record_kind::memory_sample: return "memory_sample";
        case bench_record_kind::scenario_summary: return "scenario_summary";
        case bench_record_kind::run_end: return "run_end";
    }
    return "unknown";
}

std::optional<bench_record_kind> bench_record_kind_from_name(std::string_view name) noexcept {
    if (name == "run_start") return bench_record_kind::run_start;
    if (name == "corpus") return bench_record_kind::corpus;
    if (name == "navigation_request") return bench_record_kind::navigation_request;
    if (name == "task_event") return bench_record_kind::task_event;
    if (name == "successful_present") return bench_record_kind::successful_present;
    if (name == "memory_sample") return bench_record_kind::memory_sample;
    if (name == "scenario_summary") return bench_record_kind::scenario_summary;
    if (name == "run_end") return bench_record_kind::run_end;
    return std::nullopt;
}

namespace {

// Recursively rejects private path data and raw pixel payloads anywhere in a
// record. Returns true when a violation was found.
bool scan_for_private_data(const json::value& v, std::size_t& violations) {
    if (json::is_string(v)) {
        if (contains_private_path_data(json::as_string(v))) {
            ++violations;
            return true;
        }
        return false;
    }
    if (json::is_array(v)) {
        bool found = false;
        const std::size_t n = json::array_size(v);
        for (std::size_t i = 0; i < n; ++i) {
            found = scan_for_private_data(json::array_at(v, i), violations) || found;
        }
        return found;
    }
    if (json::is_object(v)) {
        bool found = false;
        for (const auto& [key, member] : json::as_object(v)) {
            if (key == "pixels" || key == "pixel_data" || key == "image_data" ||
                key == "raw_pixels") {
                ++violations;
                found = true;
                continue;
            }
            found = scan_for_private_data(member, violations) || found;
        }
        return found;
    }
    return false;
}

void trim_trailing_whitespace(std::string_view& line) {
    while (!line.empty()) {
        const char c = line.back();
        if (c == '\r' || c == ' ' || c == '\t') {
            line.remove_suffix(1);
        } else {
            break;
        }
    }
}

}  // namespace

bench_jsonl_validation validate_navigation_jsonl(std::string_view content) {
    bench_jsonl_validation res;
    std::size_t line_no = 0;
    std::size_t pos = 0;
    const std::size_t npos = std::string_view::npos;

    while (pos <= content.size()) {
        const std::size_t nl = content.find('\n', pos);
        std::string_view line =
            (nl == npos) ? content.substr(pos) : content.substr(pos, nl - pos);
        pos = (nl == npos) ? content.size() + 1 : nl + 1;
        trim_trailing_whitespace(line);
        if (line.empty()) {
            continue;
        }
        ++line_no;

        auto parsed = json::parse(line);
        if (!parsed) {
            res.error = "line " + std::to_string(line_no) + ": malformed JSON";
            res.ok = false;
            return res;
        }
        const json::value& v = *parsed;
        if (!json::is_object(v)) {
            res.error = "line " + std::to_string(line_no) + ": record is not an object";
            return res;
        }

        const auto schema = json::get_string(v, "schema");
        if (!schema || std::string_view(*schema) != kNavigationBenchmarkSchema) {
            res.error = "line " + std::to_string(line_no) +
                        ": schema is not " + std::string(kNavigationBenchmarkSchema);
            return res;
        }
        const auto record_type = json::get_string(v, "record_type");
        if (!record_type) {
            res.error = "line " + std::to_string(line_no) + ": missing record_type";
            return res;
        }
        const auto kind = bench_record_kind_from_name(*record_type);
        if (!kind) {
            res.error = "line " + std::to_string(line_no) +
                        ": unknown record_type '" + *record_type + "'";
            return res;
        }

        const std::size_t before = res.redaction_violations;
        scan_for_private_data(v, res.redaction_violations);
        if (res.redaction_violations > before) {
            res.error = "line " + std::to_string(line_no) +
                        ": private path or pixel data present";
            return res;
        }

        res.kinds.push_back(*kind);
        ++res.records;
    }

    if (res.records == 0) {
        res.error = "no records found";
        return res;
    }
    res.ok = true;
    return res;
}

bench_jsonl_validation validate_navigation_jsonl_file(const std::filesystem::path& path) {
    bench_jsonl_validation res;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        res.error = "cannot open JSONL file";
        return res;
    }
    const std::string content((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    return validate_navigation_jsonl(content);
}

std::string open_benchmark_output(std::ofstream& out,
                                  const std::filesystem::path& path,
                                  bool overwrite) {
    if (out.is_open()) {
        return "output stream is already open";
    }
    std::error_code ec;
    if (!overwrite) {
        if (std::filesystem::exists(path, ec)) {
            return "output file already exists: " + path.filename().string();
        }
        out.open(path, std::ios::out | std::ios::binary | std::ios::noreplace);
    } else {
        out.open(path, std::ios::out | std::ios::binary | std::ios::trunc);
    }
    if (!out.is_open()) {
        return "cannot open output file: " + path.filename().string();
    }
    return {};
}

std::int64_t bench_nearest_rank_percentile(const std::vector<std::int64_t>& sorted, double p) {
    if (sorted.empty()) {
        return 0;
    }
    std::size_t rank = static_cast<std::size_t>(
        std::ceil(p / 100.0 * static_cast<double>(sorted.size())));
    if (rank < 1) {
        rank = 1;
    }
    if (rank > sorted.size()) {
        rank = sorted.size();
    }
    return sorted[rank - 1];
}

bench_scenario_summary compute_scenario_summary(
    const std::string& scenario,
    const std::string& os_cache_condition,
    std::size_t requested,
    std::size_t superseded,
    std::size_t failed,
    std::size_t timed_out,
    std::size_t no_op,
    std::vector<std::int64_t> accepted_latencies_ns) {
    bench_scenario_summary sum;
    sum.scenario = scenario;
    sum.os_cache_condition = os_cache_condition;
    sum.requested_count = requested;
    sum.successful_count = accepted_latencies_ns.size();
    sum.superseded_count = superseded;
    sum.failed_count = failed;
    sum.timed_out_count = timed_out;
    sum.no_op_count = no_op;
    sum.convention = "nearest-rank";

    if (!accepted_latencies_ns.empty()) {
        std::sort(accepted_latencies_ns.begin(), accepted_latencies_ns.end());
        sum.min_ns = accepted_latencies_ns.front();
        sum.max_ns = accepted_latencies_ns.back();
        sum.median_ns = bench_nearest_rank_percentile(accepted_latencies_ns, 50.0);
        sum.p95_ns = bench_nearest_rank_percentile(accepted_latencies_ns, 95.0);
        sum.p99_ns = bench_nearest_rank_percentile(accepted_latencies_ns, 99.0);
    }
    return sum;
}

bool query_process_memory(std::size_t& private_bytes, std::size_t& working_set, std::size_t& peak_working_set) {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        private_bytes = pmc.PrivateUsage;
        working_set = pmc.WorkingSetSize;
        peak_working_set = pmc.PeakWorkingSetSize;
        return true;
    }
    private_bytes = 0;
    working_set = 0;
    peak_working_set = 0;
    return false;
}

bool contains_private_path_data(const std::string& str) {
    // Check for drive letter e.g. "C:\" or "E:\" or backslash path or user directories
    if (str.size() >= 3 && str[1] == ':' && (str[2] == '\\' || str[2] == '/')) {
        return true;
    }
    if (str.find("Users\\") != std::string::npos || str.find("Users/") != std::string::npos) {
        return true;
    }
    if (str.find("\\\\") != std::string::npos) { // UNC path
        return true;
    }
    return false;
}

json::value serialize_run_start(
    const std::string& run_id,
    const std::string& corpus_manifest_hash,
    std::size_t corpus_image_count,
    std::int32_t viewport_w,
    std::int32_t viewport_h,
    float dpi,
    bool warp) {
    auto o = json::make_object();
    json::set_member(o, "schema", json::make_string("oz.navigation-benchmark.v1"));
    json::set_member(o, "record_type", json::make_string("run_start"));
    json::set_member(o, "run_id", json::make_string(run_id));
    json::set_member(o, "corpus_manifest_hash", json::make_string(corpus_manifest_hash));
    json::set_member(o, "corpus_image_count", json::make_int(static_cast<std::int64_t>(corpus_image_count)));
    json::set_member(o, "viewport_width", json::make_int(viewport_w));
    json::set_member(o, "viewport_height", json::make_int(viewport_h));
    json::set_member(o, "dpi", json::make_double(static_cast<double>(dpi)));
    json::set_member(o, "warp", json::make_bool(warp));
    return o;
}

json::value serialize_corpus_entry(
    const std::string& alias,
    std::size_t ordinal,
    const std::string& sha256,
    std::int32_t width,
    std::int32_t height,
    std::uint64_t file_bytes) {
    auto o = json::make_object();
    json::set_member(o, "schema", json::make_string("oz.navigation-benchmark.v1"));
    json::set_member(o, "record_type", json::make_string("corpus"));
    json::set_member(o, "alias", json::make_string(alias));
    json::set_member(o, "ordinal", json::make_int(static_cast<std::int64_t>(ordinal)));
    json::set_member(o, "sha256", json::make_string(sha256));
    json::set_member(o, "width", json::make_int(width));
    json::set_member(o, "height", json::make_int(height));
    json::set_member(o, "file_bytes", json::make_int(static_cast<std::int64_t>(file_bytes)));
    return o;
}

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
    std::int64_t relative_ns) {
    auto o = json::make_object();
    json::set_member(o, "schema", json::make_string("oz.navigation-benchmark.v1"));
    json::set_member(o, "record_type", json::make_string("navigation_request"));
    json::set_member(o, "run_id", json::make_string(run_id));
    json::set_member(o, "scenario", json::make_string(scenario));
    json::set_member(o, "os_cache_condition", json::make_string(os_cache_condition));
    json::set_member(o, "request_id", json::make_int(static_cast<std::int64_t>(request_id)));
    json::set_member(o, "epoch", json::make_int(static_cast<std::int64_t>(epoch)));
    json::set_member(o, "direction", json::make_string(direction));
    json::set_member(o, "requested_target_index", json::make_int(static_cast<std::int64_t>(requested_target_index)));
    json::set_member(o, "requested_target_alias", json::make_string(requested_target_alias));
    json::set_member(o, "from_index", json::make_int(static_cast<std::int64_t>(from_index)));
    json::set_member(o, "from_alias", json::make_string(from_alias));
    json::set_member(o, "app_cache_state", json::make_string(to_string(app_cache_state)));
    json::set_member(o, "request_ticks", json::make_int(static_cast<std::int64_t>(request_ticks)));
    json::set_member(o, "relative_ns", json::make_int(relative_ns));
    return o;
}

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
    std::size_t resident_decoded_count) {
    auto o = json::make_object();
    json::set_member(o, "schema", json::make_string("oz.navigation-benchmark.v1"));
    json::set_member(o, "record_type", json::make_string("successful_present"));
    json::set_member(o, "run_id", json::make_string(run_id));
    json::set_member(o, "scenario", json::make_string(scenario));
    json::set_member(o, "os_cache_condition", json::make_string(os_cache_condition));
    json::set_member(o, "request_id", json::make_int(static_cast<std::int64_t>(request_id)));
    json::set_member(o, "epoch", json::make_int(static_cast<std::int64_t>(epoch)));
    json::set_member(o, "committed_index", json::make_int(static_cast<std::int64_t>(committed_index)));
    json::set_member(o, "committed_alias", json::make_string(committed_alias));
    json::set_member(o, "committed_sha256", json::make_string(committed_sha256));
    json::set_member(o, "app_cache_state", json::make_string(to_string(app_cache_state)));
    json::set_member(o, "request_ticks", json::make_int(static_cast<std::int64_t>(request_ticks)));
    json::set_member(o, "present_ticks", json::make_int(static_cast<std::int64_t>(present_ticks)));
    json::set_member(o, "latency_ns", json::make_int(latency_ns));
    json::set_member(o, "cache_budget_bytes", json::make_int(static_cast<std::int64_t>(cache_budget_bytes)));
    json::set_member(o, "cache_used_bytes", json::make_int(static_cast<std::int64_t>(cache_used_bytes)));
    json::set_member(o, "cache_entry_count", json::make_int(static_cast<std::int64_t>(cache_entry_count)));
    json::set_member(o, "resident_decoded_count", json::make_int(static_cast<std::int64_t>(resident_decoded_count)));
    return o;
}

json::value serialize_task_event(
    const std::string& run_id,
    const std::string& scenario,
    std::uint64_t request_id,
    std::uint64_t epoch,
    const std::string& event_type,
    const std::string& status,
    const std::string& reason,
    std::int64_t relative_ns) {
    auto o = json::make_object();
    json::set_member(o, "schema", json::make_string("oz.navigation-benchmark.v1"));
    json::set_member(o, "record_type", json::make_string("task_event"));
    json::set_member(o, "run_id", json::make_string(run_id));
    json::set_member(o, "scenario", json::make_string(scenario));
    json::set_member(o, "request_id", json::make_int(static_cast<std::int64_t>(request_id)));
    json::set_member(o, "epoch", json::make_int(static_cast<std::int64_t>(epoch)));
    json::set_member(o, "event_type", json::make_string(event_type));
    json::set_member(o, "status", json::make_string(status));
    json::set_member(o, "reason", json::make_string(reason));
    json::set_member(o, "relative_ns", json::make_int(relative_ns));
    return o;
}

json::value serialize_memory_sample(
    const std::string& run_id,
    const std::string& phase,
    const bench_memory_sample& sample) {
    auto o = json::make_object();
    json::set_member(o, "schema", json::make_string("oz.navigation-benchmark.v1"));
    json::set_member(o, "record_type", json::make_string("memory_sample"));
    json::set_member(o, "run_id", json::make_string(run_id));
    json::set_member(o, "phase", json::make_string(phase));
    json::set_member(o, "raw_ticks", json::make_int(static_cast<std::int64_t>(sample.raw_ticks)));
    json::set_member(o, "relative_ns", json::make_int(sample.relative_ns));
    json::set_member(o, "private_bytes", json::make_int(static_cast<std::int64_t>(sample.private_bytes)));
    json::set_member(o, "working_set_bytes", json::make_int(static_cast<std::int64_t>(sample.working_set_bytes)));
    json::set_member(o, "peak_working_set_bytes", json::make_int(static_cast<std::int64_t>(sample.peak_working_set_bytes)));
    json::set_member(o, "cache_used_bytes", json::make_int(static_cast<std::int64_t>(sample.cache_used_bytes)));
    json::set_member(o, "cache_budget_bytes", json::make_int(static_cast<std::int64_t>(sample.cache_budget_bytes)));
    json::set_member(o, "cache_entry_count", json::make_int(static_cast<std::int64_t>(sample.cache_entry_count)));
    json::set_member(o, "resident_decoded_count", json::make_int(static_cast<std::int64_t>(sample.resident_decoded_count)));
    return o;
}

json::value serialize_scenario_summary(
    const std::string& run_id,
    const bench_scenario_summary& summary) {
    auto o = json::make_object();
    json::set_member(o, "schema", json::make_string("oz.navigation-benchmark.v1"));
    json::set_member(o, "record_type", json::make_string("scenario_summary"));
    json::set_member(o, "run_id", json::make_string(run_id));
    json::set_member(o, "scenario", json::make_string(summary.scenario));
    json::set_member(o, "os_cache_condition", json::make_string(summary.os_cache_condition));
    json::set_member(o, "requested_count", json::make_int(static_cast<std::int64_t>(summary.requested_count)));
    json::set_member(o, "successful_count", json::make_int(static_cast<std::int64_t>(summary.successful_count)));
    json::set_member(o, "superseded_count", json::make_int(static_cast<std::int64_t>(summary.superseded_count)));
    json::set_member(o, "failed_count", json::make_int(static_cast<std::int64_t>(summary.failed_count)));
    json::set_member(o, "timed_out_count", json::make_int(static_cast<std::int64_t>(summary.timed_out_count)));
    json::set_member(o, "no_op_count", json::make_int(static_cast<std::int64_t>(summary.no_op_count)));
    json::set_member(o, "min_ns", json::make_int(summary.min_ns));
    json::set_member(o, "max_ns", json::make_int(summary.max_ns));
    json::set_member(o, "median_ns", json::make_int(summary.median_ns));
    json::set_member(o, "p95_ns", json::make_int(summary.p95_ns));
    json::set_member(o, "p99_ns", json::make_int(summary.p99_ns));
    json::set_member(o, "convention", json::make_string(summary.convention));
    return o;
}

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
    std::size_t total_no_ops) {
    auto o = json::make_object();
    json::set_member(o, "schema", json::make_string("oz.navigation-benchmark.v1"));
    json::set_member(o, "record_type", json::make_string("run_end"));
    json::set_member(o, "run_id", json::make_string(run_id));
    json::set_member(o, "status", json::make_string(status));
    if (!error_reason.empty()) {
        json::set_member(o, "error_reason", json::make_string(error_reason));
    }
    json::set_member(o, "duration_ticks", json::make_int(static_cast<std::int64_t>(duration_ticks)));
    json::set_member(o, "duration_ns", json::make_int(duration_ns));
    json::set_member(o, "total_requests", json::make_int(static_cast<std::int64_t>(total_requests)));
    json::set_member(o, "total_presents", json::make_int(static_cast<std::int64_t>(total_presents)));
    json::set_member(o, "total_superseded", json::make_int(static_cast<std::int64_t>(total_superseded)));
    json::set_member(o, "total_failed", json::make_int(static_cast<std::int64_t>(total_failed)));
    json::set_member(o, "total_timed_out", json::make_int(static_cast<std::int64_t>(total_timed_out)));
    json::set_member(o, "total_no_ops", json::make_int(static_cast<std::int64_t>(total_no_ops)));
    return o;
}

}  // namespace oz::viewer_core

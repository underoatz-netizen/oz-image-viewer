#include "oz/runner/record.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>

namespace oz::runner {

std::string_view kind_name(record_kind k) noexcept {
    switch (k) {
        case record_kind::run_start: return "run_start";
        case record_kind::marker: return "marker";
        case record_kind::run_end: return "run_end";
        case record_kind::summary: return "summary";
        case record_kind::validation: return "validation";
        case record_kind::import: return "import";
        case record_kind::external_visible: return "external_visible";
    }
    return "unknown";
}

std::optional<record_kind> kind_from_name(std::string_view name) noexcept {
    if (name == "run_start") return record_kind::run_start;
    if (name == "marker") return record_kind::marker;
    if (name == "run_end") return record_kind::run_end;
    if (name == "summary") return record_kind::summary;
    if (name == "validation") return record_kind::validation;
    if (name == "import") return record_kind::import;
    if (name == "external_visible") return record_kind::external_visible;
    return std::nullopt;
}

std::string_view validation_kind_name(markers::validation_kind k) noexcept {
    switch (k) {
        case markers::validation_kind::duplicate: return "duplicate";
        case markers::validation_kind::missing: return "missing";
        case markers::validation_kind::out_of_order: return "out_of_order";
    }
    return "unknown";
}

std::string utc_now_iso8601() {
    SYSTEMTIME st{};
    GetSystemTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear,
                  st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

namespace {

void set_image(json::value& o, const std::optional<image_info>& img, bool redacted) {
    if (!img) {
        json::set_member(o, "image", json::make_null());
        return;
    }
    auto j = json::make_object();
    if (redacted) {
        // Private source paths never appear in public results: alias only.
        json::set_member(j, "path", json::make_null());
        json::set_member(j, "alias", json::make_string(img->alias));
    } else {
        json::set_member(j, "path", json::make_string(img->path));
        if (!img->alias.empty()) {
            json::set_member(j, "alias", json::make_string(img->alias));
        }
    }
    json::set_member(j, "format", json::make_string(img->format));
    if (img->width) {
        json::set_member(j, "width", json::make_int(*img->width));
    } else {
        json::set_member(j, "width", json::make_null());
    }
    if (img->height) {
        json::set_member(j, "height", json::make_int(*img->height));
    } else {
        json::set_member(j, "height", json::make_null());
    }
    if (img->size_bytes) {
        json::set_member(j, "size_bytes",
                         json::make_int(static_cast<std::int64_t>(*img->size_bytes)));
    } else {
        json::set_member(j, "size_bytes", json::make_null());
    }
    if (!img->sha256.empty()) {
        json::set_member(j, "sha256", json::make_string(img->sha256));
    }
    json::set_member(j, "provenance", json::make_string(img->provenance));
    json::set_member(o, "image", std::move(j));
}

void set_common(json::value& o, const record& r) {
    json::set_member(o, "schema", json::make_string(std::string(kSchema)));
    json::set_member(o, "kind", json::make_string(std::string(kind_name(r.kind))));
    json::set_member(o, "scenario", json::make_string(r.scenario));
    json::set_member(o, "cache", json::make_string(r.cache));
    json::set_member(o, "repetition", json::make_int(r.repetition));
    json::set_member(o, "warmup", json::make_bool(r.warmup));
    json::set_member(o, "subject", json::make_string(r.subject));
    if (r.redacted) {
        json::set_member(o, "subject_path", json::make_null());
    } else {
        json::set_member(o, "subject_path", json::make_string(r.subject_path));
    }
    if (r.subject_version) {
        json::set_member(o, "subject_version", json::make_string(*r.subject_version));
    } else {
        json::set_member(o, "subject_version", json::make_null());
    }
    if (r.subject_sha256.empty()) {
        json::set_member(o, "subject_sha256", json::make_null());
    } else {
        json::set_member(o, "subject_sha256", json::make_string(r.subject_sha256));
    }
    json::set_member(o, "collector", json::make_string(r.collector));
    json::set_member(o, "adapter", json::make_string(r.adapter));
    if (!r.endpoint.empty()) {
        json::set_member(o, "endpoint", json::make_string(r.endpoint));
    }
    if (!r.manifest_id.empty()) {
        json::set_member(o, "manifest_id", json::make_string(r.manifest_id));
    }
    if (!r.run_id.empty()) {
        json::set_member(o, "run_id", json::make_string(r.run_id));
    }
    if (r.pid != 0) {
        json::set_member(o, "pid", json::make_int(static_cast<std::int64_t>(r.pid)));
    }
    set_image(o, r.image, r.redacted);
}

void set_monitor(json::value& o, const record& r) {
    auto m = json::make_object();
    json::set_member(m, "index", json::make_int(r.monitor_index));
    json::set_member(m, "name", json::make_string(r.monitor_name));
    json::set_member(m, "width", json::make_int(r.monitor_width));
    json::set_member(m, "height", json::make_int(r.monitor_height));
    json::set_member(m, "refresh_rate", json::make_int(r.monitor_refresh_rate));
    json::set_member(o, "monitor", std::move(m));
}

void set_probe(json::value& o, const record& r) {
    auto p = json::make_object();
    json::set_member(p, "mean_luma", json::make_double(r.probe_mean_luma));
    json::set_member(p, "luma_variance", json::make_double(r.probe_luma_variance));
    json::set_member(p, "edge_density", json::make_double(r.probe_edge_density));
    json::set_member(p, "threshold_edge_density",
                     json::make_double(r.probe_threshold_edge_density));
    json::set_member(p, "threshold_luma_variance",
                     json::make_double(r.probe_threshold_luma_variance));
    json::set_member(p, "threshold_min_mean_luma",
                     json::make_double(r.probe_threshold_min_mean_luma));
    json::set_member(p, "threshold_max_mean_luma",
                     json::make_double(r.probe_threshold_max_mean_luma));
    json::set_member(p, "consecutive_frames", json::make_int(r.probe_consecutive_frames));
    json::set_member(p, "matched_frames", json::make_int(r.probe_matched_frames));
    json::set_member(o, "probe", std::move(p));
}

void set_cleanup(json::value& o, const record& r) {
    auto c = json::make_object();
    json::set_member(c, "outcome", json::make_string(r.cleanup_outcome));
    json::set_member(c, "forced", json::make_bool(r.cleanup_forced));
    json::set_member(c, "wait_ms", json::make_int(static_cast<std::int64_t>(r.cleanup_wait_ms)));
    json::set_member(o, "cleanup", std::move(c));
}

}  // namespace

std::string to_jsonl(const record& r) {
    auto o = json::make_object();
    set_common(o, r);
    switch (r.kind) {
        case record_kind::run_start:
            json::set_member(o, "started_utc", json::make_string(r.started_utc));
            break;
        case record_kind::marker:
            json::set_member(o, "marker", json::make_string(r.marker));
            json::set_member(o, "raw_ticks",
                             json::make_int(static_cast<std::int64_t>(r.raw_ticks)));
            json::set_member(o, "relative_ns", json::make_int(r.relative_ns));
            json::set_member(o, "thread_id", json::make_int(r.thread_id));
            break;
case record_kind::run_end: {
            json::set_member(o, "launched", json::make_bool(r.launched));
            if (!r.launched) {
                json::set_member(o, "error",
                                 json::make_int(static_cast<std::int64_t>(r.error)));
                json::set_member(o, "error_message", json::make_string(r.error_message));
            }
            if (r.exit_code) {
                json::set_member(o, "exit_code",
                                 json::make_int(static_cast<std::int64_t>(*r.exit_code)));
            }
            if (r.duration_ns) {
                json::set_member(o, "duration_ns",
                                 json::make_int(static_cast<std::int64_t>(*r.duration_ns)));
            }
            json::set_member(o, "timed_out", json::make_bool(r.timed_out));
            if (!r.status.empty()) {
                json::set_member(o, "status", json::make_string(r.status));
            }
            if (!r.reason.empty()) {
                json::set_member(o, "reason", json::make_string(r.reason));
            }
            if (!r.cleanup_outcome.empty()) {
                set_cleanup(o, r);
            }
            auto v = json::make_array();
            for (const auto& f : r.validation) {
                auto fo = json::make_object();
                json::set_member(fo, "kind",
                                 json::make_string(std::string(validation_kind_name(f.kind))));
                json::set_member(fo, "marker",
                                 json::make_string(std::string(markers::marker_name(f.id))));
                json::set_member(fo, "detail", json::make_string(f.detail));
                json::push_back(v, std::move(fo));
            }
            json::set_member(o, "validation", std::move(v));
            auto iss = json::make_array();
            for (const auto& s : r.issues) {
                json::push_back(iss, json::make_string(s));
            }
            json::set_member(o, "issues", std::move(iss));
            break;
        }
        case record_kind::summary:
            json::set_member(o, "marker", json::make_string(r.marker));
            json::set_member(o, "metric", json::make_string(r.metric));
            if (r.value_ns) {
                json::set_member(o, "value_ns", json::make_int(*r.value_ns));
            }
            if (r.count) {
                json::set_member(o, "count",
                                 json::make_int(static_cast<std::int64_t>(*r.count)));
            }
            json::set_member(o, "convention", json::make_string(r.convention));
            break;
        case record_kind::validation:
            json::set_member(o, "plan", json::make_string(r.plan_path));
            json::set_member(o, "valid", json::make_bool(r.valid));
            json::set_member(o, "subject_available", json::make_bool(r.subject_available));
            {
                auto e = json::make_array();
                for (const auto& s : r.errors) {
                    json::push_back(e, json::make_string(s));
                }
                json::set_member(o, "errors", std::move(e));
            }
            {
                auto w = json::make_array();
                for (const auto& s : r.warnings) {
                    json::push_back(w, json::make_string(s));
                }
                json::set_member(o, "warnings", std::move(w));
            }
            break;
case record_kind::import:
            json::set_member(o, "marker", json::make_string(r.marker));
            json::set_member(o, "relative_ns", json::make_int(r.relative_ns));
            json::set_member(o, "source", json::make_string(r.source));
            json::set_member(o, "imported_utc", json::make_string(r.imported_utc));
            break;
        case record_kind::external_visible:
            json::set_member(o, "status", json::make_string(r.status));
            if (!r.reason.empty()) {
                json::set_member(o, "reason", json::make_string(r.reason));
            }
            json::set_member(o, "relative_ns", json::make_int(r.relative_ns));
            json::set_member(o, "raw_ticks",
                             json::make_int(static_cast<std::int64_t>(r.raw_ticks)));
            json::set_member(o, "launch_ticks",
                             json::make_int(static_cast<std::int64_t>(r.launch_ticks)));
            json::set_member(o, "detect_ticks",
                             json::make_int(static_cast<std::int64_t>(r.detect_ticks)));
            json::set_member(o, "confirm_ticks",
                             json::make_int(static_cast<std::int64_t>(r.confirm_ticks)));
            if (r.hwnd != 0) {
                json::set_member(o, "hwnd",
                                 json::make_int(static_cast<std::int64_t>(r.hwnd)));
            }
            set_monitor(o, r);
            set_probe(o, r);
            if (!r.cleanup_outcome.empty()) {
                set_cleanup(o, r);
            }
            break;
    }
    return json::dump(o) + "\n";
}

std::optional<record> from_json(const json::value& v) {
    if (!json::is_object(v)) {
        return std::nullopt;
    }
    const auto kind = json::get_string(v, "kind");
    if (!kind) {
        return std::nullopt;
    }
    const auto k = kind_from_name(*kind);
    if (!k) {
        return std::nullopt;
    }
    record r;
    r.kind = *k;
    r.scenario = json::get_string(v, "scenario").value_or("");
    r.cache = json::get_string(v, "cache").value_or("");
    r.repetition = json::get_int(v, "repetition").value_or(0);
    r.warmup = json::get_bool(v, "warmup").value_or(false);
    r.subject = json::get_string(v, "subject").value_or("");
    r.marker = json::get_string(v, "marker").value_or("");
    r.relative_ns = json::get_int(v, "relative_ns").value_or(0);
    r.endpoint = json::get_string(v, "endpoint").value_or("");
    r.status = json::get_string(v, "status").value_or("");
    r.reason = json::get_string(v, "reason").value_or("");
    r.manifest_id = json::get_string(v, "manifest_id").value_or("");
    r.run_id = json::get_string(v, "run_id").value_or("");
    if (const json::value* img = json::find_member(v, "image")) {
        if (json::is_object(*img)) {
            r.image_alias = json::get_string(*img, "alias").value_or("");
        }
    }
    if (const json::value* mon = json::find_member(v, "monitor")) {
        if (json::is_object(*mon)) {
            r.monitor_index = static_cast<int>(json::get_int(*mon, "index").value_or(0));
            r.monitor_name = json::get_string(*mon, "name").value_or("");
            r.monitor_width = static_cast<int>(json::get_int(*mon, "width").value_or(0));
            r.monitor_height = static_cast<int>(json::get_int(*mon, "height").value_or(0));
            r.monitor_refresh_rate =
                static_cast<int>(json::get_int(*mon, "refresh_rate").value_or(0));
        }
    }
    if (const json::value* pr = json::find_member(v, "probe")) {
        if (json::is_object(*pr)) {
            r.probe_mean_luma = json::get_double(*pr, "mean_luma").value_or(0);
            r.probe_luma_variance = json::get_double(*pr, "luma_variance").value_or(0);
            r.probe_edge_density = json::get_double(*pr, "edge_density").value_or(0);
            r.probe_consecutive_frames =
                static_cast<int>(json::get_int(*pr, "consecutive_frames").value_or(0));
            r.probe_matched_frames =
                static_cast<int>(json::get_int(*pr, "matched_frames").value_or(0));
        }
    }
    if (const json::value* cl = json::find_member(v, "cleanup")) {
        if (json::is_object(*cl)) {
            r.cleanup_outcome = json::get_string(*cl, "outcome").value_or("");
            r.cleanup_forced = json::get_bool(*cl, "forced").value_or(false);
            r.cleanup_wait_ms =
                static_cast<std::uint32_t>(json::get_int(*cl, "wait_ms").value_or(0));
        }
    }
    return r;
}

}  // namespace oz::runner
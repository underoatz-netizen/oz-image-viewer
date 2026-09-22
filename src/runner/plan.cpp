#include "oz/runner/plan.hpp"

#include "oz/runner/json.hpp"
#include "oz/runner/utf.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace oz::runner {

plan_parse_result parse_plan(std::string_view text) {
    plan_parse_result result;
    auto parsed = json::parse(text);
    if (!parsed) {
        result.errors.push_back("plan is not valid JSON: " + parsed.error());
        return result;
    }
    const json::value& root = *parsed;
    if (!json::is_object(root)) {
        result.errors.push_back("plan root must be a JSON object");
        return result;
    }

    plan p;
    const auto schema = json::get_string(root, "schema");
    if (!schema) {
        result.errors.push_back("plan.schema: missing");
    } else if (*schema != kPlanSchema) {
        result.errors.push_back("plan.schema: unsupported '" + *schema +
                                "' (expected " + std::string(kPlanSchema) + ")");
    }
    p.schema = schema.value_or("");

    p.name = json::get_string(root, "name").value_or("");
    if (p.name.empty()) {
        result.errors.push_back("plan.name: missing or empty");
    }

    const json::value* subj = json::find_member(root, "subject");
    if (!subj || !json::is_object(*subj)) {
        result.errors.push_back("plan.subject: missing object");
    } else {
        p.subject.name = json::get_string(*subj, "name").value_or("");
        if (p.subject.name.empty()) {
            result.errors.push_back("plan.subject.name: missing or empty");
        }
        p.subject.executable = json::get_string(*subj, "executable").value_or("");
        if (p.subject.executable.empty()) {
            result.errors.push_back("plan.subject.executable: missing or empty");
        }
        p.subject.version_hint = json::get_string(*subj, "version_hint");
        p.subject.enabled = json::get_bool(*subj, "enabled").value_or(true);
        p.subject.skip_reason = json::get_string(*subj, "skip_reason").value_or("");
    }

    p.collector = json::get_string(root, "collector").value_or("markers");
    if (p.collector != "markers" && p.collector != "external_first_present" &&
        p.collector != "external_visible_frame") {
        result.errors.push_back("plan.collector: must be 'markers', "
                                "'external_first_present' or 'external_visible_frame'");
    }
    p.adapter = json::get_string(root, "adapter").value_or("oz_markers");
    if (p.adapter.empty()) {
        result.errors.push_back("plan.adapter: missing or empty");
    }

    // Endpoint provenance: the external_visible_frame collector measures
    // ExternalFirstImageVisible and must never be labelled FirstPresent.
    p.endpoint = json::get_string(root, "endpoint").value_or("");
    if (p.collector == "external_visible_frame") {
        if (p.endpoint.empty()) {
            p.endpoint = "ExternalFirstImageVisible";
        }
        if (p.endpoint != "ExternalFirstImageVisible") {
            result.errors.push_back(
                "plan.endpoint: external_visible_frame must use "
                "'ExternalFirstImageVisible' (never FirstPresent / window-ready / TTFI)");
        }
    }

    p.manifest = json::get_string(root, "manifest");

    // Collector configuration (external_visible_frame).
    if (const json::value* cc = json::find_member(root, "collector_config")) {
        if (!json::is_object(*cc)) {
            result.errors.push_back("plan.collector_config: must be an object");
        } else {
            collector_config cfg;
            if (const auto mi = json::get_int(*cc, "monitor_index")) {
                if (*mi < -1) {
                    result.errors.push_back(
                        "plan.collector_config.monitor_index: must be >= -1");
                } else {
                    cfg.monitor_index = static_cast<int>(*mi);
                }
            }
            if (const auto v = json::get_int(*cc, "window_wait_ms")) {
                if (*v < 1 || *v > kMaxTimeoutMs) {
                    result.errors.push_back(
                        "plan.collector_config.window_wait_ms: must be in [1, " +
                        std::to_string(kMaxTimeoutMs) + "]");
                } else {
                    cfg.window_wait_ms = static_cast<std::uint32_t>(*v);
                }
            }
            if (const auto v = json::get_int(*cc, "frame_timeout_ms")) {
                if (*v < 1 || *v > kMaxTimeoutMs) {
                    result.errors.push_back(
                        "plan.collector_config.frame_timeout_ms: must be in [1, " +
                        std::to_string(kMaxTimeoutMs) + "]");
                } else {
                    cfg.frame_timeout_ms = static_cast<std::uint32_t>(*v);
                }
            }
            if (const json::value* pr = json::find_member(*cc, "probe")) {
                if (!json::is_object(*pr)) {
                    result.errors.push_back("plan.collector_config.probe: must be an object");
                } else {
                    const auto in01 = [](double d) { return d >= 0.0 && d <= 1.0; };
                    if (const auto v = json::get_double(*pr, "min_edge_density")) {
                        if (!in01(*v)) {
                            result.errors.push_back(
                                "plan.collector_config.probe.min_edge_density: must be in [0,1]");
                        } else {
                            cfg.probe.min_edge_density = *v;
                        }
                    }
                    if (const auto v = json::get_double(*pr, "min_luma_variance")) {
                        if (!in01(*v)) {
                            result.errors.push_back(
                                "plan.collector_config.probe.min_luma_variance: must be in [0,1]");
                        } else {
                            cfg.probe.min_luma_variance = *v;
                        }
                    }
                    if (const auto v = json::get_double(*pr, "min_mean_luma")) {
                        if (!in01(*v)) {
                            result.errors.push_back(
                                "plan.collector_config.probe.min_mean_luma: must be in [0,1]");
                        } else {
                            cfg.probe.min_mean_luma = *v;
                        }
                    }
                    if (const auto v = json::get_double(*pr, "max_mean_luma")) {
                        if (!in01(*v)) {
                            result.errors.push_back(
                                "plan.collector_config.probe.max_mean_luma: must be in [0,1]");
                        } else {
                            cfg.probe.max_mean_luma = *v;
                        }
                    }
                    if (const auto v = json::get_int(*pr, "consecutive_frames")) {
                        if (*v < 1 || *v > 100) {
                            result.errors.push_back(
                                "plan.collector_config.probe.consecutive_frames: must be in [1,100]");
                        } else {
                            cfg.probe.consecutive_frames = static_cast<int>(*v);
                        }
                    }
                    if (cfg.probe.min_mean_luma > cfg.probe.max_mean_luma) {
                        result.errors.push_back(
                            "plan.collector_config.probe: min_mean_luma must be <= max_mean_luma");
                    }
                }
            }
            p.collector_config = cfg;
        }
    }

    p.cache = json::get_string(root, "cache").value_or("cold");
    if (p.cache != "cold" && p.cache != "warm") {
        result.errors.push_back(
            "plan.cache: must be 'cold' or 'warm' (protocol label; never purged automatically)");
    }

    p.working_directory = json::get_string(root, "working_directory");
    if (const auto v = json::get_int(root, "timeout_ms")) {
        if (*v < 1) {
            result.errors.push_back("plan.timeout_ms: must be >= 1");
        } else if (*v > kMaxTimeoutMs) {
            result.errors.push_back("plan.timeout_ms: must be <= " +
                                    std::to_string(kMaxTimeoutMs));
        } else {
            p.timeout_ms = static_cast<std::uint32_t>(*v);
        }
    } else {
        p.timeout_ms = 30000;
    }
    if (const auto v = json::get_int(root, "repetitions")) {
        if (*v < 1) {
            result.errors.push_back("plan.repetitions: must be >= 1");
        } else if (*v > static_cast<std::int64_t>(kMaxRepetitions)) {
            result.errors.push_back("plan.repetitions: must be <= " +
                                    std::to_string(kMaxRepetitions));
        } else {
            p.repetitions = static_cast<std::uint32_t>(*v);
        }
    } else {
        p.repetitions = 5;
    }
    if (const auto v = json::get_int(root, "warmups")) {
        if (*v < 0) {
            result.errors.push_back("plan.warmups: must be >= 0");
        } else if (*v > static_cast<std::int64_t>(kMaxWarmups)) {
            result.errors.push_back("plan.warmups: must be <= " +
                                    std::to_string(kMaxWarmups));
        } else {
            p.warmups = static_cast<std::uint32_t>(*v);
        }
    } else {
        p.warmups = 0;
    }

    const json::value* args = json::find_member(root, "args");
    if (args && json::is_array(*args)) {
        for (std::size_t i = 0; i < json::array_size(*args); ++i) {
            const json::value& a = json::array_at(*args, i);
            if (json::is_string(a)) {
                p.args.push_back(json::as_string(a));
            } else {
                result.errors.push_back("plan.args[" + std::to_string(i) +
                                        "]: must be a string");
            }
        }
    } else if (args) {
        result.errors.push_back("plan.args: must be an array of strings");
    }

    const json::value* imgs = json::find_member(root, "images");
    if (!imgs || !json::is_array(*imgs) || json::array_size(*imgs) == 0) {
        result.errors.push_back("plan.images: must be a non-empty array");
    } else {
        for (std::size_t i = 0; i < json::array_size(*imgs); ++i) {
            const json::value& iv = json::array_at(*imgs, i);
            if (!json::is_object(iv)) {
                result.errors.push_back("plan.images[" + std::to_string(i) +
                                        "]: must be an object");
                continue;
            }
            image_spec img;
            img.path = json::get_string(iv, "path").value_or("");
            if (img.path.empty()) {
                result.errors.push_back("plan.images[" + std::to_string(i) +
                                        "].path: missing or empty");
            }
            img.alias = json::get_string(iv, "alias").value_or("");
            img.sha256 = json::get_string(iv, "sha256").value_or("");
            img.format = json::get_string(iv, "format").value_or("");
            img.width = json::get_int(iv, "width");
            img.height = json::get_int(iv, "height");
            if (const auto sb = json::get_int(iv, "size_bytes")) {
                img.size_bytes = static_cast<std::uint64_t>(*sb);
            }
            img.provenance = json::get_string(iv, "provenance").value_or("");
            p.images.push_back(std::move(img));
        }
    }

    if (result.errors.empty()) {
        result.value = std::move(p);
    }
    return result;
}

std::string scenario_key(const plan& p, const image_spec& img) {
    return p.subject.name + "|" + p.cache + "|" + img.path;
}

bool subject_available(const plan& p) {
    const std::wstring w = utf8_to_wide(p.subject.executable);
    if (w.empty()) {
        return false;
    }
    const DWORD attrs = GetFileAttributesW(w.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

}  // namespace oz::runner
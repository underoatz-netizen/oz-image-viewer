// ozbench - Stage 0 benchmark runner CLI + Stage 1 reference collector.
//
// Commands (noninteractive):
//   validate <plan.json> [--set KEY=VALUE]...
//   run <plan.json> [--out FILE] [--scenario SUBSTR] [--set KEY=VALUE]...
//                   [--repetitions N] [--warmups N]
//   capture <plan.json> [--out FILE] [--manifest FILE] [--monitor N]
//                       [--set KEY=VALUE]... [--repetitions N] [--warmups N]
//   summarize <input.jsonl> [--out FILE] [--format json|csv|jsonl]
//                           [--include-warmups] [--scenario SUBSTR]
//   import <file> [--format csv|jsonl] [--out FILE] [--subject NAME]
//                 [--cache cold|warm] [--scenario ID] [--marker NAME]
//
// Exit codes: 0 ok, 1 usage/IO error, 2 invalid plan, 3 subject unavailable
// or capture did not match, 4 capture environment unavailable.
// Canonical output is UTF-8 JSON Lines (schema oz.benchmark.v1).
#include "oz/collector/capture.hpp"
#include "oz/collector/manifest.hpp"
#include "oz/runner/collect.hpp"
#include "oz/runner/import.hpp"
#include "oz/runner/plan.hpp"
#include "oz/runner/record.hpp"
#include "oz/runner/run.hpp"
#include "oz/runner/summarize.hpp"
#include "oz/runner/utf.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace oz::runner;
namespace json = oz::json;

namespace {

constexpr const char* kVersion = "0.1.0";

void print_usage() {
    std::cout
        << "ozbench " << kVersion
        << " - Stage 0 benchmark harness + Stage 1 reference collector\n"
        << "\n"
        << "Usage:\n"
        << "  ozbench validate <plan.json> [--set KEY=VALUE]...\n"
        << "  ozbench run <plan.json> [--out FILE] [--scenario SUBSTR]\n"
        << "                       [--set KEY=VALUE]... [--repetitions N] [--warmups N]\n"
        << "  ozbench capture <plan.json> [--out FILE] [--manifest FILE] [--monitor N]\n"
        << "                       [--set KEY=VALUE]... [--repetitions N] [--warmups N]\n"
        << "  ozbench summarize <input.jsonl> [--out FILE] [--format json|csv|jsonl]\n"
        << "                       [--include-warmups] [--scenario SUBSTR]\n"
        << "  ozbench import <file> [--format csv|jsonl] [--out FILE] [--subject NAME]\n"
        << "                       [--cache cold|warm] [--scenario ID] [--marker NAME]\n"
        << "  ozbench --version | --help\n"
        << "\n"
        << "Exit codes: 0 ok, 1 usage/IO error, 2 invalid plan, 3 subject unavailable\n"
        << "or capture did not match, 4 capture environment unavailable.\n"
        << "Canonical output is UTF-8 JSON Lines (schema oz.benchmark.v1).\n";
}

bool read_file(const std::wstring& path, std::string& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open file";
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB &&
        static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);  // strip UTF-8 BOM
    }
    return true;
}

bool write_file(const std::wstring& path, const std::string& text, std::string& err) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        err = "cannot open file for writing";
        return false;
    }
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return f.good();
}

std::map<std::string, std::string> parse_defines(const std::vector<std::wstring>& args,
                                                 std::size_t& i, bool& ok) {
    std::map<std::string, std::string> defines;
    while (i + 1 < args.size() && args[i] == L"--set") {
        const std::string kv = wide_to_utf8(args[i + 1]);
        const auto eq = kv.find('=');
        if (eq == std::string::npos || eq == 0) {
            std::cerr << "ozbench: --set expects KEY=VALUE\n";
            ok = false;
            return defines;
        }
        defines[kv.substr(0, eq)] = kv.substr(eq + 1);
        i += 2;
    }
    return defines;
}

record make_validation_record(const std::wstring& plan_path, bool valid,
                              bool subject_available,
                              const std::vector<std::string>& errors,
                              const std::vector<std::string>& warnings) {
    record r;
    r.kind = record_kind::validation;
    r.plan_path = wide_to_utf8(plan_path);
    r.valid = valid;
    r.subject_available = subject_available;
    r.errors = errors;
    r.warnings = warnings;
    return r;
}

int cmd_validate(const std::wstring& plan_path, const std::vector<std::wstring>& args) {
    bool ok = true;
    std::size_t i = 0;
    const auto defines = parse_defines(args, i, ok);
    if (!ok) {
        return 1;
    }
    std::string text, err;
    if (!read_file(plan_path, text, err)) {
        std::cerr << "ozbench: " << err << ": " << wide_to_utf8(plan_path) << "\n";
        return 1;
    }
    auto pr = parse_plan(text);
    if (!pr.value) {
        std::cout << to_jsonl(make_validation_record(plan_path, false, false, pr.errors,
                                                     pr.warnings));
        return 2;
    }
    // Substitute placeholders before checking availability so template plans
    // (e.g. {{fixture_exe}}) can be validated with --set.
    std::map<std::string, std::string> vars = defines;
    if (const char* env = std::getenv("OZBENCH_FIXTURE_EXE")) {
        vars.try_emplace("fixture_exe", env);
    }
    plan resolved = *pr.value;
    resolved.subject.executable = [&] {
        std::string s = resolved.subject.executable;
        for (const auto& [k, v] : vars) {
            const std::string key = "{{" + k + "}}";
            std::size_t pos = 0;
            while ((pos = s.find(key, pos)) != std::string::npos) {
                s.replace(pos, key.size(), v);
                pos += v.size();
            }
        }
        return s;
    }();

    std::vector<std::string> warnings = pr.warnings;
    bool available = resolved.subject.enabled && subject_available(resolved);
    if (!resolved.subject.enabled) {
        warnings.push_back("subject disabled: " + resolved.subject.skip_reason);
    }
    if (!subject_available(resolved)) {
        warnings.push_back("subject executable not found: " + resolved.subject.executable);
    }
    std::cout << to_jsonl(make_validation_record(plan_path, true, available, {}, warnings));
    return available ? 0 : 3;
}

int cmd_run(const std::wstring& plan_path, const std::vector<std::wstring>& args) {
    std::optional<std::wstring> out_path;
    std::optional<std::string> scenario_filter;
    std::map<std::string, std::string> defines;
    std::optional<std::uint32_t> reps, warmups;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::wstring a = args[i];
        if (a == L"--out" && i + 1 < args.size()) {
            out_path = args[++i];
        } else if (a == L"--scenario" && i + 1 < args.size()) {
            scenario_filter = wide_to_utf8(args[++i]);
        } else if (a == L"--set" && i + 1 < args.size()) {
            const std::string kv = wide_to_utf8(args[++i]);
            const auto eq = kv.find('=');
            if (eq == std::string::npos || eq == 0) {
                std::cerr << "ozbench: --set expects KEY=VALUE\n";
                return 1;
            }
            defines[kv.substr(0, eq)] = kv.substr(eq + 1);
} else if (a == L"--repetitions" && i + 1 < args.size()) {
            const int v = _wtoi(args[++i].c_str());
            if (v < 1 || v > static_cast<int>(kMaxRepetitions)) {
                std::cerr << "ozbench: --repetitions must be in [1, " << kMaxRepetitions
                          << "]\n";
                return 1;
            }
            reps = static_cast<std::uint32_t>(v);
        } else if (a == L"--warmups" && i + 1 < args.size()) {
            const int v = _wtoi(args[++i].c_str());
            if (v < 0 || v > static_cast<int>(kMaxWarmups)) {
                std::cerr << "ozbench: --warmups must be in [0, " << kMaxWarmups << "]\n";
                return 1;
            }
            warmups = static_cast<std::uint32_t>(v);
        } else {
            std::cerr << "ozbench: unknown run option: " << wide_to_utf8(a) << "\n";
            return 1;
        }
    }

    std::string text, err;
    if (!read_file(plan_path, text, err)) {
        std::cerr << "ozbench: " << err << ": " << wide_to_utf8(plan_path) << "\n";
        return 1;
    }
    auto pr = parse_plan(text);
    if (!pr.value) {
        std::cout << to_jsonl(make_validation_record(plan_path, false, false, pr.errors,
                                                     pr.warnings));
        return 2;
    }

    run_options ro;
    ro.defines = defines;
    ro.scenario_filter = scenario_filter;
    ro.repetitions = reps;
    ro.warmups = warmups;
    std::string run_err;
    const auto records = run_plan(*pr.value, ro, run_err);
    if (records.empty() && !run_err.empty()) {
        std::cout << to_jsonl(make_validation_record(plan_path, true, false, {run_err}, {}));
        return 3;
    }
    if (records.empty()) {
        std::cerr << "ozbench: no runs produced (scenario filter matched nothing?)\n";
        return 0;
    }

std::string out;
    out.reserve(records.size() * 256);
    std::vector<std::string> launch_errors;
    for (const auto& rec : records) {
        out += to_jsonl(rec);
        if (rec.kind == record_kind::run_end && !rec.launched) {
            launch_errors.push_back("launch failed: " + rec.error_message + " (error " +
                                    std::to_string(rec.error) + ")");
        }
    }
    if (!launch_errors.empty()) {
        // A failed launch must never look like clean success: append a
        // validation record and exit non-zero.
        out += to_jsonl(make_validation_record(plan_path, false, true, launch_errors, {}));
    }
    if (out_path) {
        if (!write_file(*out_path, out, err)) {
            std::cerr << "ozbench: " << err << "\n";
            return 1;
        }
    } else {
        std::cout << out;
    }
    return launch_errors.empty() ? 0 : 3;
}

int cmd_capture(const std::wstring& plan_path, const std::vector<std::wstring>& args) {
    std::optional<std::wstring> out_path;
    std::optional<std::wstring> manifest_path;
    std::optional<int> monitor_override;
    std::map<std::string, std::string> defines;
    std::optional<std::uint32_t> reps, warmups;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::wstring a = args[i];
        if (a == L"--out" && i + 1 < args.size()) {
            out_path = args[++i];
        } else if (a == L"--manifest" && i + 1 < args.size()) {
            manifest_path = args[++i];
        } else if (a == L"--monitor" && i + 1 < args.size()) {
            monitor_override = _wtoi(args[++i].c_str());
        } else if (a == L"--set" && i + 1 < args.size()) {
            const std::string kv = wide_to_utf8(args[++i]);
            const auto eq = kv.find('=');
            if (eq == std::string::npos || eq == 0) {
                std::cerr << "ozbench: --set expects KEY=VALUE\n";
                return 1;
            }
            defines[kv.substr(0, eq)] = kv.substr(eq + 1);
        } else if (a == L"--repetitions" && i + 1 < args.size()) {
            const int v = _wtoi(args[++i].c_str());
            if (v < 1 || v > static_cast<int>(kMaxRepetitions)) {
                std::cerr << "ozbench: --repetitions must be in [1, " << kMaxRepetitions
                          << "]\n";
                return 1;
            }
            reps = static_cast<std::uint32_t>(v);
        } else if (a == L"--warmups" && i + 1 < args.size()) {
            const int v = _wtoi(args[++i].c_str());
            if (v < 0 || v > static_cast<int>(kMaxWarmups)) {
                std::cerr << "ozbench: --warmups must be in [0, " << kMaxWarmups << "]\n";
                return 1;
            }
            warmups = static_cast<std::uint32_t>(v);
        } else {
            std::cerr << "ozbench: unknown capture option: " << wide_to_utf8(a) << "\n";
            return 1;
        }
    }

    std::string text, err;
    if (!read_file(plan_path, text, err)) {
        std::cerr << "ozbench: " << err << ": " << wide_to_utf8(plan_path) << "\n";
        return 1;
    }
    auto pr = parse_plan(text);
    if (!pr.value) {
        std::cout << to_jsonl(make_validation_record(plan_path, false, false, pr.errors,
                                                     pr.warnings));
        return 2;
    }
    plan p = *pr.value;
    if (p.collector != "external_visible_frame") {
        std::cerr << "ozbench: capture requires collector \"external_visible_frame\"\n";
        return 1;
    }
    if (!p.subject.enabled) {
        std::cout << to_jsonl(make_validation_record(
            plan_path, true, false, {}, {"subject disabled: " + p.subject.skip_reason}));
        return 3;
    }

    // Sealed private manifest: real source paths live here and never appear
    // in public results. Relative manifest paths resolve against the plan
    // file's directory so templates work from any working directory.
    std::wstring mpath = manifest_path.value_or(utf8_to_wide(p.manifest.value_or("")));
    if (!mpath.empty() && !(mpath.size() >= 2 && mpath[1] == L':') &&
        !(mpath.size() >= 2 && mpath[0] == L'\\' && mpath[1] == L'\\') &&
        mpath[0] != L'\\') {
        std::wstring dir = plan_path;
        const auto slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            dir.erase(slash + 1);
            mpath = dir + mpath;
        }
    }
    if (mpath.empty()) {
        std::cerr << "ozbench: capture requires a private manifest "
                     "(plan.manifest or --manifest)\n";
        return 1;
    }
    std::string merr;
    auto man = oz::collector::load_manifest(mpath, merr);
    if (!man) {
        std::cerr << "ozbench: manifest: " << merr << "\n";
        return 1;
    }

    // Resolve the subject executable (placeholders).
    std::map<std::string, std::string> vars = defines;
    if (const char* env = std::getenv("OZBENCH_FIXTURE_EXE")) {
        vars.try_emplace("fixture_exe", env);
    }
    const auto substitute = [&vars](std::string s) {
        for (const auto& [k, v] : vars) {
            const std::string key = "{{" + k + "}}";
            std::size_t pos = 0;
            while ((pos = s.find(key, pos)) != std::string::npos) {
                s.replace(pos, key.size(), v);
                pos += v.size();
            }
        }
        return s;
    };
    const std::string resolved_exe = substitute(p.subject.executable);
    {
        plan resolved = p;
        resolved.subject.executable = resolved_exe;
        if (!subject_available(resolved)) {
            std::cout << to_jsonl(make_validation_record(
                plan_path, true, false,
                {"subject executable not found: " + resolved_exe}, {}));
            return 3;
        }
    }
    const std::string exe_sha256 = oz::collector::sha256_file_hex(utf8_to_wide(resolved_exe));

    const std::uint64_t reps_n = reps.value_or(p.repetitions);
    const std::uint64_t warmups_n = warmups.value_or(p.warmups);
    if (reps_n < 1 || reps_n > kMaxRepetitions) {
        std::cerr << "ozbench: repetitions out of range [1, " << kMaxRepetitions << "]\n";
        return 1;
    }
    if (warmups_n > kMaxWarmups) {
        std::cerr << "ozbench: warmups out of range [0, " << kMaxWarmups << "]\n";
        return 1;
    }
    const std::uint64_t total_runs = warmups_n + reps_n;

    const collector_config cc = p.collector_config.value_or(collector_config{});
    const int monitor_index = monitor_override.value_or(cc.monitor_index);
    const std::string run_id =
        utc_now_iso8601() + "-" + std::to_string(GetCurrentProcessId());

    std::string out;
    std::vector<std::string> failures;
    bool env_unavailable = false;
    for (const auto& img : p.images) {
        // Link the plan image to the sealed manifest by alias (or path).
        const oz::collector::manifest_image* mi = nullptr;
        for (const auto& m : man->images) {
            if (!img.alias.empty() && m.alias == img.alias) {
                mi = &m;
                break;
            }
            if (img.alias.empty() && m.path == img.path) {
                mi = &m;
                break;
            }
        }
        if (!mi) {
            std::cerr << "ozbench: plan image not found in manifest: "
                      << (img.alias.empty() ? img.path : img.alias) << "\n";
            return 1;
        }
        const std::string scenario = p.subject.name + "|" + p.cache + "|" +
                                     (img.alias.empty() ? mi->alias : img.alias);
        vars["image_path"] = mi->path;  // private; used only for subject args

        const std::wstring exe = utf8_to_wide(resolved_exe);
        std::vector<std::wstring> cargs;
        for (const auto& a : p.args) {
            cargs.push_back(utf8_to_wide(substitute(a)));
        }
        std::optional<std::wstring> workdir;
        if (p.working_directory) {
            workdir = utf8_to_wide(substitute(*p.working_directory));
        }

        for (std::uint64_t i = 0; i < total_runs; ++i) {
            const bool warmup = i < warmups_n;
            const std::int64_t repetition = static_cast<std::int64_t>(i);

            oz::collector::capture_options co;
            co.executable = exe;
            co.args = cargs;
            co.working_directory = workdir;
            co.monitor_index = monitor_index;
            co.window_wait_ms = cc.window_wait_ms;
            co.frame_timeout_ms = cc.frame_timeout_ms;
            co.probe.min_edge_density = cc.probe.min_edge_density;
            co.probe.min_luma_variance = cc.probe.min_luma_variance;
            co.probe.min_mean_luma = cc.probe.min_mean_luma;
            co.probe.max_mean_luma = cc.probe.max_mean_luma;
            co.probe.consecutive_frames = cc.probe.consecutive_frames;
            const auto cr = oz::collector::capture_external_visible(co);

            const std::string status = oz::collector::capture_status_name(cr.status);

            // run_start (redacted: no subject_path, no image.path).
            record rs;
            rs.kind = record_kind::run_start;
            rs.scenario = scenario;
            rs.cache = p.cache;
            rs.repetition = repetition;
            rs.warmup = warmup;
            rs.subject = p.subject.name;
            rs.subject_version = p.subject.version_hint;
            rs.subject_sha256 = exe_sha256;
            rs.collector = p.collector;
            rs.adapter = p.adapter;
            rs.endpoint = p.endpoint;
            rs.manifest_id = man->id;
            rs.run_id = run_id;
            rs.redacted = true;
            rs.image_alias = mi->alias;
            rs.pid = cr.pid;
            image_info ii;
            ii.alias = mi->alias;
            ii.sha256 = mi->sha256;
            ii.format = img.format;
            ii.width = mi->width > 0 ? std::optional<std::int64_t>(mi->width) : img.width;
            ii.height = mi->height > 0 ? std::optional<std::int64_t>(mi->height) : img.height;
            ii.size_bytes = mi->size_bytes > 0
                                ? std::optional<std::uint64_t>(mi->size_bytes)
                                : img.size_bytes;
            ii.provenance = img.provenance;
            rs.image = ii;
            rs.started_utc = utc_now_iso8601();
            out += to_jsonl(rs);

            // external_visible: the reference endpoint measurement.
            record ev;
            ev.kind = record_kind::external_visible;
            ev.scenario = scenario;
            ev.cache = p.cache;
            ev.repetition = repetition;
            ev.warmup = warmup;
            ev.subject = p.subject.name;
            ev.subject_sha256 = exe_sha256;
            ev.collector = p.collector;
            ev.adapter = p.adapter;
            ev.endpoint = p.endpoint;
            ev.manifest_id = man->id;
            ev.run_id = run_id;
            ev.redacted = true;
            ev.image_alias = mi->alias;
            ev.pid = cr.pid;
            ev.hwnd = cr.hwnd;
            ev.status = status;
            ev.reason = cr.reason;
            ev.relative_ns = cr.relative_ns;
            ev.raw_ticks = cr.detect_ticks;
            ev.launch_ticks = cr.launch_ticks;
            ev.detect_ticks = cr.detect_ticks;
            ev.confirm_ticks = cr.confirm_ticks;
            ev.monitor_index = cr.monitor.index;
            ev.monitor_name = cr.monitor.name;
            ev.monitor_width = cr.monitor.width;
            ev.monitor_height = cr.monitor.height;
            ev.monitor_refresh_rate = cr.monitor.refresh_rate;
            ev.probe_mean_luma = cr.probe.mean_luma;
            ev.probe_luma_variance = cr.probe.luma_variance;
            ev.probe_edge_density = cr.probe.edge_density;
            ev.probe_threshold_edge_density = cc.probe.min_edge_density;
            ev.probe_threshold_luma_variance = cc.probe.min_luma_variance;
            ev.probe_threshold_min_mean_luma = cc.probe.min_mean_luma;
            ev.probe_threshold_max_mean_luma = cc.probe.max_mean_luma;
            ev.probe_consecutive_frames = cc.probe.consecutive_frames;
            ev.probe_matched_frames = cr.probe.matched ? cc.probe.consecutive_frames : 0;
            ev.cleanup_outcome = cr.cleanup.outcome;
            ev.cleanup_forced = cr.cleanup.forced;
            ev.cleanup_wait_ms = cr.cleanup.wait_ms;
            ev.image = ii;
            out += to_jsonl(ev);

            // run_end (redacted).
            record re;
            re.kind = record_kind::run_end;
            re.scenario = scenario;
            re.cache = p.cache;
            re.repetition = repetition;
            re.warmup = warmup;
            re.subject = p.subject.name;
            re.subject_version = p.subject.version_hint;
            re.subject_sha256 = exe_sha256;
            re.collector = p.collector;
            re.adapter = p.adapter;
            re.endpoint = p.endpoint;
            re.manifest_id = man->id;
            re.run_id = run_id;
            re.redacted = true;
            re.image_alias = mi->alias;
            re.pid = cr.pid;
            re.launched = cr.status != oz::collector::capture_status::launch_failed &&
                          cr.status != oz::collector::capture_status::preflight_failed;
            re.error = cr.error;
            re.error_message = cr.reason;
            re.exit_code = cr.exit_code;
            re.timed_out = cr.timed_out;
            re.status = status;
            re.reason = cr.reason;
            re.cleanup_outcome = cr.cleanup.outcome;
            re.cleanup_forced = cr.cleanup.forced;
            re.cleanup_wait_ms = cr.cleanup.wait_ms;
            re.image = ii;
            if (cr.status != oz::collector::capture_status::matched) {
                re.issues.push_back("capture " + status + ": " + cr.reason);
                failures.push_back(scenario + ": " + status + " - " + cr.reason);
                if (cr.status == oz::collector::capture_status::capture_failed &&
                    cr.reason.find("Desktop Duplication") != std::string::npos) {
                    env_unavailable = true;
                }
            }
            out += to_jsonl(re);
        }
    }

    if (out_path) {
        if (!write_file(*out_path, out, err)) {
            std::cerr << "ozbench: " << err << "\n";
            return 1;
        }
    } else {
        std::cout << out;
    }
    if (!failures.empty()) {
        for (const auto& f : failures) {
            std::cerr << "ozbench: capture failure: " << f << "\n";
        }
        return env_unavailable ? 4 : 3;
    }
    return 0;
}

int cmd_summarize(const std::wstring& input_path, const std::vector<std::wstring>& args) {
    std::optional<std::wstring> out_path;
    std::string format = "json";
    bool include_warmups = false;
    std::optional<std::string> scenario_filter;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::wstring a = args[i];
        if (a == L"--out" && i + 1 < args.size()) {
            out_path = args[++i];
        } else if (a == L"--format" && i + 1 < args.size()) {
            format = wide_to_utf8(args[++i]);
        } else if (a == L"--include-warmups") {
            include_warmups = true;
        } else if (a == L"--scenario" && i + 1 < args.size()) {
            scenario_filter = wide_to_utf8(args[++i]);
        } else {
            std::cerr << "ozbench: unknown summarize option: " << wide_to_utf8(a) << "\n";
            return 1;
        }
    }
    if (format != "json" && format != "csv" && format != "jsonl") {
        std::cerr << "ozbench: --format must be json, csv, or jsonl\n";
        return 1;
    }

    std::string text, err;
    if (!read_file(input_path, text, err)) {
        std::cerr << "ozbench: " << err << ": " << wide_to_utf8(input_path) << "\n";
        return 1;
    }

std::vector<record> records;
    const std::string_view text_view = text;
    std::size_t start = 0;
    while (start <= text_view.size()) {
        std::size_t end = text_view.find('\n', start);
        if (end == std::string_view::npos) {
            end = text_view.size();
        }
        std::string_view line = text_view.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            continue;
        }
auto parsed = json::parse(line);
        if (!parsed) {
            std::cerr << "ozbench: summarize: skipping unparseable line: " << parsed.error()
                      << "\n";
            continue;  // tolerate foreign lines; only canonical records count
        }
        if (auto rec = from_json(*parsed)) {
            records.push_back(std::move(*rec));
        } else {
            std::cerr << "ozbench: summarize: skipping non-canonical line\n";
        }
    }

    std::vector<record> filtered;
    filtered.reserve(records.size());
    for (auto& r : records) {
        if (scenario_filter && r.scenario.find(*scenario_filter) == std::string::npos) {
            continue;
        }
        filtered.push_back(std::move(r));
    }

    const auto rows = summarize_records(filtered, include_warmups);
    std::string out;
    if (format == "csv") {
        out = summary_to_csv(rows);
    } else if (format == "jsonl") {
        for (const auto& rec : summary_records_to_jsonl(rows)) {
            out += to_jsonl(rec);
        }
    } else {
        out = json::dump(summary_to_json(rows)) + "\n";
    }
    if (out_path) {
        if (!write_file(*out_path, out, err)) {
            std::cerr << "ozbench: " << err << "\n";
            return 1;
        }
    } else {
        std::cout << out;
    }
    return 0;
}

int cmd_import(const std::wstring& file_path, const std::vector<std::wstring>& args) {
    import_options opts;
    std::optional<std::wstring> out_path;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::wstring a = args[i];
        if (a == L"--format" && i + 1 < args.size()) {
            opts.format = wide_to_utf8(args[++i]);
        } else if (a == L"--out" && i + 1 < args.size()) {
            out_path = args[++i];
        } else if (a == L"--subject" && i + 1 < args.size()) {
            opts.subject = wide_to_utf8(args[++i]);
        } else if (a == L"--cache" && i + 1 < args.size()) {
            opts.cache = wide_to_utf8(args[++i]);
        } else if (a == L"--scenario" && i + 1 < args.size()) {
            opts.scenario = wide_to_utf8(args[++i]);
        } else if (a == L"--marker" && i + 1 < args.size()) {
            opts.marker = wide_to_utf8(args[++i]);
        } else {
            std::cerr << "ozbench: unknown import option: " << wide_to_utf8(a) << "\n";
            return 1;
        }
    }
    if (opts.format != "csv" && opts.format != "jsonl") {
        std::cerr << "ozbench: --format must be csv or jsonl\n";
        return 1;
    }

    std::string text, err;
    if (!read_file(file_path, text, err)) {
        std::cerr << "ozbench: " << err << ": " << wide_to_utf8(file_path) << "\n";
        return 1;
    }
    std::vector<std::string> errors;
    const auto records = import_samples(text, opts, errors);
    for (const auto& e : errors) {
        std::cerr << "ozbench: " << e << "\n";
    }
    std::string out;
    for (const auto& rec : records) {
        out += to_jsonl(rec);
    }
    if (out_path) {
        if (!write_file(*out_path, out, err)) {
            std::cerr << "ozbench: " << err << "\n";
            return 1;
        }
    } else {
        std::cout << out;
    }
    return errors.empty() ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    if (argc < 2) {
        print_usage();
        return 0;
    }
    const std::wstring cmd = argv[1];
    if (cmd == L"--help" || cmd == L"-h" || cmd == L"/?") {
        print_usage();
        return 0;
    }
    if (cmd == L"--version") {
        std::cout << "ozbench " << kVersion << "\n";
        return 0;
    }

    std::vector<std::wstring> rest;
    for (int i = 2; i < argc; ++i) {
        rest.push_back(argv[i]);
    }

    if (cmd == L"validate") {
        if (rest.empty()) {
            std::cerr << "ozbench: validate requires a plan file\n";
            return 1;
        }
        return cmd_validate(rest[0], std::vector<std::wstring>(rest.begin() + 1, rest.end()));
    }
if (cmd == L"run") {
        if (rest.empty()) {
            std::cerr << "ozbench: run requires a plan file\n";
            return 1;
        }
        return cmd_run(rest[0], std::vector<std::wstring>(rest.begin() + 1, rest.end()));
    }
    if (cmd == L"capture") {
        if (rest.empty()) {
            std::cerr << "ozbench: capture requires a plan file\n";
            return 1;
        }
        return cmd_capture(rest[0], std::vector<std::wstring>(rest.begin() + 1, rest.end()));
    }
    if (cmd == L"summarize") {
        if (rest.empty()) {
            std::cerr << "ozbench: summarize requires an input JSONL file\n";
            return 1;
        }
        return cmd_summarize(rest[0], std::vector<std::wstring>(rest.begin() + 1, rest.end()));
    }
    if (cmd == L"import") {
        if (rest.empty()) {
            std::cerr << "ozbench: import requires an input file\n";
            return 1;
        }
        return cmd_import(rest[0], std::vector<std::wstring>(rest.begin() + 1, rest.end()));
    }

    std::cerr << "ozbench: unknown command: " << wide_to_utf8(cmd) << "\n";
    print_usage();
    return 1;
}
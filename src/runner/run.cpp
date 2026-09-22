#include "oz/runner/run.hpp"

#include "oz/runner/collect.hpp"
#include "oz/runner/process.hpp"
#include "oz/runner/utf.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <cstdlib>

namespace oz::runner {
namespace {

image_info to_image_info(const image_spec& s) {
    image_info info;
    info.path = s.path;
    info.format = s.format;
    info.width = s.width;
    info.height = s.height;
    info.size_bytes = s.size_bytes;
    info.provenance = s.provenance;
    return info;
}

std::string substitute(std::string s, const std::map<std::string, std::string>& vars) {
    for (const auto& [k, v] : vars) {
        const std::string key = "{{" + k + "}}";
        std::size_t pos = 0;
        while ((pos = s.find(key, pos)) != std::string::npos) {
            s.replace(pos, key.size(), v);
            pos += v.size();
        }
    }
    return s;
}

// Lowercase hex SHA-256 of the file at the given UTF-8 path, or an empty
// string when the file cannot be opened/read/hashed. Used for subject
// provenance; a missing hash is reported as null in the JSONL output.
std::string sha256_file_hex(const std::string& utf8_path) {
    const std::wstring wpath = utf8_to_wide(utf8_path);
    if (wpath.empty()) {
        return {};
    }
    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return {};
    }
    struct file_guard {
        HANDLE h;
        ~file_guard() {
            if (h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
            }
        }
    } fg{h};

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    struct alg_guard {
        BCRYPT_ALG_HANDLE a;
        ~alg_guard() {
            if (a) {
                BCryptCloseAlgorithmProvider(a, 0);
            }
        }
    } ag{alg};

    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
        return {};
    }
    struct hash_guard {
        BCRYPT_HASH_HANDLE hh;
        ~hash_guard() {
            if (hh) {
                BCryptDestroyHash(hh);
            }
        }
    } hg{hash};

    char buf[64 * 1024];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) {
        if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf), n, 0) != 0) {
            return {};
        }
    }
    BYTE digest[32];
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0) {
        return {};
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const BYTE b : digest) {
        out += kHex[b >> 4];
        out += kHex[b & 0xF];
    }
    return out;
}

}  // namespace

std::vector<record> run_plan(const plan& p, const run_options& opts, std::string& error) {
    std::vector<record> out;
    if (!p.subject.enabled) {
        error = "subject disabled: " + p.subject.skip_reason;
        return out;
    }
    if (p.collector == "external_visible_frame") {
        // The reference endpoint is measured by the dedicated capture command;
        // `run` must never silently fall back to marker collection.
        error = "collector 'external_visible_frame' requires 'ozbench capture'";
        return out;
    }

    // Validate counts before any file I/O so out-of-range overrides fail
    // fast instead of starting a pathological run loop.
    const std::uint64_t reps = opts.repetitions.value_or(p.repetitions);
    const std::uint64_t warmups = opts.warmups.value_or(p.warmups);
    if (reps < 1 || reps > kMaxRepetitions) {
        error = "repetitions out of range [1, " + std::to_string(kMaxRepetitions) + "]";
        return out;
    }
    if (warmups > kMaxWarmups) {
        error = "warmups out of range [0, " + std::to_string(kMaxWarmups) + "]";
        return out;
    }
    const std::uint64_t total_runs = warmups + reps;

    std::map<std::string, std::string> vars = opts.defines;
    if (const char* env = std::getenv("OZBENCH_FIXTURE_EXE")) {
        vars.try_emplace("fixture_exe", env);
    }

    // Resolve the executable before the availability check so template plans
    // (e.g. {{fixture_exe}}) work with --set / OZBENCH_FIXTURE_EXE.
    const std::string resolved_exe = substitute(p.subject.executable, vars);
    {
        plan resolved = p;
        resolved.subject.executable = resolved_exe;
        if (!subject_available(resolved)) {
            error = "subject executable not found: " + resolved_exe;
            return out;
        }
    }

    // Subject provenance: hash the resolved executable once per plan run.
    const std::string exe_sha256 = sha256_file_hex(resolved_exe);

    for (const auto& img : p.images) {
        const std::string scenario = scenario_key(p, img);
        if (opts.scenario_filter &&
            scenario.find(*opts.scenario_filter) == std::string::npos) {
            continue;
        }
        vars["image_path"] = img.path;

        const std::wstring exe = utf8_to_wide(resolved_exe);
        std::vector<std::wstring> args;
        for (const auto& a : p.args) {
            args.push_back(utf8_to_wide(substitute(a, vars)));
        }
        std::optional<std::wstring> workdir;
        if (p.working_directory) {
            workdir = utf8_to_wide(substitute(*p.working_directory, vars));
        }

        for (std::uint64_t i = 0; i < total_runs; ++i) {
            const bool warmup = i < warmups;
            const std::int64_t repetition = static_cast<std::int64_t>(i);

            record rs;
            rs.kind = record_kind::run_start;
            rs.scenario = scenario;
            rs.cache = p.cache;
            rs.repetition = repetition;
            rs.warmup = warmup;
            rs.subject = p.subject.name;
            rs.subject_path = resolved_exe;
            rs.subject_version = p.subject.version_hint;
            rs.subject_sha256 = exe_sha256;
            rs.collector = p.collector;
            rs.adapter = p.adapter;
            rs.image = to_image_info(img);
            rs.started_utc = utc_now_iso8601();
            out.push_back(std::move(rs));

            process_options po;
            po.executable = exe;
            po.args = args;
            po.working_directory = workdir;
            po.timeout_ms = p.timeout_ms;
            po.capture_stdout = (p.collector == "markers");
            const process_result pr = launch_process(po);

            record re;
            re.kind = record_kind::run_end;
            re.scenario = scenario;
            re.cache = p.cache;
            re.repetition = repetition;
            re.warmup = warmup;
            re.subject = p.subject.name;
            re.subject_path = resolved_exe;
            re.subject_version = p.subject.version_hint;
            re.subject_sha256 = exe_sha256;
            re.collector = p.collector;
            re.adapter = p.adapter;
            re.image = to_image_info(img);
            re.launched = pr.launched;
            re.error = pr.error;
            re.error_message = pr.error_message;
            re.duration_ns = pr.duration_ns;
            re.timed_out = pr.timed_out;
            if (pr.launched) {
                re.exit_code = pr.exit_code;
            }
            if (!pr.launched) {
                re.issues.push_back("launch failed: " + pr.error_message + " (error " +
                                    std::to_string(pr.error) + ")");
            }

            if (p.collector == "markers") {
                const collected_run cr = parse_marker_stream(pr.stdout_text);
                for (const auto& m : cr.markers) {
                    record mr;
                    mr.kind = record_kind::marker;
                    mr.scenario = scenario;
                    mr.cache = p.cache;
                    mr.repetition = repetition;
                    mr.warmup = warmup;
                    mr.subject = p.subject.name;
                    mr.subject_path = resolved_exe;
                    mr.subject_version = p.subject.version_hint;
                    mr.subject_sha256 = exe_sha256;
                    mr.collector = p.collector;
                    mr.adapter = p.adapter;
                    mr.image = to_image_info(img);
                    mr.marker = std::string(markers::marker_name(m.id));
                    mr.raw_ticks = m.raw_ticks;
                    mr.relative_ns = m.relative_ns;
                    mr.thread_id = m.thread_id;
                    out.push_back(std::move(mr));
                }
                re.validation = cr.validation;
                re.issues = cr.runner_issues;
            } else {
                re.issues.push_back(
                    "collector 'external_first_present': first-present samples must be "
                    "collected via the external protocol and imported with 'ozbench import'");
            }
            out.push_back(std::move(re));
        }
    }
    return out;
}

}  // namespace oz::runner
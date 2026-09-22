// navigation_benchmark_tests - authoritative focused tests for the
// test-only navigation benchmark tooling (schema oz.navigation-benchmark.v1):
// - CLI/options bounds and shared validation (parser == core limits)
// - pure navigation-intent policy (expected index, outcome classification)
// - privacy redaction, schema serialization and JSONL stream validation
// - create-new output handling (race-safe open_benchmark_output)
// - nearest-rank percentile calculations and summary aggregation (incl. no_op)
// - session accounting: only a real, still-current outstanding request may
//   produce a latency sample; unmatched/duplicate/superseded presents never do
// - bounded watchdog: request timeout and unsatisfied setup/preload waits are
//   recorded and end the run as timed_out
// - scenario isolation: explicit labeled setup/re-arm phases, including the
//   former --repetitions 500 boundary contamination regression
// - sample eligibility: pure cached-hit and 24MP-miss sets, ineligible presents
//   audited but excluded from latency statistics
// - rapid supersession: a superseded target never contributes after a newer
//   request; the final still-current target presents successfully
// - no-op steps (empty/single-item lists, boundaries) complete instead of hanging
// - corpus validation negatives and (TurboJPEG builds) an isolated smoke corpus
#include "oz/viewer_core/navigation_benchmark.hpp"
#include "oz/viewer_core/navigation_benchmark_corpus.hpp"
#include "oz/viewer_core/navigation_benchmark_policy.hpp"
#include "oz/viewer_core/navigation_benchmark_schema.hpp"
#include "oz/collector/manifest.hpp"
#include "oz/runner/json.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace test {
int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
    }
}
}  // namespace test

#define CHECK(expr) ::test::check(!!(expr), #expr, __FILE__, __LINE__)

namespace {

std::filesystem::path unique_temp_dir(const char* tag) {
    static unsigned counter = 0;
    const auto ticks = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto dir = std::filesystem::temp_directory_path() /
                     (std::string("oz_nav_bench_") + tag + "_" + std::to_string(ticks) + "_" +
                      std::to_string(counter++));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::vector<oz::json::value> parse_records(const std::filesystem::path& path) {
    std::vector<oz::json::value> records;
    std::ifstream f(path, std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        auto parsed = oz::json::parse(line);
        if (parsed) {
            records.push_back(std::move(*parsed));
        }
    }
    return records;
}

const oz::json::value* find_record(const std::vector<oz::json::value>& records,
                                   std::string_view type,
                                   std::string_view scenario = std::string_view{}) {
    for (const auto& r : records) {
        if (oz::json::get_string(r, "record_type").value_or("") != type) {
            continue;
        }
        if (!scenario.empty() &&
            oz::json::get_string(r, "scenario").value_or("") != scenario) {
            continue;
        }
        return &r;
    }
    return nullptr;
}

oz::viewer_core::corpus_validation_result fake_corpus() {
    oz::viewer_core::corpus_validation_result c;
    c.ok = true;
    c.count = 0;
    c.manifest_sha256 = "test-manifest-hash";
    return c;
}

bool init_session(oz::viewer_core::navigation_benchmark_session& session,
                  const std::filesystem::path& out,
                  std::uint32_t timeout_ms,
                  std::uint32_t repetitions,
                  std::uint32_t rapid_burst,
                  const oz::viewer_core::corpus_validation_result* corpus) {
    oz::viewer_core::nav_bench_options opts;
    opts.enabled = true;
    opts.corpus_dir = "fake-corpus";
    opts.output_path = out;
    opts.overwrite = true;
    opts.timeout_ms = timeout_ms;
    opts.repetitions = repetitions;
    opts.rapid_burst = rapid_burst;
    return session.init(opts, 1024, 768, 96.0f, false, corpus) ==
           oz::viewer_core::nav_bench_status::success;
}

// Corpus with populated manifest entries (ordinal order matches the navigation
// list order) for state-machine tests that need real widths/heights.
oz::viewer_core::corpus_validation_result corpus_with_entries(std::size_t count,
                                                              std::size_t heavy_24mp = 50) {
    oz::viewer_core::corpus_validation_result c;
    c.ok = true;
    c.count = count;
    c.manifest_sha256 = "test-manifest-hash";
    c.entries.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        oz::viewer_core::corpus_manifest_entry e;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "img_%04zu", i);
        e.alias = buf;
        e.ordinal = i;
        e.rel_filename = e.alias + ".jpg";
        if (i < heavy_24mp) {
            e.width = 6000;
            e.height = 4000;
        } else if (i < 150) {
            e.width = 4240;
            e.height = 2832;
        } else {
            e.width = 3000;
            e.height = 2000;
        }
        e.sha256 = std::string(64, 'a');
        c.entries.push_back(std::move(e));
    }
    return c;
}

oz::viewer_core::bench_nav_direction direction_for_vk(int vk_key) {
    using oz::viewer_core::bench_nav_direction;
    switch (vk_key) {
        case VK_LEFT:
        case VK_BACK: return bench_nav_direction::backward;
        case VK_RIGHT:
        case VK_SPACE: return bench_nav_direction::forward;
        case VK_HOME: return bench_nav_direction::home;
        case VK_END: return bench_nav_direction::end;
        default: return bench_nav_direction::none;
    }
}

// Deterministic viewer simulator for full state-machine runs: it executes
// setup actions exactly as the UI integration does (re-arm commits, purge for
// verified misses, readiness reporting) and resolves measured requests through
// the same shared request/present API.
struct sim_result {
    std::size_t no_ops = 0;
    std::size_t navigation_requests = 0;
    std::size_t rearm_actions = 0;
    std::size_t purge_actions = 0;
    std::size_t cached_requests = 0;
    std::size_t cached_hit_requests = 0;
    std::size_t cached_miss_requests = 0;
    std::size_t uncached_requests = 0;
    std::size_t uncached_miss_requests = 0;
    std::size_t uncached_bad_targets = 0;
    std::size_t rapid_requests = 0;
    std::size_t rapid_superseded = 0;
    std::size_t forward_back_requests = 0;
    std::size_t memory_requests = 0;
    std::size_t invalid_rearm_actions = 0;
    std::vector<std::size_t> rearm_indices;
};

sim_result drive_full_session(oz::viewer_core::navigation_benchmark_session& session,
                              const oz::viewer_core::corpus_validation_result& corpus,
                              std::size_t list_size,
                              std::size_t max_ticks = 100000) {
    using namespace oz::viewer_core;
    sim_result r;
    std::size_t committed = 0;
    std::uint64_t epoch = 0;
    std::uint64_t rapid_outstanding = 0;
    std::size_t rapid_outstanding_target = 0;
    bool target_present_for_last_await = false;

    auto alias_for = [&](std::size_t index) {
        return index < corpus.entries.size() ? corpus.entries[index].alias
                                             : std::string{};
    };
    auto present = [&](std::uint64_t request_id, std::size_t target) {
        session.record_successful_present(request_id, session.query_ticks(), target,
                                          alias_for(target), "sha-test", 0, 0, 0, 0);
        committed = target;
    };

    for (std::size_t tick = 0; tick < max_ticks && session.is_active(); ++tick) {
        if (session.step_pending()) {
            // Rapid-burst settle: resolve the newest outstanding request.
            if (rapid_outstanding != 0) {
                present(rapid_outstanding, rapid_outstanding_target);
                rapid_outstanding = 0;
            }
            continue;
        }

        nav_bench_viewer_state viewer_state;
        viewer_state.list_size = list_size;
        viewer_state.committed_index = committed;
        viewer_state.work_drained = true;
        viewer_state.target_present = target_present_for_last_await;
        const nav_bench_step step = session.pump_benchmark_step(viewer_state);

        switch (step.kind) {
            case nav_bench_step_kind::idle:
            case nav_bench_step_kind::await_drain:
                break;
            case nav_bench_step_kind::rearm:
                ++r.rearm_actions;
                r.rearm_indices.push_back(step.target_index);
                if (step.target_index >= list_size) {
                    ++r.invalid_rearm_actions;
                }
                committed = step.target_index;  // simulated immediate commit
                target_present_for_last_await = false;
                break;
            case nav_bench_step_kind::await_cache:
                if (step.purge_app_cache) {
                    ++r.purge_actions;
                }
                // Viewer satisfies the requested residency: cached_next waits
                // for a resident preload; uncached verifies the post-purge miss.
                target_present_for_last_await = step.expect_target_present;
                break;
            case nav_bench_step_kind::navigation: {
                const auto target_opt = expected_target_index(
                    direction_for_vk(step.vk_key), committed, list_size);
                if (!target_opt.has_value()) {
                    session.note_step_no_op(step.scenario, "no_op",
                                            "navigation_no_op_at_boundary");
                    ++r.no_ops;
                    break;
                }
                const std::size_t target = *target_opt;
                ++r.navigation_requests;
                ++epoch;

                const bool cached_scenario =
                    step.scenario == kNavBenchScenarioCachedNext;
                const bool uncached_scenario =
                    step.scenario == kNavBenchScenarioUncached24MpNext;
                const bench_cache_state cache_state =
                    cached_scenario ? bench_cache_state::hit_at_request
                                    : bench_cache_state::miss_at_request;
                const std::uint64_t request_id = session.record_navigation_request(
                    step.scenario, std::string(to_string(direction_for_vk(step.vk_key))),
                    target, alias_for(target), committed, alias_for(committed),
                    cache_state, epoch);

                if (cached_scenario) {
                    ++r.cached_requests;
                    if (cache_state == bench_cache_state::hit_at_request) {
                        ++r.cached_hit_requests;
                    } else {
                        ++r.cached_miss_requests;
                    }
                }
                if (uncached_scenario) {
                    ++r.uncached_requests;
                    if (cache_state == bench_cache_state::miss_at_request) {
                        ++r.uncached_miss_requests;
                    }
                    if (target >= corpus.entries.size() ||
                        !nav_bench_is_24mp_image(corpus.entries[target].width,
                                                 corpus.entries[target].height)) {
                        ++r.uncached_bad_targets;
                    }
                }
                if (step.scenario == kNavBenchScenarioRapidReplacement) {
                    ++r.rapid_requests;
                    if (rapid_outstanding != 0) {
                        session.record_request_superseded(
                            rapid_outstanding, epoch - 1,
                            "superseded_by_newer_navigation");
                        ++r.rapid_superseded;
                    }
                    rapid_outstanding = request_id;
                    rapid_outstanding_target = target;
                    break;  // no present during the burst
                }
                if (step.scenario == kNavBenchScenarioForwardBackSweep) {
                    ++r.forward_back_requests;
                }
                if (step.scenario == kNavBenchScenarioCacheMemorySweep) {
                    ++r.memory_requests;
                }
                present(request_id, target);
                break;
            }
            case nav_bench_step_kind::finished:
                session.finish("completed", "");
                return r;
        }
    }
    return r;
}

// --- CLI parsing and shared option validation -------------------------------

void test_cli_parsing_and_bounds() {
    oz::viewer_core::nav_bench_options opts;
    std::string err;

    // Normal valid invocation
    wchar_t* argv_valid[] = {
        const_cast<wchar_t*>(L"ozviewer.exe"),
        const_cast<wchar_t*>(L"--navigation-benchmark"),
        const_cast<wchar_t*>(L"--corpus"), const_cast<wchar_t*>(L"my_corpus"),
        const_cast<wchar_t*>(L"--out"), const_cast<wchar_t*>(L"results.jsonl"),
        const_cast<wchar_t*>(L"--overwrite"),
        const_cast<wchar_t*>(L"--os-cache"), const_cast<wchar_t*>(L"cold"),
        const_cast<wchar_t*>(L"--timeout-ms"), const_cast<wchar_t*>(L"12000"),
        const_cast<wchar_t*>(L"--repetitions"), const_cast<wchar_t*>(L"10"),
        const_cast<wchar_t*>(L"--rapid-burst"), const_cast<wchar_t*>(L"8")
    };
    CHECK(oz::viewer_core::parse_navigation_benchmark_args(15, argv_valid, opts, err));
    CHECK(opts.enabled);
    CHECK(opts.corpus_dir == L"my_corpus");
    CHECK(opts.output_path == L"results.jsonl");
    CHECK(opts.overwrite == true);
    CHECK(opts.os_cache_condition == "cold_protocol");
    CHECK(opts.timeout_ms == 12000);
    CHECK(opts.repetitions == 10);
    CHECK(opts.rapid_burst == 8);

    // Maximum accepted values are the shared policy constants.
    wchar_t* argv_max[] = {
        const_cast<wchar_t*>(L"ozviewer.exe"),
        const_cast<wchar_t*>(L"--navigation-benchmark"),
        const_cast<wchar_t*>(L"--corpus"), const_cast<wchar_t*>(L"my_corpus"),
        const_cast<wchar_t*>(L"--out"), const_cast<wchar_t*>(L"results.jsonl"),
        const_cast<wchar_t*>(L"--repetitions"), const_cast<wchar_t*>(L"1000"),
        const_cast<wchar_t*>(L"--rapid-burst"), const_cast<wchar_t*>(L"100")
    };
    CHECK(oz::viewer_core::parse_navigation_benchmark_args(10, argv_max, opts, err));
    CHECK(opts.repetitions == oz::viewer_core::kNavBenchMaxRepetitions);
    CHECK(opts.rapid_burst == oz::viewer_core::kNavBenchMaxRapidBurst);

    // Missing corpus
    wchar_t* argv_no_corpus[] = {
        const_cast<wchar_t*>(L"ozviewer.exe"),
        const_cast<wchar_t*>(L"--navigation-benchmark"),
        const_cast<wchar_t*>(L"--out"), const_cast<wchar_t*>(L"results.jsonl")
    };
    CHECK(!oz::viewer_core::parse_navigation_benchmark_args(4, argv_no_corpus, opts, err));
    CHECK(!err.empty());

    // Missing output
    wchar_t* argv_no_out[] = {
        const_cast<wchar_t*>(L"ozviewer.exe"),
        const_cast<wchar_t*>(L"--navigation-benchmark"),
        const_cast<wchar_t*>(L"--corpus"), const_cast<wchar_t*>(L"my_corpus")
    };
    CHECK(!oz::viewer_core::parse_navigation_benchmark_args(4, argv_no_out, opts, err));
    CHECK(!err.empty());

    // Zero repetitions rejected
    wchar_t* argv_zero_rep[] = {
        const_cast<wchar_t*>(L"ozviewer.exe"),
        const_cast<wchar_t*>(L"--navigation-benchmark"),
        const_cast<wchar_t*>(L"--corpus"), const_cast<wchar_t*>(L"my_corpus"),
        const_cast<wchar_t*>(L"--out"), const_cast<wchar_t*>(L"results.jsonl"),
        const_cast<wchar_t*>(L"--repetitions"), const_cast<wchar_t*>(L"0")
    };
    CHECK(!oz::viewer_core::parse_navigation_benchmark_args(7, argv_zero_rep, opts, err));

    // Repetitions above the shared maximum rejected
    wchar_t* argv_big_rep[] = {
        const_cast<wchar_t*>(L"ozviewer.exe"),
        const_cast<wchar_t*>(L"--navigation-benchmark"),
        const_cast<wchar_t*>(L"--corpus"), const_cast<wchar_t*>(L"my_corpus"),
        const_cast<wchar_t*>(L"--out"), const_cast<wchar_t*>(L"results.jsonl"),
        const_cast<wchar_t*>(L"--repetitions"), const_cast<wchar_t*>(L"1001")
    };
    CHECK(!oz::viewer_core::parse_navigation_benchmark_args(7, argv_big_rep, opts, err));
    CHECK(err.find("repetitions") != std::string::npos);

    // Rapid burst above the shared maximum rejected
    wchar_t* argv_big_burst[] = {
        const_cast<wchar_t*>(L"ozviewer.exe"),
        const_cast<wchar_t*>(L"--navigation-benchmark"),
        const_cast<wchar_t*>(L"--corpus"), const_cast<wchar_t*>(L"my_corpus"),
        const_cast<wchar_t*>(L"--out"), const_cast<wchar_t*>(L"results.jsonl"),
        const_cast<wchar_t*>(L"--rapid-burst"), const_cast<wchar_t*>(L"101")
    };
    CHECK(!oz::viewer_core::parse_navigation_benchmark_args(7, argv_big_burst, opts, err));

    // Timeout out of bounds rejected
    wchar_t* argv_bad_timeout[] = {
        const_cast<wchar_t*>(L"ozviewer.exe"),
        const_cast<wchar_t*>(L"--navigation-benchmark"),
        const_cast<wchar_t*>(L"--corpus"), const_cast<wchar_t*>(L"my_corpus"),
        const_cast<wchar_t*>(L"--out"), const_cast<wchar_t*>(L"results.jsonl"),
        const_cast<wchar_t*>(L"--timeout-ms"), const_cast<wchar_t*>(L"0")
    };
    CHECK(!oz::viewer_core::parse_navigation_benchmark_args(7, argv_bad_timeout, opts, err));

    // Unknown cache condition rejected
    wchar_t* argv_bad_cache[] = {
        const_cast<wchar_t*>(L"ozviewer.exe"),
        const_cast<wchar_t*>(L"--navigation-benchmark"),
        const_cast<wchar_t*>(L"--corpus"), const_cast<wchar_t*>(L"my_corpus"),
        const_cast<wchar_t*>(L"--out"), const_cast<wchar_t*>(L"results.jsonl"),
        const_cast<wchar_t*>(L"--os-cache"), const_cast<wchar_t*>(L"lukewarm")
    };
    CHECK(!oz::viewer_core::parse_navigation_benchmark_args(7, argv_bad_cache, opts, err));

    // The removed in-session relocation option must not be accepted: relocation
    // is an external, cross-launch procedure documented in navigation-benchmark.md.
    wchar_t* argv_relocation[] = {
        const_cast<wchar_t*>(L"ozviewer.exe"),
        const_cast<wchar_t*>(L"--navigation-benchmark"),
        const_cast<wchar_t*>(L"--corpus"), const_cast<wchar_t*>(L"my_corpus"),
        const_cast<wchar_t*>(L"--out"), const_cast<wchar_t*>(L"results.jsonl"),
        const_cast<wchar_t*>(L"--relocation-dir"), const_cast<wchar_t*>(L"elsewhere")
    };
    CHECK(!oz::viewer_core::parse_navigation_benchmark_args(8, argv_relocation, opts, err));

    // Non navigation benchmark returns false (not an error, standard launch)
    wchar_t* argv_regular[] = {
        const_cast<wchar_t*>(L"ozviewer.exe"),
        const_cast<wchar_t*>(L"photo.jpg")
    };
    CHECK(!oz::viewer_core::parse_navigation_benchmark_args(2, argv_regular, opts, err));
    CHECK(!opts.enabled);
}

void test_shared_option_validation() {
    oz::viewer_core::nav_bench_options opts;
    opts.corpus_dir = "corpus";
    opts.output_path = "out.jsonl";
    CHECK(oz::viewer_core::validate_nav_bench_options(opts).empty());

    opts.corpus_dir.clear();
    CHECK(!oz::viewer_core::validate_nav_bench_options(opts).empty());
    opts.corpus_dir = "corpus";

    opts.output_path.clear();
    CHECK(!oz::viewer_core::validate_nav_bench_options(opts).empty());
    opts.output_path = "out.jsonl";

    opts.os_cache_condition = "lukewarm";
    CHECK(!oz::viewer_core::validate_nav_bench_options(opts).empty());
    opts.os_cache_condition = "uncontrolled";

    opts.repetitions = 0;
    CHECK(!oz::viewer_core::validate_nav_bench_options(opts).empty());
    opts.repetitions = oz::viewer_core::kNavBenchMaxRepetitions + 1;
    CHECK(!oz::viewer_core::validate_nav_bench_options(opts).empty());
    opts.repetitions = 5;

    opts.rapid_burst = 0;
    CHECK(!oz::viewer_core::validate_nav_bench_options(opts).empty());
    opts.rapid_burst = oz::viewer_core::kNavBenchMaxRapidBurst + 1;
    CHECK(!oz::viewer_core::validate_nav_bench_options(opts).empty());
    opts.rapid_burst = 4;

    opts.timeout_ms = 0;
    CHECK(!oz::viewer_core::validate_nav_bench_options(opts).empty());
    opts.timeout_ms = oz::viewer_core::kNavBenchMaxTimeoutMs + 1;
    CHECK(!oz::viewer_core::validate_nav_bench_options(opts).empty());
    opts.timeout_ms = 1000;
    CHECK(oz::viewer_core::validate_nav_bench_options(opts).empty());
}

// --- Pure policy ------------------------------------------------------------

void test_navigation_intent_policy() {
    using oz::viewer_core::bench_nav_direction;
    using oz::viewer_core::expected_target_index;

    CHECK(expected_target_index(bench_nav_direction::forward, 0, 5) == 1);
    CHECK(expected_target_index(bench_nav_direction::forward, 4, 5) == std::nullopt);
    CHECK(expected_target_index(bench_nav_direction::backward, 0, 5) == std::nullopt);
    CHECK(expected_target_index(bench_nav_direction::backward, 3, 5) == 2);
    CHECK(expected_target_index(bench_nav_direction::home, 2, 5) == 0);
    CHECK(expected_target_index(bench_nav_direction::home, 0, 5) == std::nullopt);
    CHECK(expected_target_index(bench_nav_direction::end, 2, 5) == 4);
    CHECK(expected_target_index(bench_nav_direction::end, 4, 5) == std::nullopt);
    CHECK(expected_target_index(bench_nav_direction::none, 2, 5) == std::nullopt);
    // Empty list and invalid current index are no-ops.
    CHECK(expected_target_index(bench_nav_direction::forward, 0, 0) == std::nullopt);
    CHECK(expected_target_index(bench_nav_direction::forward, 5, 5) == std::nullopt);

    CHECK(oz::viewer_core::to_string(bench_nav_direction::forward) == "forward");
    CHECK(oz::viewer_core::to_string(bench_nav_direction::backward) == "backward");
    CHECK(oz::viewer_core::to_string(bench_nav_direction::home) == "home");
    CHECK(oz::viewer_core::to_string(bench_nav_direction::end) == "end");
    CHECK(oz::viewer_core::to_string(bench_nav_direction::none) == "none");
}

void test_outcome_classification_policy() {
    using oz::viewer_core::bench_outcome;
    using oz::viewer_core::classify_request_outcome;
    using oz::viewer_core::is_latency_accepted;

    CHECK(classify_request_outcome(5, 5, true, false, false, false) == bench_outcome::success);
    CHECK(classify_request_outcome(4, 5, true, false, false, false) == bench_outcome::superseded);
    CHECK(classify_request_outcome(5, 5, false, true, false, false) == bench_outcome::failed);
    CHECK(classify_request_outcome(5, 5, false, false, true, false) == bench_outcome::timed_out);
    CHECK(classify_request_outcome(5, 5, true, false, false, true) == bench_outcome::no_op);
    CHECK(classify_request_outcome(5, 5, false, false, false, false) == bench_outcome::failed);

    CHECK(is_latency_accepted(bench_outcome::success));
    CHECK(!is_latency_accepted(bench_outcome::superseded));
    CHECK(!is_latency_accepted(bench_outcome::failed));
    CHECK(!is_latency_accepted(bench_outcome::timed_out));
    CHECK(!is_latency_accepted(bench_outcome::no_op));
}

// --- Privacy / schema serialization / JSONL validation ----------------------

void test_privacy_redaction_and_schema_validation() {
    CHECK(oz::viewer_core::contains_private_path_data("C:\\sensitive\\user\\image.jpg"));
    CHECK(oz::viewer_core::contains_private_path_data("E:\\Private\\data.jpg"));
    CHECK(oz::viewer_core::contains_private_path_data("\\\\server\\share\\photo.jpg"));
    CHECK(oz::viewer_core::contains_private_path_data("/Users/admin/test.jpg"));
    CHECK(!oz::viewer_core::contains_private_path_data("img_0042"));
    CHECK(!oz::viewer_core::contains_private_path_data("img_0042.jpg"));
    CHECK(!oz::viewer_core::contains_private_path_data("uncached_24mp_next"));

    // Schema structure for run_start
    auto rs = oz::viewer_core::serialize_run_start("run_test_1", "sha256dummy", 500, 1920, 1080, 144.0f, true);
    CHECK(oz::json::get_string(rs, "schema").value_or("") == "oz.navigation-benchmark.v1");
    CHECK(oz::json::get_string(rs, "record_type").value_or("") == "run_start");
    CHECK(oz::json::get_string(rs, "run_id").value_or("") == "run_test_1");
    CHECK(oz::json::get_int(rs, "corpus_image_count").value_or(0) == 500);
    CHECK(oz::json::get_int(rs, "viewport_width").value_or(0) == 1920);
    CHECK(oz::json::get_int(rs, "viewport_height").value_or(0) == 1080);
    CHECK(oz::json::get_bool(rs, "warp").value_or(false) == true);

    // navigation_request carries both cache dimensions separately
    auto req = oz::viewer_core::serialize_navigation_request(
        "run-1", "cached_next", "cold_protocol", 1, 10, "forward", 15, "img_0015.jpg",
        14, "img_0014.jpg", oz::viewer_core::bench_cache_state::hit_at_request, 1000, 500);
    CHECK(oz::json::get_string(req, "record_type").value_or("") == "navigation_request");
    CHECK(oz::json::get_string(req, "app_cache_state").value_or("") == "hit_at_request");
    CHECK(oz::json::get_string(req, "os_cache_condition").value_or("") == "cold_protocol");
    CHECK(oz::json::get_string(req, "direction").value_or("") == "forward");
    CHECK(oz::json::get_int(req, "requested_target_index").value_or(0) == 15);

    // successful_present fields, including distinct cache entry/resident counts
    auto pres = oz::viewer_core::serialize_successful_present(
        "run-1", "cached_next", "cold_protocol", 1, 10, 15, "img_0015.jpg", "sha256dummy",
        oz::viewer_core::bench_cache_state::hit_at_request,
        1000, 1500, 500000,
        512ull * 1024 * 1024, 64ull * 1024 * 1024, 4, 2);
    CHECK(oz::json::get_string(pres, "record_type").value_or("") == "successful_present");
    CHECK(oz::json::get_int(pres, "committed_index").value_or(0) == 15);
    CHECK(oz::json::get_int(pres, "latency_ns").value_or(0) == 500000);
    CHECK(oz::json::get_int(pres, "cache_entry_count").value_or(0) == 4);
    CHECK(oz::json::get_int(pres, "resident_decoded_count").value_or(0) == 2);

    // task_event structure
    auto ev = oz::viewer_core::serialize_task_event("run_test_1", "rapid_replacement", 42, 5, "superseded", "superseded", "replaced", 123456);
    CHECK(oz::json::get_string(ev, "record_type").value_or("") == "task_event");
    CHECK(oz::json::get_string(ev, "event_type").value_or("") == "superseded");
    CHECK(oz::json::get_string(ev, "reason").value_or("") == "replaced");
    CHECK(oz::json::get_int(ev, "request_id").value_or(0) == 42);

    // memory_sample keeps entry count and resident decoded count distinct
    oz::viewer_core::bench_memory_sample ms;
    ms.raw_ticks = 1000;
    ms.relative_ns = 50000;
    ms.private_bytes = 1024 * 1024 * 30;
    ms.working_set_bytes = 1024 * 1024 * 40;
    ms.peak_working_set_bytes = 1024 * 1024 * 45;
    ms.cache_used_bytes = 1024 * 1024 * 10;
    ms.cache_budget_bytes = 1024 * 1024 * 128;
    ms.cache_entry_count = 3;
    ms.resident_decoded_count = 2;

    auto ms_val = oz::viewer_core::serialize_memory_sample("run_test_1", "post_burst", ms);
    CHECK(oz::json::get_string(ms_val, "record_type").value_or("") == "memory_sample");
    CHECK(oz::json::get_string(ms_val, "phase").value_or("") == "post_burst");
    CHECK(oz::json::get_int(ms_val, "cache_entry_count").value_or(0) == 3);
    CHECK(oz::json::get_int(ms_val, "resident_decoded_count").value_or(0) == 2);
    CHECK(oz::json::get_int(ms_val, "private_bytes").value_or(0) == 1024 * 1024 * 30);
}

void test_jsonl_stream_validation() {
    // Valid stream
    std::string good;
    good += oz::json::dump(oz::viewer_core::serialize_run_start("r", "h", 0, 1, 1, 96.0f, false)) + "\n";
    good += oz::json::dump(oz::viewer_core::serialize_run_end(
                "r", "completed", "", 1, 1, 0, 0, 0, 0, 0, 0)) + "\n";
    auto v = oz::viewer_core::validate_navigation_jsonl(good);
    CHECK(v.ok);
    CHECK(v.records == 2);
    CHECK(v.kinds.size() == 2);
    CHECK(v.kinds[0] == oz::viewer_core::bench_record_kind::run_start);
    CHECK(v.kinds[1] == oz::viewer_core::bench_record_kind::run_end);

    // Malformed JSON
    CHECK(!oz::viewer_core::validate_navigation_jsonl("{not json}\n").ok);

    // Wrong schema
    auto wrong_schema = oz::json::make_object();
    oz::json::set_member(wrong_schema, "schema", oz::json::make_string("other.v1"));
    oz::json::set_member(wrong_schema, "record_type", oz::json::make_string("run_end"));
    CHECK(!oz::viewer_core::validate_navigation_jsonl(oz::json::dump(wrong_schema) + "\n").ok);

    // Private path data anywhere in a record
    std::string private_stream =
        oz::json::dump(oz::viewer_core::serialize_task_event(
            "r", "cached_next", 1, 1, "failed", "failed", "C:\\Users\\Bob\\secret.jpg", 1)) +
        "\n";
    CHECK(!oz::viewer_core::validate_navigation_jsonl(private_stream).ok);

    // Pixel payload members are rejected
    auto pixel_rec = oz::viewer_core::serialize_task_event("r", "s", 1, 1, "failed", "failed", "x", 1);
    oz::json::set_member(pixel_rec, "pixels", oz::json::make_string("AAAA"));
    CHECK(!oz::viewer_core::validate_navigation_jsonl(oz::json::dump(pixel_rec) + "\n").ok);

    // Empty stream
    CHECK(!oz::viewer_core::validate_navigation_jsonl("").ok);
}

void test_open_benchmark_output_create_new() {
    const auto dir = unique_temp_dir("output");
    const auto out = dir / "run.jsonl";

    // create-new succeeds for a fresh path
    {
        std::ofstream a;
        CHECK(oz::viewer_core::open_benchmark_output(a, out, false).empty());
        CHECK(a.is_open());
        a << "first\n";
    }

    // refusing to truncate an existing file is an explicit error
    {
        std::ofstream b;
        const std::string err = oz::viewer_core::open_benchmark_output(b, out, false);
        CHECK(!err.empty());
        CHECK(!b.is_open());
        CHECK(read_text(out) == "first\n");  // existing content preserved
    }

    // explicit overwrite truncates
    {
        std::ofstream c;
        CHECK(oz::viewer_core::open_benchmark_output(c, out, true).empty());
        c << "second\n";
    }
    CHECK(read_text(out) == "second\n");

    // an already-open stream is rejected
    {
        std::ofstream d;
        CHECK(oz::viewer_core::open_benchmark_output(d, out, true).empty());
        CHECK(!oz::viewer_core::open_benchmark_output(d, out, true).empty());
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// --- Percentiles and summaries ----------------------------------------------

void test_percentiles_and_summary_endpoint_inclusion() {
    std::vector<std::int64_t> samples = { 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000 };
    CHECK(oz::viewer_core::bench_nearest_rank_percentile(samples, 50.0) == 500);
    CHECK(oz::viewer_core::bench_nearest_rank_percentile(samples, 95.0) == 1000);
    CHECK(oz::viewer_core::bench_nearest_rank_percentile(samples, 99.0) == 1000);
    CHECK(oz::viewer_core::bench_nearest_rank_percentile({}, 50.0) == 0);

    // Superseded / failed / timed-out / no-op counts are metadata only; latency
    // statistics come solely from accepted samples.
    std::vector<std::int64_t> lats = { 300, 100, 500, 200, 400 };
    auto summary = oz::viewer_core::compute_scenario_summary(
        "cached_next", "warm_protocol", 10, 3, 1, 1, 2, lats);
    CHECK(summary.scenario == "cached_next");
    CHECK(summary.os_cache_condition == "warm_protocol");
    CHECK(summary.requested_count == 10);
    CHECK(summary.successful_count == 5);
    CHECK(summary.superseded_count == 3);
    CHECK(summary.failed_count == 1);
    CHECK(summary.timed_out_count == 1);
    CHECK(summary.no_op_count == 2);
    CHECK(summary.min_ns == 100);
    CHECK(summary.max_ns == 500);
    CHECK(summary.median_ns == 300);
    CHECK(summary.p95_ns == 500);
    CHECK(summary.p99_ns == 500);
    CHECK(summary.convention == "nearest-rank");

    // Cache state labels stay distinct from OS cache condition labels.
    CHECK(std::string(oz::viewer_core::to_string(oz::viewer_core::bench_cache_state::hit_at_request)) == "hit_at_request");
    CHECK(std::string(oz::viewer_core::to_string(oz::viewer_core::bench_cache_state::miss_at_request)) == "miss_at_request");
    CHECK(std::string(oz::viewer_core::os_cache_condition_str("cold_protocol")) == "cold_protocol");
    CHECK(std::string(oz::viewer_core::os_cache_condition_str("warm_protocol")) == "warm_protocol");
    CHECK(std::string(oz::viewer_core::os_cache_condition_str("other")) == "uncontrolled");
}

// --- Session lifecycle / accounting -----------------------------------------

void test_session_requires_valid_options() {
    const auto dir = unique_temp_dir("opts");
    const auto out = dir / "run.jsonl";
    oz::viewer_core::navigation_benchmark_session session;
    auto corpus = fake_corpus();

    oz::viewer_core::nav_bench_options opts;
    opts.enabled = true;
    opts.corpus_dir.clear();  // invalid
    opts.output_path = out;
    opts.overwrite = true;
    CHECK(session.init(opts, 1024, 768, 96.0f, false, &corpus) ==
          oz::viewer_core::nav_bench_status::invalid_args);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void test_session_output_create_new_routing() {
    // Audit requirement: session init() must route through the create-new
    // helper instead of exists()+trunc, never truncating an existing run.
    const auto dir = unique_temp_dir("session_output");
    const auto out = dir / "run.jsonl";
    auto corpus = fake_corpus();

    oz::viewer_core::nav_bench_options opts;
    opts.enabled = true;
    opts.corpus_dir = "fake-corpus";
    opts.output_path = out;
    opts.overwrite = false;
    opts.timeout_ms = 1000;
    opts.repetitions = 1;
    opts.rapid_burst = 1;

    {
        std::ofstream existing(out, std::ios::binary);
        existing << "occupied\n";
    }

    oz::viewer_core::navigation_benchmark_session refusing;
    CHECK(refusing.init(opts, 1024, 768, 96.0f, false, &corpus) ==
          oz::viewer_core::nav_bench_status::output_exists);
    CHECK(!refusing.is_active());
    CHECK(read_text(out) == "occupied\n");  // existing run never truncated

    opts.overwrite = true;
    oz::viewer_core::navigation_benchmark_session accepting;
    CHECK(accepting.init(opts, 1024, 768, 96.0f, false, &corpus) ==
          oz::viewer_core::nav_bench_status::success);
    accepting.finish("completed", "");
    CHECK(oz::viewer_core::validate_navigation_jsonl_file(out).ok);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void test_session_accounting_and_present_matching() {
    const auto dir = unique_temp_dir("accounting");
    const auto out = dir / "run.jsonl";
    auto corpus = fake_corpus();

    oz::viewer_core::navigation_benchmark_session session;
    CHECK(init_session(session, out, 10000, 1, 1, &corpus));
    CHECK(session.is_active());

    // Make the session start meaningfully older than the request so a
    // session-start timestamp would produce a visible fake latency.
    Sleep(25);

    const auto req1 = session.record_navigation_request(
        "cached_next", "forward", 1, "img_0001.jpg", 0, "img_0000.jpg",
        oz::viewer_core::bench_cache_state::hit_at_request, 1);
    CHECK(req1 != 0);
    session.record_successful_present(req1, session.query_ticks(), 1, "img_0001.jpg",
                                      "sha1", 1024, 512, 2, 1);

    // Duplicate present for the same (already resolved) request: task event only.
    session.record_successful_present(req1, session.query_ticks(), 1, "img_0001.jpg",
                                      "sha1", 1024, 512, 2, 1);
    // Unmatched present: never a latency sample.
    session.record_successful_present(424242, session.query_ticks(), 7, "img_0007.jpg",
                                      "sha7", 1024, 512, 2, 1);

    // Superseded present: the request epoch is older than the latest epoch.
    const auto req2 = session.record_navigation_request(
        "rapid_replacement", "forward", 2, "img_0002.jpg", 1, "img_0001.jpg",
        oz::viewer_core::bench_cache_state::miss_at_request, 5);
    const auto req3 = session.record_navigation_request(
        "rapid_replacement", "forward", 3, "img_0003.jpg", 2, "img_0002.jpg",
        oz::viewer_core::bench_cache_state::miss_at_request, 6);
    CHECK(req2 != 0 && req3 != 0);
    session.record_successful_present(req2, session.query_ticks(), 2, "img_0002.jpg",
                                      "sha2", 1024, 512, 2, 1);
    session.record_request_superseded(req3, 6, "superseded_by_newer_navigation");

    session.finish("completed", "");
    CHECK(!session.is_active());

    const auto validation = oz::viewer_core::validate_navigation_jsonl_file(out);
    CHECK(validation.ok);

    const auto records = parse_records(out);
    const auto* present = find_record(records, "successful_present");
    CHECK(present != nullptr);
    if (present != nullptr) {
        CHECK(oz::json::get_int(*present, "request_id").value_or(0) == static_cast<std::int64_t>(req1));
        CHECK(oz::json::get_string(*present, "app_cache_state").value_or("") == "hit_at_request");
        const auto latency = oz::json::get_int(*present, "latency_ns").value_or(-1);
        CHECK(latency >= 0);
        CHECK(latency < 20'000'000);  // microseconds, not the 25ms session age
    }

    const auto* cached_summary = find_record(records, "scenario_summary", "cached_next");
    CHECK(cached_summary != nullptr);
    if (cached_summary != nullptr) {
        CHECK(oz::json::get_int(*cached_summary, "requested_count").value_or(0) == 1);
        CHECK(oz::json::get_int(*cached_summary, "successful_count").value_or(0) == 1);
        CHECK(oz::json::get_int(*cached_summary, "superseded_count").value_or(0) == 0);
    }
    const auto* rapid_summary = find_record(records, "scenario_summary", "rapid_replacement");
    CHECK(rapid_summary != nullptr);
    if (rapid_summary != nullptr) {
        CHECK(oz::json::get_int(*rapid_summary, "requested_count").value_or(0) == 2);
        CHECK(oz::json::get_int(*rapid_summary, "successful_count").value_or(0) == 0);
        CHECK(oz::json::get_int(*rapid_summary, "superseded_count").value_or(0) == 2);
    }

    const auto* run_end = find_record(records, "run_end");
    CHECK(run_end != nullptr);
    if (run_end != nullptr) {
        CHECK(oz::json::get_string(*run_end, "status").value_or("") == "completed");
        CHECK(oz::json::get_int(*run_end, "total_presents").value_or(0) == 1);
        CHECK(oz::json::get_int(*run_end, "total_timed_out").value_or(-1) == 0);
    }

    // No raw private path may appear anywhere in the public stream.
    CHECK(!oz::viewer_core::contains_private_path_data(read_text(out)));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void test_session_watchdog_times_out_and_ends_run() {
    const auto dir = unique_temp_dir("watchdog");
    const auto out = dir / "run.jsonl";
    auto corpus = corpus_with_entries(2, 2);

    oz::viewer_core::navigation_benchmark_session session;
    CHECK(init_session(session, out, 1 /* ms */, 1, 1, &corpus));

    oz::viewer_core::nav_bench_viewer_state viewer_state;
    viewer_state.list_size = 2;
    viewer_state.committed_index = 0;
    viewer_state.work_drained = true;
    viewer_state.target_present = true;  // verified preload ready

    const auto setup_step = session.pump_benchmark_step(viewer_state);
    CHECK(setup_step.kind == oz::viewer_core::nav_bench_step_kind::await_cache);
    CHECK(setup_step.target_index == 1);
    CHECK(setup_step.expect_target_present);

    const auto nav_step = session.pump_benchmark_step(viewer_state);
    CHECK(nav_step.kind == oz::viewer_core::nav_bench_step_kind::navigation);
    CHECK(nav_step.vk_key == VK_RIGHT);
    CHECK(nav_step.quiesce);

    const auto req = session.record_navigation_request(
        session.current_scenario_label(), "forward", 1, "img_0001.jpg", 0, "img_0000.jpg",
        oz::viewer_core::bench_cache_state::hit_at_request, 1);
    CHECK(req != 0);
    CHECK(session.step_pending());

    Sleep(10);
    CHECK(session.enforce_watchdog());
    CHECK(session.timed_out());
    CHECK(!session.step_pending());
    CHECK(session.timeout_reason() == "request_exceeded_timeout_ms");

    session.finish("timed_out", session.timeout_reason());

    const auto validation = oz::viewer_core::validate_navigation_jsonl_file(out);
    CHECK(validation.ok);
    const auto records = parse_records(out);
    const auto* timed_out_event = find_record(records, "task_event", "cached_next");
    CHECK(timed_out_event != nullptr);
    if (timed_out_event != nullptr) {
        CHECK(oz::json::get_string(*timed_out_event, "event_type").value_or("") == "timed_out");
        CHECK(oz::json::get_int(*timed_out_event, "request_id").value_or(0) == static_cast<std::int64_t>(req));
    }
    const auto* run_end = find_record(records, "run_end");
    CHECK(run_end != nullptr);
    if (run_end != nullptr) {
        CHECK(oz::json::get_string(*run_end, "status").value_or("") == "timed_out");
        CHECK(oz::json::get_int(*run_end, "total_timed_out").value_or(0) == 1);
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// Regression: with a single-item navigation list the measured keys are
// genuinely at the boundary. The state machine must still advance (no WM_TIMER
// hang), record only truthful categorized no_op events (never fabricated
// samples), and complete. The uncached/rapid scenarios have no eligible range
// and therefore issue no measured requests at all.
void test_single_item_list_records_truthful_no_ops() {
    const auto dir = unique_temp_dir("noop");
    const auto out = dir / "run.jsonl";
    auto corpus = corpus_with_entries(1, 1);

    oz::viewer_core::navigation_benchmark_session session;
    CHECK(init_session(session, out, 10000, 1, 1, &corpus));

    const auto sim = drive_full_session(session, corpus, 1 /* list_size */);

    // cached_next (1) + forward_back (1) + Home + End = 4 truthful no-ops.
    CHECK(sim.no_ops == 4);
    CHECK(sim.navigation_requests == 0);
    CHECK(sim.rearm_actions == 0);

    const auto validation = oz::viewer_core::validate_navigation_jsonl_file(out);
    CHECK(validation.ok);
    const auto records = parse_records(out);

    std::size_t no_op_events = 0;
    for (const auto& r : records) {
        if (oz::json::get_string(r, "record_type").value_or("") == "task_event" &&
            oz::json::get_string(r, "event_type").value_or("") == "no_op") {
            ++no_op_events;
        }
    }
    CHECK(no_op_events == 4);

    const auto* run_end = find_record(records, "run_end");
    CHECK(run_end != nullptr);
    if (run_end != nullptr) {
        CHECK(oz::json::get_string(*run_end, "status").value_or("") == "completed");
        CHECK(oz::json::get_int(*run_end, "total_no_ops").value_or(0) == 4);
        CHECK(oz::json::get_int(*run_end, "total_requests").value_or(0) == 0);
    }
    const auto* cached_summary = find_record(records, "scenario_summary", "cached_next");
    CHECK(cached_summary != nullptr);
    if (cached_summary != nullptr) {
        CHECK(oz::json::get_int(*cached_summary, "no_op_count").value_or(0) == 1);
        CHECK(oz::json::get_int(*cached_summary, "successful_count").value_or(0) == 0);
    }
    // No latency sample may ever come from a no-op step.
    for (const auto& r : records) {
        CHECK(oz::json::get_string(r, "record_type").value_or("") != "successful_present");
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void test_session_failed_request_clears_wait() {
    const auto dir = unique_temp_dir("failed");
    const auto out = dir / "run.jsonl";
    auto corpus = corpus_with_entries(2, 0);  // no 24MP pair: uncached is empty

    oz::viewer_core::navigation_benchmark_session session;
    CHECK(init_session(session, out, 10000, 1, 1, &corpus));

    oz::viewer_core::nav_bench_viewer_state viewer_state;
    viewer_state.list_size = 2;
    viewer_state.committed_index = 0;
    viewer_state.work_drained = true;
    viewer_state.target_present = true;

    const auto setup_step = session.pump_benchmark_step(viewer_state);
    CHECK(setup_step.kind == oz::viewer_core::nav_bench_step_kind::await_cache);
    const auto nav_step = session.pump_benchmark_step(viewer_state);
    CHECK(nav_step.kind == oz::viewer_core::nav_bench_step_kind::navigation);
    CHECK(nav_step.vk_key == VK_RIGHT);

    const auto req = session.record_navigation_request(
        session.current_scenario_label(), "forward", 1, "img_0001.jpg", 0, "img_0000.jpg",
        oz::viewer_core::bench_cache_state::hit_at_request, 1);
    CHECK(req != 0);
    CHECK(session.step_pending());

    // A failed decode resolves the outstanding request as failed (not timed out)
    // and releases the step wait so the state machine can continue.
    session.record_request_failed(req, 1, "p0_decode_failed");
    CHECK(!session.step_pending());
    CHECK(!session.timed_out());

    // The machine advances past cached_next instead of hanging on the failed
    // step; the next scenario's setup (or an empty residual phase) is produced.
    const auto after = session.pump_benchmark_step(viewer_state);
    CHECK(after.kind != oz::viewer_core::nav_bench_step_kind::idle ||
          session.current_phase() != oz::viewer_core::nav_bench_phase::cached_next_run);

    session.finish("completed", "");

    const auto validation = oz::viewer_core::validate_navigation_jsonl_file(out);
    CHECK(validation.ok);
    const auto records = parse_records(out);
    const auto* run_end = find_record(records, "run_end");
    CHECK(run_end != nullptr);
    if (run_end != nullptr) {
        CHECK(oz::json::get_int(*run_end, "total_failed").value_or(0) == 1);
        CHECK(oz::json::get_int(*run_end, "total_timed_out").value_or(0) == 0);
        CHECK(oz::json::get_string(*run_end, "status").value_or("") == "completed");
    }
    const auto* summary = find_record(records, "scenario_summary", "cached_next");
    CHECK(summary != nullptr);
    if (summary != nullptr) {
        CHECK(oz::json::get_int(*summary, "failed_count").value_or(0) == 1);
        CHECK(oz::json::get_int(*summary, "successful_count").value_or(0) == 0);
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// --- Scenario isolation / sample eligibility ---------------------------------

// cached_next must expose a pure request-time-hit sample set: a miss that
// slips past readiness is presented truthfully but audited as excluded and
// never contributes latency statistics.
void test_cached_next_pure_hit_sample_filtering() {
    const auto dir = unique_temp_dir("cached_filter");
    const auto out = dir / "run.jsonl";
    auto corpus = corpus_with_entries(4, 4);

    oz::viewer_core::navigation_benchmark_session session;
    CHECK(init_session(session, out, 10000, 1, 1, &corpus));
    Sleep(2);  // make session-start placeholder latency detectable if present

    const auto hit = session.record_navigation_request(
        "cached_next", "forward", 1, "img_0001", 0, "img_0000",
        oz::viewer_core::bench_cache_state::hit_at_request, 1);
    session.record_successful_present(hit, session.query_ticks(), 1, "img_0001",
                                      "sha", 0, 0, 0, 0);

    const auto miss = session.record_navigation_request(
        "cached_next", "forward", 2, "img_0002", 1, "img_0001",
        oz::viewer_core::bench_cache_state::miss_at_request, 2);
    session.record_successful_present(miss, session.query_ticks(), 2, "img_0002",
                                      "sha", 0, 0, 0, 0);

    session.finish("completed", "");
    const auto records = parse_records(out);

    const auto* cached_sum = find_record(records, "scenario_summary", "cached_next");
    CHECK(cached_sum != nullptr);
    if (cached_sum != nullptr) {
        CHECK(oz::json::get_int(*cached_sum, "requested_count").value_or(0) == 2);
        CHECK(oz::json::get_int(*cached_sum, "successful_count").value_or(0) == 1);
    }

    bool excluded_cache_state = false;
    std::size_t excluded_events = 0;
    for (const auto& rec : records) {
        if (oz::json::get_string(rec, "record_type").value_or("") != "task_event") continue;
        if (oz::json::get_string(rec, "event_type").value_or("") != "step") continue;
        if (oz::json::get_string(rec, "status").value_or("") != "excluded") continue;
        ++excluded_events;
        const auto reason = oz::json::get_string(rec, "reason").value_or("");
        if (reason.find("cache_state_not_hit_at_request") != std::string::npos) {
            excluded_cache_state = true;
        }
    }
    CHECK(excluded_events == 1);
    CHECK(excluded_cache_state);

    // The ineligible present is still a real present; only its latency is
    // excluded from the scenario summary.
    const auto* run_end = find_record(records, "run_end");
    CHECK(run_end != nullptr);
    if (run_end != nullptr) {
        CHECK(oz::json::get_int(*run_end, "total_presents").value_or(0) == 2);
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// uncached_24mp_next accepts only request-time misses whose committed target
// is a 24MP image (corpus indices 0..49 in the standard corpus).
void test_uncached_24mp_target_enforcement() {
    const auto dir = unique_temp_dir("uncached_filter");
    const auto out = dir / "run.jsonl";
    auto corpus = corpus_with_entries(4, 2);  // 0,1 = 24MP; 2 = 12MP; 3 = 6MP

    oz::viewer_core::navigation_benchmark_session session;
    CHECK(init_session(session, out, 10000, 1, 1, &corpus));

    // Valid: request-time miss and committed 24MP target.
    const auto good = session.record_navigation_request(
        "uncached_24mp_next", "forward", 1, "img_0001", 0, "img_0000",
        oz::viewer_core::bench_cache_state::miss_at_request, 1);
    session.record_successful_present(good, session.query_ticks(), 1, "img_0001",
                                      "sha", 0, 0, 0, 0);

    // Excluded: committed target is not 24MP.
    const auto not_24mp = session.record_navigation_request(
        "uncached_24mp_next", "forward", 2, "img_0002", 1, "img_0001",
        oz::viewer_core::bench_cache_state::miss_at_request, 2);
    session.record_successful_present(not_24mp, session.query_ticks(), 2, "img_0002",
                                      "sha", 0, 0, 0, 0);

    // Excluded: request-time hit (cache not actually cleared).
    const auto hit = session.record_navigation_request(
        "uncached_24mp_next", "forward", 1, "img_0001", 0, "img_0000",
        oz::viewer_core::bench_cache_state::hit_at_request, 3);
    session.record_successful_present(hit, session.query_ticks(), 1, "img_0001",
                                      "sha", 0, 0, 0, 0);

    session.finish("completed", "");
    const auto records = parse_records(out);

    const auto* summary = find_record(records, "scenario_summary", "uncached_24mp_next");
    CHECK(summary != nullptr);
    if (summary != nullptr) {
        CHECK(oz::json::get_int(*summary, "requested_count").value_or(0) == 3);
        CHECK(oz::json::get_int(*summary, "successful_count").value_or(0) == 1);
    }

    bool excluded_not_24mp = false;
    bool excluded_hit = false;
    std::size_t excluded_events = 0;
    for (const auto& rec : records) {
        if (oz::json::get_string(rec, "record_type").value_or("") != "task_event") continue;
        if (oz::json::get_string(rec, "event_type").value_or("") != "step" ||
            oz::json::get_string(rec, "status").value_or("") != "excluded") {
            continue;
        }
        ++excluded_events;
        const auto reason = oz::json::get_string(rec, "reason").value_or("");
        if (reason.find("committed_target_not_24mp") != std::string::npos) {
            excluded_not_24mp = true;
        }
        if (reason.find("cache_state_not_miss_at_request") != std::string::npos) {
            excluded_hit = true;
        }
    }
    CHECK(excluded_events == 2);
    CHECK(excluded_not_24mp);
    CHECK(excluded_hit);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// Rapid replacement: a superseded request whose present lands late must never
// be sampled after a newer request, while the final still-current target does
// produce the accepted present.
void test_rapid_supersession_and_final_target() {
    const auto dir = unique_temp_dir("rapid_final");
    const auto out = dir / "run.jsonl";
    auto corpus = corpus_with_entries(8, 8);

    oz::viewer_core::navigation_benchmark_session session;
    CHECK(init_session(session, out, 10000, 1, 2, &corpus));

    const auto older = session.record_navigation_request(
        "rapid_replacement", "forward", 1, "img_0001", 0, "img_0000",
        oz::viewer_core::bench_cache_state::miss_at_request, 1);
    const auto newer = session.record_navigation_request(
        "rapid_replacement", "forward", 2, "img_0002", 1, "img_0001",
        oz::viewer_core::bench_cache_state::miss_at_request, 2);
    CHECK(older != 0 && newer != 0);

    // The final current target presents successfully.
    session.record_successful_present(newer, session.query_ticks(), 2, "img_0002",
                                      "sha", 0, 0, 0, 0);
    // The superseded request's present lands afterwards and must never be a
    // sample for the superseded target.
    session.record_successful_present(older, session.query_ticks(), 1, "img_0001",
                                      "sha", 0, 0, 0, 0);

    session.finish("completed", "");
    const auto records = parse_records(out);

    const auto* summary = find_record(records, "scenario_summary", "rapid_replacement");
    CHECK(summary != nullptr);
    if (summary != nullptr) {
        CHECK(oz::json::get_int(*summary, "requested_count").value_or(0) == 2);
        CHECK(oz::json::get_int(*summary, "successful_count").value_or(0) == 1);
        CHECK(oz::json::get_int(*summary, "superseded_count").value_or(0) == 1);
    }
    const auto* run_end = find_record(records, "run_end");
    CHECK(run_end != nullptr);
    if (run_end != nullptr) {
        // The late superseded present is a categorized superseded event, never
        // a present sample, so only the final target's present is counted.
        CHECK(oz::json::get_int(*run_end, "total_presents").value_or(0) == 1);
        CHECK(oz::json::get_int(*run_end, "total_superseded").value_or(0) == 1);
    }
    // Only the newer request id appears as a successful_present.
    const auto* present = find_record(records, "successful_present");
    CHECK(present != nullptr);
    if (present != nullptr) {
        CHECK(oz::json::get_int(*present, "request_id").value_or(0) ==
              static_cast<std::int64_t>(newer));
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// Setup waits are bounded by --timeout-ms: an unsatisfied verified-preload
// wait ends the run as timed_out with categorized evidence instead of hanging.
void test_setup_wait_watchdog_bounds_preload_readiness() {
    const auto dir = unique_temp_dir("setup_wait");
    const auto out = dir / "run.jsonl";
    auto corpus = corpus_with_entries(2, 2);

    oz::viewer_core::navigation_benchmark_session session;
    CHECK(init_session(session, out, 1 /* ms */, 1, 1, &corpus));

    oz::viewer_core::nav_bench_viewer_state viewer_state;
    viewer_state.list_size = 2;
    viewer_state.committed_index = 0;
    viewer_state.work_drained = true;
    viewer_state.target_present = false;

    const auto wait_step = session.pump_benchmark_step(viewer_state);
    CHECK(wait_step.kind == oz::viewer_core::nav_bench_step_kind::await_cache);
    CHECK(wait_step.target_index == 1);
    CHECK(wait_step.expect_target_present);

    Sleep(10);
    CHECK(session.enforce_watchdog());
    CHECK(session.timed_out());
    CHECK(session.timeout_reason() == "setup_wait_exceeded_timeout_ms");

    session.finish("timed_out", session.timeout_reason());
    const auto records = parse_records(out);

    bool setup_timed_out_event = false;
    for (const auto& rec : records) {
        if (oz::json::get_string(rec, "record_type").value_or("") != "task_event") continue;
        if (oz::json::get_string(rec, "event_type").value_or("") != "timed_out") continue;
        if (oz::json::get_string(rec, "reason").value_or("") ==
            "setup_wait_exceeded_timeout_ms") {
            setup_timed_out_event = true;
        }
    }
    CHECK(setup_timed_out_event);
    const auto* run_end = find_record(records, "run_end");
    CHECK(run_end != nullptr);
    if (run_end != nullptr) {
        CHECK(oz::json::get_string(*run_end, "status").value_or("") == "timed_out");
        CHECK(oz::json::get_int(*run_end, "total_timed_out").value_or(0) == 1);
    }
    CHECK(oz::viewer_core::validate_navigation_jsonl_file(out).ok);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// Regression for the former --repetitions 500 boundary contamination: with 500
// measured cached steps on the 500-image corpus, the old state machine walked
// to index 499 and every later scenario step became a boundary no-op. The fixed
// machine isolates each scenario with explicit, labeled re-arm/setup so every
// measured step is valid and samples are pure.
void test_state_machine_scenario_isolation_with_500_steps() {
    const auto dir = unique_temp_dir("isolation500");
    const auto out = dir / "run.jsonl";
    const std::size_t corpus_count = 500;
    auto corpus = corpus_with_entries(corpus_count);

    oz::viewer_core::navigation_benchmark_session session;
    const auto sim = [&] {
        // Former defect configuration: --repetitions 500 on the 500-image
        // corpus with --rapid-burst 100.
        CHECK(init_session(session, out, 60000, 500, 100, &corpus));
        return drive_full_session(session, corpus, corpus_count);
    }();

    // The former defect in one assertion: a fixed 500-step forward walk from
    // index 0 ends at index 499 whose Next is a boundary no-op (and every
    // later scenario inherited that dead boundary). The policy still reports
    // that boundary truthfully; the isolated machine never issues it.
    CHECK(!oz::viewer_core::expected_target_index(
              oz::viewer_core::bench_nav_direction::forward, 499, 500).has_value());

    // No measured scenario may run into the list boundary.
    CHECK(sim.no_ops == 0);
    CHECK(sim.invalid_rearm_actions == 0);
    CHECK(sim.cached_requests == 500);
    CHECK(sim.cached_hit_requests == 500);
    CHECK(sim.cached_miss_requests == 0);
    CHECK(sim.uncached_requests == 10);
    CHECK(sim.uncached_miss_requests == 10);
    CHECK(sim.uncached_bad_targets == 0);
    CHECK(sim.purge_actions == 10);
    CHECK(sim.rapid_requests == 100);
    CHECK(sim.rapid_superseded == 99);
    CHECK(sim.forward_back_requests == 500);
    CHECK(sim.memory_requests == 2);
    // Explicit labeled re-arms: cached boundary step, uncached prefix, rapid
    // restore, forward/back mid, memory last.
    CHECK(sim.rearm_actions == 5);

    const auto validation = oz::viewer_core::validate_navigation_jsonl_file(out);
    CHECK(validation.ok);
    const auto records = parse_records(out);

    // Setup transitions are labeled and never summarized as latency samples.
    std::size_t setup_step_events = 0;
    for (const auto& rec : records) {
        if (oz::json::get_string(rec, "record_type").value_or("") != "task_event") continue;
        if (oz::json::get_string(rec, "event_type").value_or("") == "step" &&
            oz::json::get_string(rec, "status").value_or("") == "setup") {
            ++setup_step_events;
        }
    }
    CHECK(setup_step_events == 5);
    CHECK(find_record(records, "scenario_summary", "setup_rearm") == nullptr);

    const auto* cached_sum = find_record(records, "scenario_summary", "cached_next");
    CHECK(cached_sum != nullptr);
    if (cached_sum != nullptr) {
        CHECK(oz::json::get_int(*cached_sum, "requested_count").value_or(0) == 500);
        CHECK(oz::json::get_int(*cached_sum, "successful_count").value_or(0) == 500);
        CHECK(oz::json::get_int(*cached_sum, "no_op_count").value_or(0) == 0);
    }
    const auto* uncached_sum =
        find_record(records, "scenario_summary", "uncached_24mp_next");
    CHECK(uncached_sum != nullptr);
    if (uncached_sum != nullptr) {
        CHECK(oz::json::get_int(*uncached_sum, "requested_count").value_or(0) == 10);
        CHECK(oz::json::get_int(*uncached_sum, "successful_count").value_or(0) == 10);
        CHECK(oz::json::get_int(*uncached_sum, "no_op_count").value_or(0) == 0);
    }
    const auto* rapid_sum = find_record(records, "scenario_summary", "rapid_replacement");
    CHECK(rapid_sum != nullptr);
    if (rapid_sum != nullptr) {
        CHECK(oz::json::get_int(*rapid_sum, "superseded_count").value_or(0) == 99);
        CHECK(oz::json::get_int(*rapid_sum, "successful_count").value_or(0) == 1);
        CHECK(oz::json::get_int(*rapid_sum, "no_op_count").value_or(0) == 0);
    }
    const auto* forward_sum =
        find_record(records, "scenario_summary", "forward_back_sweep");
    CHECK(forward_sum != nullptr);
    if (forward_sum != nullptr) {
        CHECK(oz::json::get_int(*forward_sum, "requested_count").value_or(0) == 500);
        CHECK(oz::json::get_int(*forward_sum, "no_op_count").value_or(0) == 0);
    }

    // Every accepted cached sample is a request-time hit; every accepted
    // uncached sample is a request-time miss on a 24MP target.
    for (const auto& rec : records) {
        if (oz::json::get_string(rec, "record_type").value_or("") != "successful_present") {
            continue;
        }
        const auto scenario = oz::json::get_string(rec, "scenario").value_or("");
        const auto cache_state = oz::json::get_string(rec, "app_cache_state").value_or("");
        const auto committed_idx = oz::json::get_int(rec, "committed_index").value_or(-1);
        if (scenario == "cached_next") {
            CHECK(cache_state == "hit_at_request");
        }
        if (scenario == "uncached_24mp_next") {
            CHECK(cache_state == "miss_at_request");
            CHECK(committed_idx >= 0 && committed_idx < 50);
        }
    }
    CHECK(!oz::viewer_core::contains_private_path_data(read_text(out)));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// --- Corpus validation ------------------------------------------------------

void test_corpus_validator_negatives() {
    auto res_bad = oz::viewer_core::validate_navigation_corpus(L"non_existent_folder_xyz_123");
    CHECK(!res_bad.ok);
    CHECK(!res_bad.error.empty());

    const auto dir = unique_temp_dir("empty_corpus");
    auto res_empty = oz::viewer_core::validate_navigation_corpus(dir);
    CHECK(!res_empty.ok);
    CHECK(res_empty.error.find("exactly 500") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

#if OZVIEWER_HAVE_TURBOJPEG
// Isolated smoke corpus (never final benchmark data): proves the deterministic
// generator/validator contract on explicit invocation without producing the
// 500-image corpus.
void test_corpus_smoke_generation_and_validation() {
    const auto dir = unique_temp_dir("corpus_smoke");

    const std::string gen_err = oz::viewer_core::generate_deterministic_corpus(dir, nullptr, 6);
    CHECK(gen_err.empty());

    auto val = oz::viewer_core::validate_navigation_corpus(dir, 6);
    CHECK(val.ok);
    CHECK(val.count == 6);
    CHECK(val.entries.size() == 6);
    CHECK(!val.manifest_sha256.empty());
    CHECK(std::filesystem::exists(dir / "manifest.json"));
    CHECK(std::filesystem::exists(dir / "manifest.json.sha256"));
    if (!val.entries.empty()) {
        CHECK(val.entries[0].alias == "img_0000");
        CHECK(val.entries[0].rel_filename == "img_0000.jpg");
        CHECK(val.entries[0].width == 6000);
        CHECK(val.entries[0].height == 4000);
    }

    // Wrong expected count is rejected.
    auto wrong_count = oz::viewer_core::validate_navigation_corpus(dir, 7);
    CHECK(!wrong_count.ok);

    // A manifest without its seal is rejected, and a recomputed seal restores
    // validity (proves the seal is actually verified).
    {
        std::error_code ec_rm;
        std::filesystem::remove(dir / "manifest.json.sha256", ec_rm);
        auto no_seal = oz::viewer_core::validate_navigation_corpus(dir, 6);
        CHECK(!no_seal.ok);
        const std::string seal =
            oz::collector::sha256_file_hex((dir / "manifest.json").wstring());
        std::ofstream seal_out(dir / "manifest.json.sha256", std::ios::binary);
        seal_out << seal;
    }
    CHECK(oz::viewer_core::validate_navigation_corpus(dir, 6).ok);

    // Injected EXIF/APP1 metadata is rejected.
    {
        const auto file = dir / "img_0000.jpg";
        std::ifstream in(file, std::ios::binary);
        const std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)),
                                              std::istreambuf_iterator<char>());
        std::vector<unsigned char> modified;
        modified.push_back(0xFF);
        modified.push_back(0xD8);
        const unsigned char app1[] = {0xFF, 0xE1, 0x00, 0x08, 'E', 'x', 'i', 'f', 0x00, 0x00};
        modified.insert(modified.end(), app1, app1 + sizeof(app1));
        modified.insert(modified.end(), data.begin() + 2, data.end());
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(modified.data()),
                  static_cast<std::streamsize>(modified.size()));
    }
    auto with_exif = oz::viewer_core::validate_navigation_corpus(dir, 6);
    CHECK(!with_exif.ok);
    CHECK(with_exif.error.find("metadata") != std::string::npos);

    // Foreign files are rejected.
    {
        std::ofstream foreign(dir / "notes.txt", std::ios::binary);
        foreign << "not a jpeg";
    }
    auto with_foreign = oz::viewer_core::validate_navigation_corpus(dir, 6);
    CHECK(!with_foreign.ok);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
#endif  // OZVIEWER_HAVE_TURBOJPEG

}  // namespace

int wmain() {
    test_cli_parsing_and_bounds();
    test_shared_option_validation();
    test_navigation_intent_policy();
    test_outcome_classification_policy();
    test_privacy_redaction_and_schema_validation();
    test_jsonl_stream_validation();
    test_open_benchmark_output_create_new();
    test_percentiles_and_summary_endpoint_inclusion();
    test_session_requires_valid_options();
    test_session_output_create_new_routing();
    test_session_accounting_and_present_matching();
    test_session_watchdog_times_out_and_ends_run();
    test_single_item_list_records_truthful_no_ops();
    test_session_failed_request_clears_wait();
    test_cached_next_pure_hit_sample_filtering();
    test_uncached_24mp_target_enforcement();
    test_rapid_supersession_and_final_target();
    test_setup_wait_watchdog_bounds_preload_readiness();
    test_state_machine_scenario_isolation_with_500_steps();
    test_corpus_validator_negatives();
#if OZVIEWER_HAVE_TURBOJPEG
    test_corpus_smoke_generation_and_validation();
#endif

    std::printf("navigation_benchmark_tests: %d checks, %d failures\n",
                test::g_checks, test::g_failures);
    return test::g_failures == 0 ? 0 : 1;
}

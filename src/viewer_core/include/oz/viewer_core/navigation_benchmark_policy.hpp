// oz/viewer_core/navigation_benchmark_policy.hpp - pure, GUI-free policy
// helpers for the navigation benchmark (schema oz.navigation-benchmark.v1).
//
// Everything here is a deterministic pure function or a constant: no windows,
// no HWND, no renderer, no worker, no I/O. The viewer integration calls
// expected_target_index() to decode navigation intent (the same no-wrap rule as
// Stage 2.1), and the benchmark session calls classify_request_outcome() so
// only successful, still-current presents contribute latency samples.
#pragma once

#include "oz/viewer_core/navigation_benchmark_schema.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace oz::viewer_core {

// ---------------------------------------------------------------------------
// Expected navigation transitions
// ---------------------------------------------------------------------------

// Navigation intent decoded from a key. `none` is a no-op.
enum class bench_nav_direction : std::uint8_t {
    forward,
    backward,
    home,
    end,
    none
};

inline std::string_view to_string(bench_nav_direction d) noexcept {
    switch (d) {
        case bench_nav_direction::forward: return "forward";
        case bench_nav_direction::backward: return "backward";
        case bench_nav_direction::home: return "home";
        case bench_nav_direction::end: return "end";
        case bench_nav_direction::none: return "none";
    }
    return "none";
}

// Expected target index for a request over `list_size` items whose current
// index is `current_index`. No wrapping, matching the Stage 2.1 navigation
// contract: forward at the last item and backward at the first item are no-ops.
// Returns nullopt for a no-op or an invalid current index.
inline std::optional<std::size_t> expected_target_index(bench_nav_direction dir,
                                                        std::size_t current_index,
                                                        std::size_t list_size) noexcept {
    if (list_size == 0 || current_index >= list_size) {
        return std::nullopt;
    }
    switch (dir) {
        case bench_nav_direction::forward:
            if (current_index + 1 < list_size) {
                return current_index + 1;
            }
            return std::nullopt;
        case bench_nav_direction::backward:
            if (current_index > 0) {
                return current_index - 1;
            }
            return std::nullopt;
        case bench_nav_direction::home:
            if (current_index != 0) {
                return std::size_t{0};
            }
            return std::nullopt;
        case bench_nav_direction::end:
            if (current_index != list_size - 1) {
                return list_size - 1;
            }
            return std::nullopt;
        case bench_nav_direction::none:
            return std::nullopt;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Request / epoch outcome classification
// ---------------------------------------------------------------------------

enum class bench_outcome : std::uint8_t {
    success,
    superseded,
    failed,
    timed_out,
    no_op
};

inline std::string_view to_string(bench_outcome o) noexcept {
    switch (o) {
        case bench_outcome::success: return "success";
        case bench_outcome::superseded: return "superseded";
        case bench_outcome::failed: return "failed";
        case bench_outcome::timed_out: return "timed_out";
        case bench_outcome::no_op: return "no_op";
    }
    return "failed";
}

// Classifies a completed request. A request is `superseded` when its epoch is
// older than the latest user epoch, even if a present for it eventually lands:
// such a present must not contribute a latency sample. Failure and timeout are
// reported explicitly; `no_op` covers a request that produced no index change.
inline bench_outcome classify_request_outcome(std::uint64_t request_epoch,
                                              std::uint64_t latest_epoch,
                                              bool present_succeeded,
                                              bool failed,
                                              bool timed_out,
                                              bool no_op) noexcept {
    if (timed_out) {
        return bench_outcome::timed_out;
    }
    if (failed) {
        return bench_outcome::failed;
    }
    if (no_op) {
        return bench_outcome::no_op;
    }
    if (request_epoch < latest_epoch) {
        return bench_outcome::superseded;
    }
    if (!present_succeeded) {
        return bench_outcome::failed;
    }
    return bench_outcome::success;
}

// True only for an accepted, successful present. Latency samples must be
// recorded exclusively for outcomes where this returns true.
inline bool is_latency_accepted(bench_outcome outcome) noexcept {
    return outcome == bench_outcome::success;
}

// ---------------------------------------------------------------------------
// Bounded option constants (single source of truth for parser and validator)
//
// These are the operational safety bounds: they cap the number of automated
// navigation steps a single run may schedule. Both the CLI parser and
// validate_nav_bench_options() consume these constants, so a value accepted at
// parse time is always accepted by validation and vice versa.
// ---------------------------------------------------------------------------

inline constexpr std::uint32_t kNavBenchMinTimeoutMs = 1;
inline constexpr std::uint32_t kNavBenchMaxTimeoutMs = 86400000;  // 24 h per request
inline constexpr std::uint32_t kNavBenchMinRepetitions = 1;
inline constexpr std::uint32_t kNavBenchMaxRepetitions = 1000;
inline constexpr std::uint32_t kNavBenchMinRapidBurst = 1;
inline constexpr std::uint32_t kNavBenchMaxRapidBurst = 100;

// ---------------------------------------------------------------------------
// Scenario contract: labels, setup/re-arm, and latency-sample eligibility
//
// Every scenario begins with an explicit test-only setup/re-arm phase. Setup
// transitions are labeled with kNavBenchScenarioSetupRearm and are excluded
// from latency summaries: they never receive a successful_present latency
// sample. A scenario therefore never depends on where the previous scenario
// left the cursor.
// ---------------------------------------------------------------------------

inline constexpr std::string_view kNavBenchScenarioInitialWarmup = "initial_warmup";
inline constexpr std::string_view kNavBenchScenarioCachedNext = "cached_next";
inline constexpr std::string_view kNavBenchScenarioUncached24MpNext = "uncached_24mp_next";
inline constexpr std::string_view kNavBenchScenarioRapidReplacement = "rapid_replacement";
inline constexpr std::string_view kNavBenchScenarioForwardBackSweep = "forward_back_sweep";
inline constexpr std::string_view kNavBenchScenarioCacheMemorySweep = "cache_memory_sweep";
inline constexpr std::string_view kNavBenchScenarioSetupRearm = "setup_rearm";

// Controlled uncached 24MP scenario bound: at most this many measured
// request-time-miss samples, each with its own re-arm/drain/purge setup.
inline constexpr std::uint32_t kNavBenchMaxUncached24MpSteps = 10;

// The corpus contract reserves indices 0..49 for 24MP (6000x4000) images.
inline constexpr std::size_t kNavBench24MpIndexLimit = 50;
inline constexpr std::int32_t kNavBench24MpMinWidth = 6000;
inline constexpr std::int32_t kNavBench24MpMinHeight = 4000;

inline bool nav_bench_is_24mp_image(std::int32_t width, std::int32_t height) noexcept {
    return width >= kNavBench24MpMinWidth && height >= kNavBench24MpMinHeight;
}

// Reason that a successful present must NOT enter its scenario's latency
// summary; an empty view means the sample is eligible.
//  - cached_next        : request-time hit AND present committed the requested target.
//  - uncached_24mp_next : request-time miss AND committed target is a 24MP image
//                         AND the present committed the requested target.
//  - every other scenario: any still-current successful present.
// Ineligible successful presents stay in the stream (audited as a labeled
// step/excluded task event) but never contribute percentiles.
inline std::string_view nav_bench_sample_ineligibility_reason(
    std::string_view scenario, bench_cache_state cache_state,
    bool committed_is_24mp, bool committed_matches_request) noexcept {
    if (scenario == kNavBenchScenarioCachedNext) {
        if (cache_state != bench_cache_state::hit_at_request) {
            return "cache_state_not_hit_at_request";
        }
        if (!committed_matches_request) {
            return "committed_index_mismatch";
        }
        return {};
    }
    if (scenario == kNavBenchScenarioUncached24MpNext) {
        if (cache_state != bench_cache_state::miss_at_request) {
            return "cache_state_not_miss_at_request";
        }
        if (!committed_is_24mp) {
            return "committed_target_not_24mp";
        }
        if (!committed_matches_request) {
            return "committed_index_mismatch";
        }
        return {};
    }
    return {};
}

// Allowed OS/protocol cache conditions. This is a protocol label only: the
// benchmark never purges the OS cache and never combines cold and warm results.
inline constexpr std::string_view kOsCacheUncontrolled = "uncontrolled";
inline constexpr std::string_view kOsCacheColdProtocol = "cold_protocol";
inline constexpr std::string_view kOsCacheWarmProtocol = "warm_protocol";

inline bool is_valid_os_cache_condition(std::string_view condition) noexcept {
    return condition == kOsCacheUncontrolled || condition == kOsCacheColdProtocol ||
           condition == kOsCacheWarmProtocol;
}

}  // namespace oz::viewer_core

#pragma once

#include "oz/viewer_core/navigation_benchmark_policy.hpp"
#include "oz/viewer_core/navigation_benchmark_schema.hpp"
#include "oz/viewer_core/navigation_benchmark_corpus.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace oz::viewer_core {

// Phase / scenario enumeration for navigation benchmark state machine
enum class nav_bench_phase {
    uninitialized,
    initial_warmup,
    cached_next_run,
    controlled_uncached_24mp_run,
    rapid_replacement_run,
    forward_back_sweep,
    cache_memory_sweep,
    finished
};

struct nav_bench_options {
    bool enabled = false;
    std::filesystem::path corpus_dir;
    std::filesystem::path output_path;
    bool overwrite = false;
    std::string os_cache_condition = "uncontrolled";
    std::uint32_t timeout_ms = 10000;
    std::uint32_t repetitions = 5;
    std::uint32_t rapid_burst = 10;
};

// Parse command line arguments for --navigation-benchmark
// Returns true if benchmark mode requested and options parsed and validated
// cleanly, false if syntax error / out-of-bounds value / not requested.
bool parse_navigation_benchmark_args(
    int argc, wchar_t* const* argv,
    nav_bench_options& out_opts,
    std::string& out_error);

// Result / exit codes for --navigation-benchmark
enum class nav_bench_status {
    success = 0,
    invalid_args = 1,
    corpus_invalid = 2,
    output_exists = 3,
    desktop_unavailable = 4,
    execution_failed = 5,
    timed_out = 6
};

// Validates bounded options (timeouts, repetition counts, cache-condition
// label and required paths) before any corpus or output work. Returns an empty
// string on success, or a human-readable reason. Uses the shared
// kNavBench* constants as the single source of truth for limits.
std::string validate_nav_bench_options(const nav_bench_options& opts);

// ---------------------------------------------------------------------------
// State-machine action contract (test-only navigation benchmark)
//
// The session never issues a measured navigation request while a scenario's
// setup/re-arm preconditions are unsatisfied. Setup actions (re-arm, drain,
// cache verification) are labeled and never produce latency samples.
// ---------------------------------------------------------------------------

// Kind of action the benchmark state machine asks the UI thread to perform.
enum class nav_bench_step_kind : std::uint8_t {
    idle,         // nothing to do this tick; pump again after state changes
    navigation,   // issue vk_key through the ordinary shared navigation path
    rearm,        // test-only direct selection to target_index (labeled setup)
    await_cache,  // wait until target_index residency matches expect_target_present
    await_drain,  // wait until decode work and completions are drained
    finished      // state machine complete; caller finishes the run
};

struct nav_bench_step {
    nav_bench_step_kind kind = nav_bench_step_kind::idle;
    int vk_key = 0;
    bool quiesce = false;               // measured step waits for a terminal outcome
    bool purge_app_cache = false;       // clear cache except the pinned current image
    std::size_t target_index = 0;       // rearm / await_cache target
    bool expect_target_present = false; // await_cache: resident (true) or evicted (false)
    std::string scenario;               // scenario label of the issued request
};

// Viewer state observed on the UI thread and consumed by the state machine.
// `target_present` must be evaluated for the most recent await_cache target.
struct nav_bench_viewer_state {
    std::size_t list_size = 0;        // navigation list size
    std::size_t committed_index = 0;  // currently committed/presented index
    bool work_drained = true;         // no queued/in-flight decode or unprocessed completion
    bool target_present = false;      // cache residency of the last await_cache target
};

// Cache/memory statistics fed by the UI-owned cache. `entries` counts cached
// (path, profile) variants; `resident` counts distinct decoded source images.
// The two are reported separately and must never be conflated.
struct nav_cache_stats {
    std::size_t used_bytes = 0;
    std::size_t budget_bytes = 0;
    std::size_t entry_count = 0;
    std::size_t resident_decoded_count = 0;
};

class navigation_benchmark_session {
public:
    navigation_benchmark_session() = default;
    ~navigation_benchmark_session();

    // QueryPerformanceCounter accessor shared with the UI integration so the
    // present endpoint is stamped immediately after a successful Present.
    static std::uint64_t query_ticks() noexcept;

    // Initialize session: validates options, corpus and output path, then
    // opens the JSONL writer with create-new semantics (race-safe; refuses an
    // existing file unless `overwrite` is set) and emits the header records.
    // `prevalidated` may supply a corpus validation already performed before
    // window creation; when null the corpus is validated here.
    nav_bench_status init(const nav_bench_options& opts,
                          std::int32_t viewport_w,
                          std::int32_t viewport_h,
                          float dpi,
                          bool warp,
                          const corpus_validation_result* prevalidated = nullptr);

    bool is_active() const noexcept { return active_; }
    nav_bench_phase current_phase() const noexcept { return phase_; }
    const std::string& current_scenario_label() const noexcept;
    const std::string& timeout_reason() const noexcept { return timeout_reason_; }
    bool timed_out() const noexcept { return abort_timed_out_; }

    // Called on navigation request trigger (shared with UI WM_KEYDOWN)
    // Returns assigned request_id
    std::uint64_t record_navigation_request(
        const std::string& scenario,
        const std::string& direction,
        std::size_t requested_target_index,
        const std::string& requested_target_alias,
        std::size_t from_index,
        const std::string& from_alias,
        bench_cache_state cache_state,
        std::uint64_t epoch);

    // Called immediately after successful swap-chain Present on UI thread
    // (present_ticks is stamped right after Present returns; 0 means "now").
    // Only a present matched to a real, still-current outstanding request
    // produces a latency sample. Unmatched, duplicate and superseded presents
    // are recorded as categorized task events and never as latency.
    void record_successful_present(
        std::uint64_t request_id,
        std::uint64_t present_ticks,
        std::size_t committed_index,
        const std::string& committed_alias,
        const std::string& committed_sha256,
        std::size_t cache_budget,
        std::size_t cache_used,
        std::size_t cache_entries,
        std::size_t resident_decoded);

    // Called when a pending request is superseded by newer user request
    void record_request_superseded(std::uint64_t request_id, std::uint64_t epoch, const std::string& reason);

    // Called when a request fails or times out
    void record_request_failed(std::uint64_t request_id, std::uint64_t epoch, const std::string& reason);
    void record_request_timed_out(std::uint64_t request_id, std::uint64_t epoch, const std::string& reason);

    // Called when an automated step could not issue a navigation request
    // (empty list, already at boundary, non-navigation key). Clears the wait
    // for the step and records a categorized no_op task event.
    void note_step_no_op(const std::string& scenario, const std::string& direction,
                         const std::string& reason);

    // UI-owned cache statistics used for memory samples.
    void update_cache_stats(std::size_t used_bytes, std::size_t budget_bytes,
                            std::size_t entry_count, std::size_t resident_decoded);

    // Advance the scenario state machine using the latest viewer state.
    // Setup/re-arm actions carry no latency sample; a measured navigation
    // action is only produced once every setup precondition is satisfied.
    // Returns a `finished` step when all scenarios are complete.
    nav_bench_step pump_benchmark_step(const nav_bench_viewer_state& viewer_state);

    // True while the current quiesced step (or a post-burst settle step) is
    // still waiting for its request to reach a terminal outcome
    // (present/superseded/failed/timed out).
    bool step_pending() const noexcept;

    // Bounded watchdog: times out any in-flight request older than
    // timeout_ms as well as any unsatisfied scenario setup wait, records a
    // categorized timed_out task event, and returns true when the run must
    // stop with status "timed_out".
    bool enforce_watchdog();

    // Finish benchmark, emit scenario summaries and run_end
    void finish(const std::string& final_status, const std::string& error_reason);

    const nav_bench_options& options() const noexcept { return opts_; }
    const corpus_validation_result& corpus() const noexcept { return corpus_val_; }
    const std::string& run_id() const noexcept { return run_id_; }

private:
    static constexpr std::size_t kNoSetupIndex = static_cast<std::size_t>(-1);

    // A single scenario setup precondition. Setup actions are never measured.
    enum class setup_kind : std::uint8_t {
        rearm,        // direct selection to `index`
        drain,        // wait for decode/completion quiescence
        cache_verify  // wait until residency of `index` matches `expect_present`
    };
    struct setup_item {
        setup_kind kind = setup_kind::rearm;
        std::size_t index = 0;
        bool expect_present = false;
        bool purge_before_verify = false;
        std::size_t serial = 0;  // identifies one verification handshake
    };

    void write_jsonl_line(const json::value& v);
    void emit_memory_sample(const std::string& phase);
    void begin_settle_step();
    void clear_awaiting(std::uint64_t request_id) noexcept;

    nav_bench_step make_idle_step() const;
    nav_bench_step make_finished_step() const;
    nav_bench_step make_navigation_step(int vk_key, bool quiesce,
                                        std::string_view scenario);
    nav_bench_step make_await_cache_step(const setup_item& item) const;
    nav_bench_step make_rearm_step(const setup_item& item) const;
    nav_bench_step make_await_drain_step() const;

    // Processes queued setup items for the current measured step. Returns the
    // next setup action while a precondition is unsatisfied, or nullopt when
    // every precondition is satisfied (or no setup is required).
    std::optional<nav_bench_step> process_setup(const nav_bench_viewer_state& viewer_state);
    setup_item make_setup_item(setup_kind kind, std::size_t index,
                               bool expect_present, bool purge_before_verify);

    bool settle_before_transition();
    void reset_setup();
    void start_setup_wait();
    void audit_setup_rearm(const setup_item& item);

    void begin_cached_step_setup(const nav_bench_viewer_state& viewer_state);
    void begin_uncached_pairs(std::size_t list_size);
    void begin_uncached_step_setup(std::size_t rearm_index, std::size_t target_index);
    void begin_rapid_setup();
    void begin_forward_back_setup(const nav_bench_viewer_state& viewer_state);
    void begin_memory_sweep_setup(const nav_bench_viewer_state& viewer_state);

    std::size_t rapid_burst_steps(std::size_t list_size) const noexcept;
    std::size_t forward_back_rearm_index(std::size_t list_size) const noexcept;

    bool active_ = false;
    nav_bench_options opts_;
    corpus_validation_result corpus_val_;
    std::string run_id_;
    std::ofstream out_file_;

    std::uint64_t qpc_frequency_ = 1;
    std::uint64_t session_start_ticks_ = 0;
    std::uint64_t next_request_id_ = 1;
    std::uint64_t current_epoch_ = 0;  // latest user epoch observed

    nav_bench_phase phase_ = nav_bench_phase::uninitialized;
    std::size_t phase_step_ = 0;
    std::size_t total_requests_ = 0;
    std::size_t total_presents_ = 0;
    std::size_t total_superseded_ = 0;
    std::size_t total_failed_ = 0;
    std::size_t total_timed_out_ = 0;
    std::size_t total_no_ops_ = 0;

    // Step/watchdog state
    bool expect_request_for_step_ = false;  // next recorded request is awaited
    bool awaiting_present_ = false;         // current step is waiting for a terminal outcome
    std::uint64_t awaited_request_id_ = 0;
    bool abort_timed_out_ = false;
    std::string timeout_reason_;

    // Scenario setup/re-arm state (test-only). Items are processed in order;
    // measured navigation is issued only after every item is satisfied.
    std::vector<setup_item> setup_items_;
    std::size_t setup_pos_ = 0;
    bool setup_pending_ = false;
    bool setup_ready_ = false;
    std::size_t setup_serial_ = 0;
    std::size_t cache_verify_confirmed_serial_ = kNoSetupIndex;
    std::size_t rearm_audited_index_ = kNoSetupIndex;
    bool setup_wait_active_ = false;
    std::uint64_t setup_wait_start_ticks_ = 0;

    // Controlled uncached 24MP scenario: re-arm sources whose successor is a
    // known 24MP image inside the corpus' 0..49 reservation.
    std::vector<std::size_t> uncached_rearm_indices_;
    bool uncached_pairs_ready_ = false;

    nav_cache_stats cache_stats_;

    struct in_flight_request {
        std::uint64_t request_id = 0;
        std::uint64_t epoch = 0;
        std::string scenario;
        std::string os_cache_condition;
        std::size_t requested_target_index = 0;
        std::string requested_target_alias;
        bench_cache_state cache_state = bench_cache_state::miss_at_request;
        std::uint64_t request_ticks = 0;
        std::int64_t request_rel_ns = 0;
        bool completed = false;
    };
    std::vector<in_flight_request> pending_requests_;

    in_flight_request* find_active_request(std::uint64_t request_id) noexcept;

    // Latency accumulation per scenario for summary computation
    struct scenario_tracker {
        std::size_t requested = 0;
        std::size_t superseded = 0;
        std::size_t failed = 0;
        std::size_t timed_out = 0;
        std::size_t no_ops = 0;
        std::vector<std::int64_t> accepted_latencies_ns;
    };
    std::unordered_map<std::string, scenario_tracker> scenarios_;
};

}  // namespace oz::viewer_core

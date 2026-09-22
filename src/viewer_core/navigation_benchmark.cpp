#include "oz/viewer_core/navigation_benchmark.hpp"
#include "oz/runner/utf.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace oz::viewer_core {

std::string validate_nav_bench_options(const nav_bench_options& opts) {
    if (opts.corpus_dir.empty()) {
        return "corpus directory is required";
    }
    if (opts.output_path.empty()) {
        return "output path is required";
    }
    if (opts.timeout_ms < kNavBenchMinTimeoutMs || opts.timeout_ms > kNavBenchMaxTimeoutMs) {
        return "timeout_ms must be in [" + std::to_string(kNavBenchMinTimeoutMs) + ", " +
               std::to_string(kNavBenchMaxTimeoutMs) + "]";
    }
    if (opts.repetitions < kNavBenchMinRepetitions ||
        opts.repetitions > kNavBenchMaxRepetitions) {
        return "repetitions must be in [" + std::to_string(kNavBenchMinRepetitions) + ", " +
               std::to_string(kNavBenchMaxRepetitions) + "]";
    }
    if (opts.rapid_burst < kNavBenchMinRapidBurst || opts.rapid_burst > kNavBenchMaxRapidBurst) {
        return "rapid_burst must be in [" + std::to_string(kNavBenchMinRapidBurst) + ", " +
               std::to_string(kNavBenchMaxRapidBurst) + "]";
    }
    if (!is_valid_os_cache_condition(opts.os_cache_condition)) {
        return "os_cache_condition must be uncontrolled|cold_protocol|warm_protocol";
    }
    return {};
}

namespace {

std::string generate_run_id() {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    gmtime_s(&tm_buf, &tt);
    char buf[64];
    std::strftime(buf, sizeof(buf), "navbench-%Y%m%d-%H%M%S", &tm_buf);
    return std::string(buf);
}

std::uint64_t get_qpc_ticks() {
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);
    return static_cast<std::uint64_t>(li.QuadPart);
}

std::uint64_t get_qpc_freq() {
    LARGE_INTEGER li{};
    QueryPerformanceFrequency(&li);
    return static_cast<std::uint64_t>(li.QuadPart);
}

std::int64_t ticks_to_ns(std::uint64_t ticks_diff, std::uint64_t freq) {
    if (freq == 0) return 0;
    return static_cast<std::int64_t>((static_cast<double>(ticks_diff) * 1e9) / static_cast<double>(freq));
}

}  // namespace

std::uint64_t navigation_benchmark_session::query_ticks() noexcept {
    return get_qpc_ticks();
}

const std::string& navigation_benchmark_session::current_scenario_label() const noexcept {
    static const std::string kUninitialized = "uninitialized";
    static const std::string kInitialWarmup(kNavBenchScenarioInitialWarmup);
    static const std::string kCachedNext(kNavBenchScenarioCachedNext);
    static const std::string kUncached24MpNext(kNavBenchScenarioUncached24MpNext);
    static const std::string kRapidReplacement(kNavBenchScenarioRapidReplacement);
    static const std::string kForwardBackSweep(kNavBenchScenarioForwardBackSweep);
    static const std::string kCacheMemorySweep(kNavBenchScenarioCacheMemorySweep);
    switch (phase_) {
        case nav_bench_phase::initial_warmup: return kInitialWarmup;
        case nav_bench_phase::cached_next_run: return kCachedNext;
        case nav_bench_phase::controlled_uncached_24mp_run: return kUncached24MpNext;
        case nav_bench_phase::rapid_replacement_run: return kRapidReplacement;
        case nav_bench_phase::forward_back_sweep: return kForwardBackSweep;
        case nav_bench_phase::cache_memory_sweep: return kCacheMemorySweep;
        case nav_bench_phase::uninitialized:
        case nav_bench_phase::finished:
        default:
            return kUninitialized;
    }
}

navigation_benchmark_session::~navigation_benchmark_session() {
    if (out_file_.is_open()) {
        out_file_.flush();
        out_file_.close();
    }
}

void navigation_benchmark_session::write_jsonl_line(const json::value& v) {
    if (!out_file_.is_open()) {
        return;
    }
    const std::string s = json::dump(v);
    out_file_ << s << "\n";
    out_file_.flush();
}

void navigation_benchmark_session::emit_memory_sample(const std::string& phase) {
    bench_memory_sample sample;
    sample.raw_ticks = get_qpc_ticks();
    sample.relative_ns = ticks_to_ns(sample.raw_ticks - session_start_ticks_, qpc_frequency_);
    query_process_memory(sample.private_bytes, sample.working_set_bytes, sample.peak_working_set_bytes);
    sample.cache_used_bytes = cache_stats_.used_bytes;
    sample.cache_budget_bytes = cache_stats_.budget_bytes;
    sample.cache_entry_count = cache_stats_.entry_count;
    sample.resident_decoded_count = cache_stats_.resident_decoded_count;

    write_jsonl_line(serialize_memory_sample(run_id_, phase, sample));
}

nav_bench_status navigation_benchmark_session::init(
    const nav_bench_options& opts,
    std::int32_t viewport_w,
    std::int32_t viewport_h,
    float dpi,
    bool warp,
    const corpus_validation_result* prevalidated) {
    opts_ = opts;
    const std::string option_error = validate_nav_bench_options(opts_);
    if (!option_error.empty()) {
        return nav_bench_status::invalid_args;
    }

    qpc_frequency_ = get_qpc_freq();
    if (qpc_frequency_ == 0) {
        qpc_frequency_ = 1;
    }
    session_start_ticks_ = get_qpc_ticks();
    run_id_ = generate_run_id();

    if (prevalidated != nullptr) {
        corpus_val_ = *prevalidated;
    } else {
        corpus_val_ = validate_navigation_corpus(opts_.corpus_dir);
    }
    if (!corpus_val_.ok) {
        return nav_bench_status::corpus_invalid;
    }

    // Race-safe create-new output handling: the helper requests atomic
    // create-new from the OS unless --overwrite was explicitly requested, so
    // two concurrent runs cannot both believe they own the file.
    const std::string open_error =
        open_benchmark_output(out_file_, opts_.output_path, opts_.overwrite);
    if (!open_error.empty()) {
        if (open_error.find("already exists") != std::string::npos) {
            return nav_bench_status::output_exists;
        }
        return nav_bench_status::execution_failed;
    }

    active_ = true;
    phase_ = nav_bench_phase::initial_warmup;
    phase_step_ = 0;
    expect_request_for_step_ = false;
    awaiting_present_ = false;
    awaited_request_id_ = 0;
    abort_timed_out_ = false;
    timeout_reason_.clear();
    cache_stats_ = nav_cache_stats{};
    reset_setup();
    uncached_rearm_indices_.clear();
    uncached_pairs_ready_ = false;
    setup_wait_active_ = false;
    setup_wait_start_ticks_ = 0;

    // 1. Emit run_start
    write_jsonl_line(serialize_run_start(
        run_id_,
        corpus_val_.manifest_sha256,
        corpus_val_.entries.size(),
        viewport_w,
        viewport_h,
        dpi,
        warp));

    // 2. Emit corpus records (with public aliases/ordinals/hashes, NO absolute paths)
    for (const auto& me : corpus_val_.entries) {
        write_jsonl_line(serialize_corpus_entry(
            me.alias,
            me.ordinal,
            me.sha256,
            me.width,
            me.height,
            me.file_bytes));
    }

    // Initial memory sample (cache statistics are still zero: the UI-owned
    // cache has not been configured yet at this point).
    emit_memory_sample("startup");

    return nav_bench_status::success;
}

std::uint64_t navigation_benchmark_session::record_navigation_request(
    const std::string& scenario,
    const std::string& direction,
    std::size_t requested_target_index,
    const std::string& requested_target_alias,
    std::size_t from_index,
    const std::string& from_alias,
    bench_cache_state cache_state,
    std::uint64_t epoch) {
    if (!active_) return 0;

    const std::uint64_t req_id = next_request_id_++;
    const std::uint64_t now_ticks = get_qpc_ticks();
    const std::int64_t rel_ns = ticks_to_ns(now_ticks - session_start_ticks_, qpc_frequency_);

    ++total_requests_;
    current_epoch_ = (std::max)(current_epoch_, epoch);
    scenarios_[scenario].requested++;

    in_flight_request ifr;
    ifr.request_id = req_id;
    ifr.epoch = epoch;
    ifr.scenario = scenario;
    ifr.os_cache_condition = opts_.os_cache_condition;
    ifr.requested_target_index = requested_target_index;
    ifr.requested_target_alias = requested_target_alias;
    ifr.cache_state = cache_state;
    ifr.request_ticks = now_ticks;
    ifr.request_rel_ns = rel_ns;
    ifr.completed = false;
    pending_requests_.push_back(ifr);

    // A quiesced automated step awaits the request it just issued.
    if (expect_request_for_step_) {
        expect_request_for_step_ = false;
        awaiting_present_ = true;
        awaited_request_id_ = req_id;
    }

    write_jsonl_line(serialize_navigation_request(
        run_id_,
        scenario,
        opts_.os_cache_condition,
        req_id,
        epoch,
        direction,
        requested_target_index,
        requested_target_alias,
        from_index,
        from_alias,
        cache_state,
        now_ticks,
        rel_ns));

    return req_id;
}

navigation_benchmark_session::in_flight_request*
navigation_benchmark_session::find_active_request(std::uint64_t request_id) noexcept {
    for (auto& req : pending_requests_) {
        if (!req.completed && req.request_id == request_id) {
            return &req;
        }
    }
    return nullptr;
}

void navigation_benchmark_session::clear_awaiting(std::uint64_t request_id) noexcept {
    if (awaiting_present_ && awaited_request_id_ == request_id) {
        awaiting_present_ = false;
        awaited_request_id_ = 0;
    }
}

void navigation_benchmark_session::record_successful_present(
    std::uint64_t request_id,
    std::uint64_t present_ticks,
    std::size_t committed_index,
    const std::string& committed_alias,
    const std::string& committed_sha256,
    std::size_t cache_budget,
    std::size_t cache_used,
    std::size_t cache_entries,
    std::size_t resident_decoded) {
    if (!active_) return;

    const std::uint64_t end_ticks =
        (present_ticks != 0) ? present_ticks : get_qpc_ticks();
    const std::int64_t rel_ns = ticks_to_ns(end_ticks - session_start_ticks_, qpc_frequency_);

    in_flight_request* matched = find_active_request(request_id);
    if (matched == nullptr) {
        // A present that cannot be tied to a real outstanding request is never
        // a latency sample. Distinguish a repeated present for an already
        // resolved request from a completely unattributed one.
        bool known_request = false;
        for (const auto& req : pending_requests_) {
            if (req.request_id == request_id) {
                known_request = true;
                break;
            }
        }
        write_jsonl_line(serialize_task_event(
            run_id_,
            "unattributed",
            request_id,
            current_epoch_,
            known_request ? "duplicate_present" : "unmatched_present",
            "ignored",
            known_request ? "request_already_resolved" : "no_outstanding_request",
            rel_ns));
        return;
    }

    const bench_outcome outcome =
        classify_request_outcome(matched->epoch, current_epoch_, true, false, false, false);
    matched->completed = true;
    clear_awaiting(matched->request_id);

    if (!is_latency_accepted(outcome)) {
        if (outcome == bench_outcome::superseded) {
            ++total_superseded_;
            scenarios_[matched->scenario].superseded++;
            write_jsonl_line(serialize_task_event(
                run_id_,
                matched->scenario,
                matched->request_id,
                matched->epoch,
                "superseded",
                "superseded",
                "present_for_superseded_request",
                rel_ns));
        } else {
            ++total_failed_;
            scenarios_[matched->scenario].failed++;
            write_jsonl_line(serialize_task_event(
                run_id_,
                matched->scenario,
                matched->request_id,
                matched->epoch,
                "failed",
                "failed",
                "present_outcome_not_accepted",
                rel_ns));
        }
        return;
    }

    const std::uint64_t latency_ticks =
        (end_ticks >= matched->request_ticks) ? (end_ticks - matched->request_ticks) : 0;
    const std::int64_t latency_ns = ticks_to_ns(latency_ticks, qpc_frequency_);
    ++total_presents_;

    // Scenario sample eligibility: cached_next accepts only request-time hits,
    // uncached_24mp_next only request-time misses on a committed 24MP target.
    // An ineligible successful present is auditable in the stream but never
    // enters latency statistics, so each scenario's percentiles are pure.
    bool committed_is_24mp = false;
    if (committed_index < corpus_val_.entries.size()) {
        const auto& entry = corpus_val_.entries[committed_index];
        committed_is_24mp = nav_bench_is_24mp_image(entry.width, entry.height);
    }
    const bool committed_matches_request =
        (committed_index == matched->requested_target_index);
    const std::string_view ineligibility = nav_bench_sample_ineligibility_reason(
        matched->scenario, matched->cache_state, committed_is_24mp,
        committed_matches_request);
    if (ineligibility.empty()) {
        scenarios_[matched->scenario].accepted_latencies_ns.push_back(latency_ns);
    } else {
        write_jsonl_line(serialize_task_event(
            run_id_,
            matched->scenario,
            matched->request_id,
            matched->epoch,
            "step",
            "excluded",
            std::string("sample_ineligible:") + std::string(ineligibility),
            rel_ns));
    }

    // app_cache_state is the state observed when the request was accepted
    // (stored on the in-flight request); it is never re-derived at present
    // time and is distinct from os_cache_condition.
    write_jsonl_line(serialize_successful_present(
        run_id_,
        matched->scenario,
        matched->os_cache_condition,
        matched->request_id,
        matched->epoch,
        committed_index,
        committed_alias,
        committed_sha256,
        matched->cache_state,
        matched->request_ticks,
        end_ticks,
        latency_ns,
        cache_budget,
        cache_used,
        cache_entries,
        resident_decoded));
}

void navigation_benchmark_session::record_request_superseded(
    std::uint64_t request_id, std::uint64_t epoch, const std::string& reason) {
    (void)epoch;  // the matched request carries the authoritative epoch
    if (!active_) return;

    in_flight_request* matched = find_active_request(request_id);
    if (matched == nullptr) {
        return;
    }
    matched->completed = true;
    clear_awaiting(matched->request_id);
    ++total_superseded_;
    scenarios_[matched->scenario].superseded++;

    const std::uint64_t now_ticks = get_qpc_ticks();
    const std::int64_t rel_ns = ticks_to_ns(now_ticks - session_start_ticks_, qpc_frequency_);

    write_jsonl_line(serialize_task_event(
        run_id_,
        matched->scenario,
        matched->request_id,
        matched->epoch,
        "superseded",
        "superseded",
        reason,
        rel_ns));
}

void navigation_benchmark_session::record_request_failed(
    std::uint64_t request_id, std::uint64_t epoch, const std::string& reason) {
    (void)epoch;  // the matched request carries the authoritative epoch
    if (!active_) return;

    in_flight_request* matched = find_active_request(request_id);
    if (matched == nullptr) {
        return;
    }
    matched->completed = true;
    clear_awaiting(matched->request_id);
    ++total_failed_;
    scenarios_[matched->scenario].failed++;

    const std::uint64_t now_ticks = get_qpc_ticks();
    const std::int64_t rel_ns = ticks_to_ns(now_ticks - session_start_ticks_, qpc_frequency_);

    write_jsonl_line(serialize_task_event(
        run_id_,
        matched->scenario,
        matched->request_id,
        matched->epoch,
        "failed",
        "failed",
        reason,
        rel_ns));
}

void navigation_benchmark_session::record_request_timed_out(
    std::uint64_t request_id, std::uint64_t epoch, const std::string& reason) {
    (void)epoch;  // the matched request carries the authoritative epoch
    if (!active_) return;

    in_flight_request* matched = find_active_request(request_id);
    if (matched == nullptr) {
        return;
    }
    matched->completed = true;
    clear_awaiting(matched->request_id);
    ++total_timed_out_;
    scenarios_[matched->scenario].timed_out++;

    const std::uint64_t now_ticks = get_qpc_ticks();
    const std::int64_t rel_ns = ticks_to_ns(now_ticks - session_start_ticks_, qpc_frequency_);

    write_jsonl_line(serialize_task_event(
        run_id_,
        matched->scenario,
        matched->request_id,
        matched->epoch,
        "timed_out",
        "timed_out",
        reason,
        rel_ns));
}

void navigation_benchmark_session::note_step_no_op(
    const std::string& scenario, const std::string& direction, const std::string& reason) {
    if (!active_) return;
    (void)direction;  // retained for API symmetry with navigation_request records

    expect_request_for_step_ = false;
    ++total_no_ops_;
    scenarios_[scenario].no_ops++;

    const std::uint64_t now_ticks = get_qpc_ticks();
    const std::int64_t rel_ns = ticks_to_ns(now_ticks - session_start_ticks_, qpc_frequency_);

    write_jsonl_line(serialize_task_event(
        run_id_,
        scenario,
        0,
        current_epoch_,
        "no_op",
        "no_op",
        reason.empty() ? std::string("no_navigation_change") : reason,
        rel_ns));
}

void navigation_benchmark_session::update_cache_stats(
    std::size_t used_bytes, std::size_t budget_bytes,
    std::size_t entry_count, std::size_t resident_decoded) {
    cache_stats_.used_bytes = used_bytes;
    cache_stats_.budget_bytes = budget_bytes;
    cache_stats_.entry_count = entry_count;
    cache_stats_.resident_decoded_count = resident_decoded;
}

void navigation_benchmark_session::begin_settle_step() {
    expect_request_for_step_ = false;
    awaiting_present_ = false;
    awaited_request_id_ = 0;

    // The newest still-outstanding request becomes the settle target. If none
    // is outstanding (for example a rapid burst whose last request already
    // presented), the settle step completes without an extra wait.
    for (auto it = pending_requests_.rbegin(); it != pending_requests_.rend(); ++it) {
        if (!it->completed) {
            awaiting_present_ = true;
            awaited_request_id_ = it->request_id;
            return;
        }
    }
}

bool navigation_benchmark_session::step_pending() const noexcept {
    if (!active_ || !awaiting_present_ || awaited_request_id_ == 0) {
        return false;
    }
    for (const auto& req : pending_requests_) {
        if (!req.completed && req.request_id == awaited_request_id_) {
            return true;
        }
    }
    return false;
}

bool navigation_benchmark_session::enforce_watchdog() {
    if (!active_) {
        return false;
    }
    const std::int64_t timeout_ns =
        static_cast<std::int64_t>(opts_.timeout_ms) * 1000000;
    const std::uint64_t now_ticks = get_qpc_ticks();

    bool expired_any = false;
    for (auto& req : pending_requests_) {
        if (req.completed) {
            continue;
        }
        const std::int64_t elapsed_ns =
            ticks_to_ns(now_ticks - req.request_ticks, qpc_frequency_);
        if (elapsed_ns > timeout_ns) {
            req.completed = true;
            ++total_timed_out_;
            scenarios_[req.scenario].timed_out++;
            const std::int64_t rel_ns =
                ticks_to_ns(now_ticks - session_start_ticks_, qpc_frequency_);
            write_jsonl_line(serialize_task_event(
                run_id_,
                req.scenario,
                req.request_id,
                req.epoch,
                "timed_out",
                "timed_out",
                "request_exceeded_timeout_ms",
                rel_ns));
            expired_any = true;
        }
    }

    // A step waiting on a request that has now resolved must not keep waiting.
    if (awaiting_present_ && !step_pending()) {
        awaiting_present_ = false;
        awaited_request_id_ = 0;
    }

    // Scenario setup/re-arm waits are bounded by the same timeout: a preload
    // that never becomes resident, or a re-arm that never commits, must not
    // hang the run with no categorized evidence.
    if (!abort_timed_out_ && setup_wait_active_) {
        const std::int64_t setup_elapsed_ns =
            ticks_to_ns(now_ticks - setup_wait_start_ticks_, qpc_frequency_);
        if (setup_elapsed_ns > timeout_ns) {
            setup_wait_active_ = false;
            const std::string setup_label = current_scenario_label();
            ++total_timed_out_;
            scenarios_[setup_label].timed_out++;
            const std::int64_t rel_ns =
                ticks_to_ns(now_ticks - session_start_ticks_, qpc_frequency_);
            write_jsonl_line(serialize_task_event(
                run_id_,
                setup_label,
                0,
                current_epoch_,
                "timed_out",
                "timed_out",
                "setup_wait_exceeded_timeout_ms",
                rel_ns));
            abort_timed_out_ = true;
            timeout_reason_ = "setup_wait_exceeded_timeout_ms";
        }
    }

    if (expired_any) {
        abort_timed_out_ = true;
        timeout_reason_ = "request_exceeded_timeout_ms";
    }
    return abort_timed_out_;
}

// ---------------------------------------------------------------------------
// Scenario setup/re-arm helpers
// ---------------------------------------------------------------------------

nav_bench_step navigation_benchmark_session::make_idle_step() const {
    return nav_bench_step{};
}

nav_bench_step navigation_benchmark_session::make_finished_step() const {
    nav_bench_step step;
    step.kind = nav_bench_step_kind::finished;
    return step;
}

nav_bench_step navigation_benchmark_session::make_navigation_step(
    int vk_key, bool quiesce, std::string_view scenario) {
    nav_bench_step step;
    step.kind = nav_bench_step_kind::navigation;
    step.vk_key = vk_key;
    step.quiesce = quiesce;
    step.scenario = std::string(scenario);
    if (quiesce) {
        expect_request_for_step_ = true;
    }
    return step;
}

nav_bench_step navigation_benchmark_session::make_await_cache_step(
    const setup_item& item) const {
    nav_bench_step step;
    step.kind = nav_bench_step_kind::await_cache;
    step.target_index = item.index;
    step.expect_target_present = item.expect_present;
    step.purge_app_cache = item.purge_before_verify;
    step.scenario = current_scenario_label();
    return step;
}

nav_bench_step navigation_benchmark_session::make_rearm_step(const setup_item& item) const {
    nav_bench_step step;
    step.kind = nav_bench_step_kind::rearm;
    step.target_index = item.index;
    step.scenario = std::string(kNavBenchScenarioSetupRearm);
    return step;
}

nav_bench_step navigation_benchmark_session::make_await_drain_step() const {
    nav_bench_step step;
    step.kind = nav_bench_step_kind::await_drain;
    step.scenario = current_scenario_label();
    return step;
}

navigation_benchmark_session::setup_item navigation_benchmark_session::make_setup_item(
    setup_kind kind, std::size_t index, bool expect_present, bool purge_before_verify) {
    setup_item item;
    item.kind = kind;
    item.index = index;
    item.expect_present = expect_present;
    item.purge_before_verify = purge_before_verify;
    item.serial = ++setup_serial_;
    return item;
}

void navigation_benchmark_session::reset_setup() {
    setup_items_.clear();
    setup_pos_ = 0;
    setup_pending_ = false;
    setup_ready_ = false;
    cache_verify_confirmed_serial_ = kNoSetupIndex;
    rearm_audited_index_ = kNoSetupIndex;
    setup_wait_active_ = false;
}

void navigation_benchmark_session::start_setup_wait() {
    if (!setup_wait_active_) {
        setup_wait_active_ = true;
        setup_wait_start_ticks_ = get_qpc_ticks();
    }
}

void navigation_benchmark_session::audit_setup_rearm(const setup_item& item) {
    if (rearm_audited_index_ == item.index) {
        return;
    }
    rearm_audited_index_ = item.index;
    const std::uint64_t now_ticks = get_qpc_ticks();
    const std::int64_t rel_ns = ticks_to_ns(now_ticks - session_start_ticks_, qpc_frequency_);
    write_jsonl_line(serialize_task_event(
        run_id_,
        std::string(kNavBenchScenarioSetupRearm),
        0,
        current_epoch_,
        "step",
        "setup",
        "rearm_to_index:" + std::to_string(item.index),
        rel_ns));
}

std::optional<nav_bench_step> navigation_benchmark_session::process_setup(
    const nav_bench_viewer_state& viewer_state) {
    while (setup_pending_) {
        if (setup_pos_ >= setup_items_.size()) {
            setup_pending_ = false;
            setup_ready_ = true;
            setup_wait_active_ = false;
            return std::nullopt;
        }
        const setup_item& item = setup_items_[setup_pos_];
        switch (item.kind) {
            case setup_kind::rearm: {
                if (viewer_state.committed_index == item.index) {
                    ++setup_pos_;
                    rearm_audited_index_ = kNoSetupIndex;
                    setup_wait_active_ = false;
                    continue;
                }
                audit_setup_rearm(item);
                start_setup_wait();
                return make_rearm_step(item);
            }
            case setup_kind::drain: {
                if (viewer_state.work_drained) {
                    ++setup_pos_;
                    setup_wait_active_ = false;
                    continue;
                }
                start_setup_wait();
                return make_await_drain_step();
            }
            case setup_kind::cache_verify: {
                if (cache_verify_confirmed_serial_ != item.serial) {
                    // First encounter: hand the target to the UI so it can
                    // evaluate residency for this exact target (purging first
                    // when the scenario requires a verified miss).
                    cache_verify_confirmed_serial_ = item.serial;
                    start_setup_wait();
                    return make_await_cache_step(item);
                }
                if (viewer_state.target_present == item.expect_present) {
                    ++setup_pos_;
                    cache_verify_confirmed_serial_ = kNoSetupIndex;
                    setup_wait_active_ = false;
                    continue;
                }
                // Not yet as required (or a late completion landed after a
                // purge): re-issue; the UI purges again when requested.
                start_setup_wait();
                return make_await_cache_step(item);
            }
        }
    }
    setup_ready_ = true;
    return std::nullopt;
}

bool navigation_benchmark_session::settle_before_transition() {
    // Every measured quiesced step resolves its own request; the rapid burst
    // is the only producer of an outstanding request at a transition. A
    // re-arm/verify wait never coexists with an outstanding request.
    begin_settle_step();
    return !step_pending();
}

void navigation_benchmark_session::begin_cached_step_setup(
    const nav_bench_viewer_state& viewer_state) {
    reset_setup();
    setup_pending_ = true;
    if (viewer_state.list_size < 2) {
        // Empty/single-item list: no forward step exists; the caller issues
        // the key and records a truthful no-op.
        return;
    }
    const std::size_t idx = (viewer_state.committed_index < viewer_state.list_size)
                                ? viewer_state.committed_index
                                : viewer_state.list_size - 1;
    if (idx + 1 >= viewer_state.list_size) {
        // No forward runway remains: re-arm to the start of the list so the
        // measured Next is a valid ordinary step instead of a boundary no-op.
        setup_items_.push_back(make_setup_item(setup_kind::rearm, 0, false, false));
        setup_items_.push_back(make_setup_item(setup_kind::cache_verify, 1, true, false));
    } else {
        // Wait for the verified preload of the next target before the measured
        // request, so every cached_next sample is a pure request-time hit.
        setup_items_.push_back(
            make_setup_item(setup_kind::cache_verify, idx + 1, true, false));
    }
}

void navigation_benchmark_session::begin_uncached_pairs(std::size_t list_size) {
    uncached_rearm_indices_.clear();
    const auto& entries = corpus_val_.entries;
    const std::size_t limit =
        (std::min)({entries.size(), list_size, kNavBench24MpIndexLimit});
    for (std::size_t i = 0;
         i + 1 < limit && uncached_rearm_indices_.size() < kNavBenchMaxUncached24MpSteps;
         ++i) {
        if (nav_bench_is_24mp_image(entries[i].width, entries[i].height) &&
            nav_bench_is_24mp_image(entries[i + 1].width, entries[i + 1].height)) {
            uncached_rearm_indices_.push_back(i);
        }
    }
    uncached_pairs_ready_ = true;
}

void navigation_benchmark_session::begin_uncached_step_setup(std::size_t rearm_index,
                                                             std::size_t target_index) {
    reset_setup();
    setup_pending_ = true;
    setup_items_.push_back(make_setup_item(setup_kind::rearm, rearm_index, false, false));
    setup_items_.push_back(make_setup_item(setup_kind::drain, 0, false, false));
    setup_items_.push_back(
        make_setup_item(setup_kind::cache_verify, target_index, false, true));
}

void navigation_benchmark_session::begin_rapid_setup() {
    reset_setup();
    setup_pending_ = true;
    // Re-arm to the start of the list; the burst length is bounded by the
    // remaining forward runway so no burst request is a boundary no-op.
    setup_items_.push_back(make_setup_item(setup_kind::rearm, 0, false, false));
}

void navigation_benchmark_session::begin_forward_back_setup(
    const nav_bench_viewer_state& viewer_state) {
    reset_setup();
    setup_pending_ = true;
    if (viewer_state.list_size >= 2) {
        setup_items_.push_back(make_setup_item(
            setup_kind::rearm, forward_back_rearm_index(viewer_state.list_size), false,
            false));
    }
}

void navigation_benchmark_session::begin_memory_sweep_setup(
    const nav_bench_viewer_state& viewer_state) {
    reset_setup();
    setup_pending_ = true;
    if (viewer_state.list_size >= 2) {
        setup_items_.push_back(
            make_setup_item(setup_kind::rearm, viewer_state.list_size - 1, false, false));
    }
}

std::size_t navigation_benchmark_session::rapid_burst_steps(
    std::size_t list_size) const noexcept {
    if (list_size < 2) {
        return 0;
    }
    return (std::min)(static_cast<std::size_t>(opts_.rapid_burst), list_size - 1);
}

std::size_t navigation_benchmark_session::forward_back_rearm_index(
    std::size_t list_size) const noexcept {
    if (list_size < 3) {
        return 0;
    }
    return list_size / 2;
}

nav_bench_step navigation_benchmark_session::pump_benchmark_step(
    const nav_bench_viewer_state& viewer_state) {
    if (!active_ || abort_timed_out_) {
        return make_idle_step();
    }
    if (step_pending()) {
        return make_idle_step();
    }

    for (;;) {
        if (setup_pending_) {
            if (auto setup_step = process_setup(viewer_state)) {
                return *setup_step;
            }
        }

        switch (phase_) {
            case nav_bench_phase::initial_warmup: {
                // The initial image is already presented before the timer
                // starts; each scenario owns its explicit re-arm.
                phase_ = nav_bench_phase::cached_next_run;
                phase_step_ = 0;
                reset_setup();
                continue;
            }
            case nav_bench_phase::cached_next_run: {
                if (phase_step_ >= opts_.repetitions) {
                    if (!settle_before_transition()) {
                        return make_idle_step();
                    }
                    emit_memory_sample("post_cached_next");
                    phase_ = nav_bench_phase::controlled_uncached_24mp_run;
                    phase_step_ = 0;
                    uncached_pairs_ready_ = false;
                    reset_setup();
                    continue;
                }
                if (!setup_ready_) {
                    begin_cached_step_setup(viewer_state);
                    continue;
                }
                ++phase_step_;
                reset_setup();
                return make_navigation_step(VK_RIGHT, true,
                                            kNavBenchScenarioCachedNext);
            }
            case nav_bench_phase::controlled_uncached_24mp_run: {
                if (!uncached_pairs_ready_) {
                    begin_uncached_pairs(viewer_state.list_size);
                }
                if (phase_step_ >= uncached_rearm_indices_.size()) {
                    if (!settle_before_transition()) {
                        return make_idle_step();
                    }
                    emit_memory_sample("post_uncached_24mp");
                    phase_ = nav_bench_phase::rapid_replacement_run;
                    phase_step_ = 0;
                    reset_setup();
                    continue;
                }
                if (!setup_ready_) {
                    const std::size_t rearm_index = uncached_rearm_indices_[phase_step_];
                    begin_uncached_step_setup(rearm_index, rearm_index + 1);
                    continue;
                }
                ++phase_step_;
                reset_setup();
                return make_navigation_step(VK_RIGHT, true,
                                            kNavBenchScenarioUncached24MpNext);
            }
            case nav_bench_phase::rapid_replacement_run: {
                const std::size_t burst = rapid_burst_steps(viewer_state.list_size);
                if (phase_step_ == 0 && !setup_ready_) {
                    begin_rapid_setup();
                    continue;
                }
                if (phase_step_ < burst) {
                    ++phase_step_;
                    // Setup stays satisfied for the whole burst; requests are
                    // issued without quiescing so replacement is exercised.
                    return make_navigation_step(VK_RIGHT, false,
                                                kNavBenchScenarioRapidReplacement);
                }
                if (!settle_before_transition()) {
                    return make_idle_step();
                }
                emit_memory_sample("post_rapid_burst");
                phase_ = nav_bench_phase::forward_back_sweep;
                phase_step_ = 0;
                reset_setup();
                continue;
            }
            case nav_bench_phase::forward_back_sweep: {
                if (phase_step_ == 0 && !setup_ready_) {
                    begin_forward_back_setup(viewer_state);
                    continue;
                }
                if (phase_step_ >= opts_.repetitions) {
                    if (!settle_before_transition()) {
                        return make_idle_step();
                    }
                    emit_memory_sample("post_forward_back");
                    phase_ = nav_bench_phase::cache_memory_sweep;
                    phase_step_ = 0;
                    reset_setup();
                    continue;
                }
                ++phase_step_;
                const int key = (phase_step_ % 2 == 1) ? VK_RIGHT : VK_LEFT;
                return make_navigation_step(key, true,
                                            kNavBenchScenarioForwardBackSweep);
            }
            case nav_bench_phase::cache_memory_sweep: {
                if (phase_step_ == 0 && !setup_ready_) {
                    begin_memory_sweep_setup(viewer_state);
                    continue;
                }
                if (phase_step_ == 0) {
                    ++phase_step_;
                    return make_navigation_step(VK_HOME, true,
                                                kNavBenchScenarioCacheMemorySweep);
                }
                if (phase_step_ == 1) {
                    ++phase_step_;
                    return make_navigation_step(VK_END, true,
                                                kNavBenchScenarioCacheMemorySweep);
                }
                emit_memory_sample("final_sweep");
                phase_ = nav_bench_phase::finished;
                reset_setup();
                return make_finished_step();
            }
            case nav_bench_phase::finished:
            case nav_bench_phase::uninitialized:
            default:
                return make_finished_step();
        }
    }
}

void navigation_benchmark_session::finish(
    const std::string& final_status, const std::string& error_reason) {
    if (!active_) return;

    const std::uint64_t end_ticks = get_qpc_ticks();
    const std::uint64_t dur_ticks = end_ticks - session_start_ticks_;
    const std::int64_t dur_ns = ticks_to_ns(dur_ticks, qpc_frequency_);

    // Emit scenario summaries in deterministic scenario-name order.
    std::vector<std::string> scenario_names;
    scenario_names.reserve(scenarios_.size());
    for (const auto& [name, data] : scenarios_) {
        scenario_names.push_back(name);
    }
    std::sort(scenario_names.begin(), scenario_names.end());

    for (const auto& sc_name : scenario_names) {
        const auto& sc_data = scenarios_[sc_name];
        bench_scenario_summary sum = compute_scenario_summary(
            sc_name,
            opts_.os_cache_condition,
            sc_data.requested,
            sc_data.superseded,
            sc_data.failed,
            sc_data.timed_out,
            sc_data.no_ops,
            sc_data.accepted_latencies_ns);
        write_jsonl_line(serialize_scenario_summary(run_id_, sum));
    }

    // Emit run_end
    write_jsonl_line(serialize_run_end(
        run_id_,
        final_status,
        error_reason,
        dur_ticks,
        dur_ns,
        total_requests_,
        total_presents_,
        total_superseded_,
        total_failed_,
        total_timed_out_,
        total_no_ops_));

    if (out_file_.is_open()) {
        out_file_.flush();
        out_file_.close();
    }
    active_ = false;
}

bool parse_navigation_benchmark_args(
    int argc, wchar_t* const* argv,
    nav_bench_options& out_opts,
    std::string& out_error) {
    out_opts = nav_bench_options{};
    if (argc < 2) {
        return false;
    }
    bool is_nav_bench = false;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--navigation-benchmark") {
            is_nav_bench = true;
            break;
        }
    }
    if (!is_nav_bench) {
        return false;
    }
    out_opts.enabled = true;

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--navigation-benchmark") {
            continue;
        } else if (arg == L"--corpus" && i + 1 < argc) {
            out_opts.corpus_dir = argv[++i];
        } else if (arg == L"--out" && i + 1 < argc) {
            out_opts.output_path = argv[++i];
        } else if (arg == L"--overwrite") {
            out_opts.overwrite = true;
        } else if (arg == L"--os-cache" && i + 1 < argc) {
            std::wstring val = argv[++i];
            if (val == L"cold" || val == L"cold_protocol") {
                out_opts.os_cache_condition = "cold_protocol";
            } else if (val == L"warm" || val == L"warm_protocol") {
                out_opts.os_cache_condition = "warm_protocol";
            } else if (val == L"uncontrolled") {
                out_opts.os_cache_condition = "uncontrolled";
            } else {
                out_error = "os-cache must be uncontrolled|cold_protocol|warm_protocol";
                return false;
            }
        } else if (arg == L"--timeout-ms" && i + 1 < argc) {
            const int t = _wtoi(argv[++i]);
            if (t < static_cast<int>(kNavBenchMinTimeoutMs) ||
                t > static_cast<int>(kNavBenchMaxTimeoutMs)) {
                out_error = "timeout-ms must be in [" + std::to_string(kNavBenchMinTimeoutMs) +
                            ", " + std::to_string(kNavBenchMaxTimeoutMs) + "]";
                return false;
            }
            out_opts.timeout_ms = static_cast<std::uint32_t>(t);
        } else if (arg == L"--repetitions" && i + 1 < argc) {
            const int r = _wtoi(argv[++i]);
            if (r < static_cast<int>(kNavBenchMinRepetitions) ||
                r > static_cast<int>(kNavBenchMaxRepetitions)) {
                out_error = "repetitions must be in [" + std::to_string(kNavBenchMinRepetitions) +
                            ", " + std::to_string(kNavBenchMaxRepetitions) + "]";
                return false;
            }
            out_opts.repetitions = static_cast<std::uint32_t>(r);
        } else if (arg == L"--rapid-burst" && i + 1 < argc) {
            const int b = _wtoi(argv[++i]);
            if (b < static_cast<int>(kNavBenchMinRapidBurst) ||
                b > static_cast<int>(kNavBenchMaxRapidBurst)) {
                out_error = "rapid-burst must be in [" + std::to_string(kNavBenchMinRapidBurst) +
                            ", " + std::to_string(kNavBenchMaxRapidBurst) + "]";
                return false;
            }
            out_opts.rapid_burst = static_cast<std::uint32_t>(b);
        } else {
            out_error = "unknown or malformed argument: " + oz::runner::wide_to_utf8(arg);
            return false;
        }
    }

    if (out_opts.corpus_dir.empty()) {
        out_error = "--corpus <dir> is required for --navigation-benchmark";
        return false;
    }
    if (out_opts.output_path.empty()) {
        out_error = "--out <file.jsonl> is required for --navigation-benchmark";
        return false;
    }

    // Shared bounded-option validation: parser and core agree on the limits.
    const std::string validation_error = validate_nav_bench_options(out_opts);
    if (!validation_error.empty()) {
        out_error = validation_error;
        return false;
    }
    return true;
}

}  // namespace oz::viewer_core

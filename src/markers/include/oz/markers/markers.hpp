// oz_benchmark_markers - dependency-free timing marker library.
//
// Stage 0 contract: exactly these twelve marker names, in canonical order:
//   ProcessStart, RendererReady, FileOpened, CodecDetected, HeaderParsed,
//   DecodeStart, DecodeComplete, GpuUploadStart, GpuUploadComplete,
//   FirstPresent, FolderEnumerationStart, FolderEnumerationComplete
//
// Canonical order follows the spec's startup flow; the folder-enumeration
// markers canonically follow FirstPresent because enumeration is asynchronous.
//
// This header is intentionally free of Windows.h, JSON, and CLI concerns so
// future application code can link the library without pulling in the runner.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oz::markers {

// The twelve approved Stage 0 marker names, in canonical order.
enum class marker_id : std::uint8_t {
    process_start = 0,
    renderer_ready,
    file_opened,
    codec_detected,
    header_parsed,
    decode_start,
    decode_complete,
    gpu_upload_start,
    gpu_upload_complete,
    first_present,
    folder_enumeration_start,
    folder_enumeration_complete,
};

constexpr std::size_t marker_count = 12;

// Exact canonical name for a marker ("ProcessStart", ...). Empty for invalid ids.
std::string_view marker_name(marker_id id) noexcept;

// Inverse lookup; nullopt for unknown names.
std::optional<marker_id> marker_from_name(std::string_view name) noexcept;

// QueryPerformanceCounter frequency (ticks per second). 0 if unavailable.
std::uint64_t qpc_frequency() noexcept;

// Current QueryPerformanceCounter value.
std::uint64_t qpc_now() noexcept;

// Convert a tick delta to integer nanoseconds without overflow for any
// realistic session (the seconds*1e9 term overflows only after ~584 years
// of ticks at 10 MHz).
std::int64_t ticks_to_ns(std::uint64_t ticks, std::uint64_t frequency) noexcept;

// One recorded marker sample.
struct marker_record {
    marker_id id{};
    std::uint64_t raw_ticks{};   // QueryPerformanceCounter value at record time
    std::int64_t relative_ns{};  // integer ns since session start (derived)
    std::uint32_t thread_id{};   // OS thread id that recorded the marker
};

enum class validation_kind : std::uint8_t { duplicate, missing, out_of_order };

struct validation_failure {
    validation_kind kind{};
    marker_id id{};
    std::string detail;
};

// Validates an arbitrary sequence of records (e.g. parsed from a subject's
// output stream) using the same rules as marker_session::validate().
std::vector<validation_failure> validate_records(const std::vector<marker_record>& records) noexcept;

// Records markers against a session start. Constructing the session records
// ProcessStart immediately. record() is thread-safe; records() must only be
// read after recording has finished (or from a single thread).
class marker_session {
public:
    marker_session();

    void record(marker_id id);

    const std::vector<marker_record>& records() const noexcept { return records_; }
    std::uint64_t start_ticks() const noexcept { return start_ticks_; }
    std::uint64_t frequency() const noexcept { return frequency_; }

    // duplicate / missing / out-of-order failures (see docs/benchmark.md).
    std::vector<validation_failure> validate() const;

private:
    std::uint64_t start_ticks_;
    std::uint64_t frequency_;
    std::vector<marker_record> records_;
    mutable std::mutex mutex_;
};

}  // namespace oz::markers
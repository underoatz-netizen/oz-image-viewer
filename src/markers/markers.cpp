#include "oz/markers/markers.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <array>

namespace oz::markers {
namespace {

constexpr std::string_view kMarkerNames[marker_count] = {
    "ProcessStart", "RendererReady", "FileOpened", "CodecDetected",
    "HeaderParsed", "DecodeStart", "DecodeComplete", "GpuUploadStart",
    "GpuUploadComplete", "FirstPresent", "FolderEnumerationStart",
    "FolderEnumerationComplete",
};

std::uint64_t qpc_now_impl() noexcept {
    LARGE_INTEGER v{};
    QueryPerformanceCounter(&v);
    return static_cast<std::uint64_t>(v.QuadPart);
}

}  // namespace

std::uint64_t qpc_frequency() noexcept {
    LARGE_INTEGER v{};
    QueryPerformanceFrequency(&v);
    return static_cast<std::uint64_t>(v.QuadPart);
}

std::uint64_t qpc_now() noexcept {
    return qpc_now_impl();
}

std::string_view marker_name(marker_id id) noexcept {
    const auto i = static_cast<std::size_t>(id);
    return i < marker_count ? kMarkerNames[i] : std::string_view{};
}

std::optional<marker_id> marker_from_name(std::string_view name) noexcept {
    for (std::size_t i = 0; i < marker_count; ++i) {
        if (kMarkerNames[i] == name) {
            return static_cast<marker_id>(i);
        }
    }
    return std::nullopt;
}

std::int64_t ticks_to_ns(std::uint64_t ticks, std::uint64_t frequency) noexcept {
    if (frequency == 0) {
        return 0;
    }
    const std::uint64_t seconds = ticks / frequency;
    const std::uint64_t remainder = ticks % frequency;
    return static_cast<std::int64_t>(seconds * 1'000'000'000ULL +
                                     remainder * 1'000'000'000ULL / frequency);
}

std::vector<validation_failure> validate_records(const std::vector<marker_record>& records) noexcept {
    std::vector<validation_failure> failures;
    std::array<std::size_t, marker_count> counts{};
    std::array<bool, marker_count> seen{};
    std::size_t max_index = 0;
    bool have_any = false;

for (const auto& r : records) {
        const auto i = static_cast<std::size_t>(r.id);
        if (i >= marker_count) {
            continue;
        }
        if (counts[i] > 0) {
            // A duplicated marker is reported once as duplicate; it is not
            // additionally reported as out_of_order (non-duplicative).
            failures.push_back({validation_kind::duplicate, r.id,
                                std::string(marker_name(r.id)) + " recorded more than once"});
            continue;
        }
        ++counts[i];
        if (have_any && i < max_index) {
            failures.push_back({validation_kind::out_of_order, r.id,
                                std::string(marker_name(r.id)) +
                                    " recorded after a later-canonical marker"});
        }
        max_index = std::max(max_index, i);
        seen[i] = true;
        have_any = true;
    }

    for (std::size_t i = 0; i < max_index; ++i) {
        if (!seen[i]) {
            failures.push_back({validation_kind::missing, static_cast<marker_id>(i),
                                std::string(marker_name(static_cast<marker_id>(i))) +
                                    " canonically precedes the last recorded marker "
                                    "but was never recorded"});
        }
    }
    return failures;
}

marker_session::marker_session() : start_ticks_(qpc_now_impl()), frequency_(qpc_frequency()) {
    record(marker_id::process_start);
}

void marker_session::record(marker_id id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t now = qpc_now_impl();
    marker_record r;
    r.id = id;
    r.raw_ticks = now;
    r.relative_ns = ticks_to_ns(now - start_ticks_, frequency_);
    r.thread_id = static_cast<std::uint32_t>(::GetCurrentThreadId());
    records_.push_back(r);
}

std::vector<validation_failure> marker_session::validate() const {
    std::vector<marker_record> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = records_;
    }
    return validate_records(snapshot);
}

}  // namespace oz::markers
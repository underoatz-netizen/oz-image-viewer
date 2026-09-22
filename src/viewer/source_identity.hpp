// oz/viewer/source_identity.hpp - shared stable file-identity helper (Editing ON/OFF).
//
// Ownership note: these helpers were previously defined in edit_controller.cpp,
// which is excluded when OZVIEWER_ENABLE_EDITING=OFF, while copy_controller.cpp
// (always built) calls read_file_stable_identity. They now live in the
// always-built source_identity.cpp so the Editing-OFF viewer links. Behavior is
// unchanged; edit_controller.hpp re-exports these declarations so existing
// Editing-ON includes keep working.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace oz::viewer {

// Source verification helper: checks file identity, size, and last write time before/after read
bool verify_source_file_metadata(const std::filesystem::path& path,
                                 std::uint64_t expected_size,
                                 std::uint64_t expected_last_write,
                                 std::uint64_t* out_current_size = nullptr,
                                 std::uint64_t* out_current_last_write = nullptr) noexcept;

// Read file contents while enforcing stable identity around the read boundary.
// Captures size + last write time before and after the read, and fails if they do
// not match each other (or do not match expected_size/expected_last_write when non-zero).
bool read_file_stable_identity(const std::filesystem::path& path,
                               std::vector<std::uint8_t>& out_data,
                               std::uint64_t expected_size,
                               std::uint64_t expected_last_write,
                               std::uint64_t& out_stable_size,
                               std::uint64_t& out_stable_last_write,
                               std::string& out_error_detail,
                               const std::function<bool()>& check_cancel);

// Test seam for the stable-read boundary (post-first-snapshot/pre-second-
// snapshot inside read_file_stable_identity). Thread-safe, scoped, resettable.
// Production code never installs a hook; the viewer UI has no access to it.
struct scoped_stable_read_hook {
    explicit scoped_stable_read_hook(std::function<void()> hook);
    ~scoped_stable_read_hook();
    scoped_stable_read_hook(const scoped_stable_read_hook&) = delete;
    scoped_stable_read_hook& operator=(const scoped_stable_read_hook&) = delete;

private:
    std::function<void()> previous_;
};

}  // namespace oz::viewer

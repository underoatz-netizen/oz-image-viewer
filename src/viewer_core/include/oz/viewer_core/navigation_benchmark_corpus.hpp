#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace oz::viewer_core {

struct corpus_manifest_entry {
    std::string alias;
    std::size_t ordinal = 0;
    std::string sha256;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::uint64_t file_bytes = 0;
    std::string rel_filename;
};

struct corpus_validation_result {
    bool ok = false;
    std::string error;
    std::size_t count = 0;
    std::string manifest_sha256;
    std::vector<corpus_manifest_entry> entries;
};

// Validates that directory contains exactly `expected_count` JPEG files
// matching standard specs (default 500 for the real navigation benchmark),
// no foreign files (only .jpg/.jpeg and manifest.json/manifest.json.sha256),
// valid magic, positive dimensions, no EXIF/XMP/comment metadata, and that a
// present manifest.json (including its .sha256 seal) matches the actual files.
corpus_validation_result validate_navigation_corpus(
    const std::filesystem::path& dir, std::size_t expected_count = 500);

// Deterministic generator: generates exactly `count` metadata-free synthetic
// JPEGs with stable aliases and documented mixture (including 24MP images) into
// out_dir. Writes manifest.json and its .sha256 seal. Returns error string or
// empty on success. The default count is the navigation benchmark corpus size;
// smaller counts exist only for isolated smoke checks and are never final
// performance data.
// progress_cb is optional callback: (completed, total).
std::string generate_deterministic_corpus(
    const std::filesystem::path& out_dir,
    const std::function<void(std::size_t, std::size_t)>& progress_cb = nullptr,
    std::size_t count = 500);

}  // namespace oz::viewer_core

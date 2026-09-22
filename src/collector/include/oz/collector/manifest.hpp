// oz/collector/manifest.hpp - sealed private manifest (schema oz.manifest.v1).
//
// The private manifest maps public aliases to private source-image paths plus
// identity metadata (hash, dimensions, bytes). It is sealed: the sibling file
// "<manifest>.sha256" must contain the lowercase hex SHA-256 of the manifest
// file bytes. Any mismatch is treated as tampering and the manifest is
// rejected. Private paths are used only inside the collector; public results
// carry aliases only.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace oz::collector {

struct manifest_image {
    std::string alias;       // public alias used in results (never the path)
    std::string path;        // private source path; never printed in public output
    std::string sha256;      // lowercase hex SHA-256 of the image file
    std::int64_t width = 0;
    std::int64_t height = 0;
    std::uint64_t size_bytes = 0;
    std::uint64_t pixels = 0;
    double target_mp = 0;
    double deviation_mp = 0;
};

struct manifest {
    std::string schema;  // "oz.manifest.v1"
    std::string id;      // manifest identifier carried into result records
    std::vector<manifest_image> images;
};

// Loads the manifest at `path` and validates its seal (sibling "<path>.sha256`).
// Returns nullopt with a human-readable error on any failure: missing file,
// malformed JSON, missing seal, or seal mismatch (tamper).
std::optional<manifest> load_manifest(const std::wstring& path, std::string& error);

// Lowercase hex SHA-256 of a byte range (used for sealing and hashes).
std::string sha256_hex(const void* data, std::size_t size);

// Lowercase hex SHA-256 of a file's bytes; empty when the file is unreadable.
std::string sha256_file_hex(const std::wstring& path);

}  // namespace oz::collector
// heif_loader.hpp - hardened lazy DLL loader for libheif/libde265 (Stage 3.4).
// Process-lifetime LoadLibraryExW with LOAD_LIBRARY_SEARCH_APPLICATION_DIRECTORY
// only (never PATH, registry, plugin, or working-dir scan).
// Resolves required libheif C-API function pointers and checks version compatibility.
#pragma once

#include <cstddef>
#include <cstdint>

#if OZVIEWER_HAVE_HEIC
#include <libheif/heif.h>
#else
// Minimal forward declarations / typedefs for stub builds
struct heif_context;
struct heif_image_handle;
struct heif_image;
struct heif_decoding_options;
struct heif_color_profile_nclx;
struct heif_decoder_descriptor;
struct heif_error {
    int code;
    int subcode;
    const char* message;
};
enum heif_colorspace { heif_colorspace_RGB = 1 };
enum heif_chroma { heif_chroma_interleaved_RGBA = 10 };
enum heif_channel { heif_channel_interleaved = 10 };
enum heif_compression_format { heif_compression_HEVC = 1 };
#endif

namespace oz::viewer_core::heif_loader {

struct libheif_api {
    const char* (*get_version)(void) = nullptr;
    uint32_t (*get_version_number)(void) = nullptr;
    struct heif_context* (*context_alloc)(void) = nullptr;
    void (*context_free)(struct heif_context*) = nullptr;
    struct heif_error (*context_read_from_memory_without_copy)(struct heif_context*, const void*, size_t, const struct heif_reading_options*) = nullptr;
    struct heif_error (*context_get_primary_image_handle)(struct heif_context*, struct heif_image_handle**) = nullptr;
    void (*context_set_max_decoding_threads)(struct heif_context*, int) = nullptr;
    int (*context_get_number_of_top_level_images)(struct heif_context*) = nullptr;
    int (*context_has_sequence)(struct heif_context*) = nullptr;

    void (*image_handle_release)(const struct heif_image_handle*) = nullptr;
    int (*image_handle_get_width)(const struct heif_image_handle*) = nullptr;
    int (*image_handle_get_height)(const struct heif_image_handle*) = nullptr;
    int (*image_handle_get_ispe_width)(const struct heif_image_handle*) = nullptr;
    int (*image_handle_get_ispe_height)(const struct heif_image_handle*) = nullptr;
    int (*image_handle_has_alpha_channel)(const struct heif_image_handle*) = nullptr;
    int (*image_handle_is_premultiplied_alpha)(const struct heif_image_handle*) = nullptr;
    int (*image_handle_get_luma_bits_per_pixel)(const struct heif_image_handle*) = nullptr;
    int (*image_handle_get_chroma_bits_per_pixel)(const struct heif_image_handle*) = nullptr;
    int (*image_handle_has_depth_image)(const struct heif_image_handle*) = nullptr;
    int (*image_handle_get_number_of_auxiliary_images)(const struct heif_image_handle*, int) = nullptr;
    int (*image_handle_get_number_of_thumbnails)(const struct heif_image_handle*) = nullptr;

    // Metadata & Color
    size_t (*image_handle_get_raw_color_profile_size)(const struct heif_image_handle*) = nullptr;
    struct heif_error (*image_handle_get_raw_color_profile)(const struct heif_image_handle*, void*) = nullptr;
    struct heif_error (*image_handle_get_nclx_color_profile)(const struct heif_image_handle*, struct heif_color_profile_nclx**) = nullptr;
    void (*nclx_color_profile_free)(struct heif_color_profile_nclx*) = nullptr;
    int (*image_handle_get_number_of_metadata_blocks)(const struct heif_image_handle*, const char*) = nullptr;
    int (*image_handle_get_list_of_metadata_block_IDs)(const struct heif_image_handle*, const char*, uint32_t*, int) = nullptr;
    size_t (*image_handle_get_metadata_size)(const struct heif_image_handle*, uint32_t) = nullptr;
    struct heif_error (*image_handle_get_metadata)(const struct heif_image_handle*, uint32_t, void*) = nullptr;

    // Decode
    struct heif_decoding_options* (*decoding_options_alloc)(void) = nullptr;
    void (*decoding_options_free)(struct heif_decoding_options*) = nullptr;
    struct heif_error (*decode_image)(const struct heif_image_handle*, struct heif_image**, enum heif_colorspace, enum heif_chroma, const struct heif_decoding_options*) = nullptr;

    // Image data access
    void (*image_release)(const struct heif_image*) = nullptr;
    int (*image_get_width)(const struct heif_image*, enum heif_channel) = nullptr;
    int (*image_get_height)(const struct heif_image*, enum heif_channel) = nullptr;
    const uint8_t* (*image_get_plane_readonly)(const struct heif_image*, enum heif_channel, int*) = nullptr;

    // Decoders enumeration
    int (*get_decoder_descriptors)(enum heif_compression_format, const struct heif_decoder_descriptor**, int) = nullptr;
    const char* (*decoder_descriptor_get_id_name)(const struct heif_decoder_descriptor*) = nullptr;
};

// Returns true if libheif and its dependencies (libde265) were loaded successfully
// from application directory and all required entry points and versions are validated.
bool is_heif_available() noexcept;

// Non-probing query: returns true if libheif has ALREADY been initialized/loaded
// in this process, without triggering any LoadLibraryExW attempt.
bool is_heif_loaded() noexcept;

// Observability probes for conformance audit:
// Returns cumulative number of times the HEIC dynamic loader attempted initialization/loading
// (e.g. LoadLibraryExW attempts, whether successful or failed).
std::uint64_t load_attempt_count() noexcept;

// Resets cumulative loader initialization attempt count.
void reset_load_attempt_count() noexcept;

// Returns pointer to table of resolved libheif functions, or nullptr if unavailable.
const libheif_api* get_api() noexcept;

}  // namespace oz::viewer_core::heif_loader

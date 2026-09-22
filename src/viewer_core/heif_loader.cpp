// heif_loader.cpp - hardened lazy DLL loader for libheif/libde265 (Stage 3.4).
#include "heif_loader.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <windows.h>

#ifndef LOAD_LIBRARY_SEARCH_APPLICATION_DIRECTORY
#define LOAD_LIBRARY_SEARCH_APPLICATION_DIRECTORY 0x00000200
#endif

#include <mutex>
#include <atomic>
#include <cstring>

namespace oz::viewer_core::heif_loader {
namespace {

std::atomic<std::uint64_t> g_load_attempts{0};

#if OZVIEWER_HAVE_HEIC

HMODULE g_libheif_dll = nullptr;
libheif_api g_api{};
bool g_initialized = false;
bool g_available = false;
std::mutex g_init_mutex;

void init_loader_unlocked() noexcept {
    g_load_attempts.fetch_add(1, std::memory_order_relaxed);
    if (g_initialized) {
        return;
    }
    g_initialized = true;
    g_available = false;

    // Hardened DLL loading:
    // ONLY LOAD_LIBRARY_SEARCH_APPLICATION_DIRECTORY. Never PATH, current dir, or registry.
    // Notice that heif.dll depends on libde265.dll, so loading heif.dll with
    // LOAD_LIBRARY_SEARCH_APPLICATION_DIRECTORY will resolve libde265.dll from the application directory too.
    HMODULE h = LoadLibraryExW(L"heif.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIRECTORY);
    if (h == nullptr) {
        return;
    }

    auto resolve = [h](const char* sym) -> FARPROC {
        return GetProcAddress(h, sym);
    };

    #define BIND(field, name) \
        g_api.field = reinterpret_cast<decltype(g_api.field)>(resolve(name)); \
        if (g_api.field == nullptr) { FreeLibrary(h); return; }

    BIND(get_version, "heif_get_version");
    BIND(get_version_number, "heif_get_version_number");
    BIND(context_alloc, "heif_context_alloc");
    BIND(context_free, "heif_context_free");
    BIND(context_read_from_memory_without_copy, "heif_context_read_from_memory_without_copy");
    BIND(context_get_primary_image_handle, "heif_context_get_primary_image_handle");
    BIND(context_set_max_decoding_threads, "heif_context_set_max_decoding_threads");
    BIND(context_get_number_of_top_level_images, "heif_context_get_number_of_top_level_images");
    BIND(context_has_sequence, "heif_context_has_sequence");

    BIND(image_handle_release, "heif_image_handle_release");
    BIND(image_handle_get_width, "heif_image_handle_get_width");
    BIND(image_handle_get_height, "heif_image_handle_get_height");
    BIND(image_handle_get_ispe_width, "heif_image_handle_get_ispe_width");
    BIND(image_handle_get_ispe_height, "heif_image_handle_get_ispe_height");
    BIND(image_handle_has_alpha_channel, "heif_image_handle_has_alpha_channel");
    BIND(image_handle_is_premultiplied_alpha, "heif_image_handle_is_premultiplied_alpha");
    BIND(image_handle_get_luma_bits_per_pixel, "heif_image_handle_get_luma_bits_per_pixel");
    BIND(image_handle_get_chroma_bits_per_pixel, "heif_image_handle_get_chroma_bits_per_pixel");
    BIND(image_handle_has_depth_image, "heif_image_handle_has_depth_image");
    BIND(image_handle_get_number_of_auxiliary_images, "heif_image_handle_get_number_of_auxiliary_images");
    BIND(image_handle_get_number_of_thumbnails, "heif_image_handle_get_number_of_thumbnails");

    BIND(image_handle_get_raw_color_profile_size, "heif_image_handle_get_raw_color_profile_size");
    BIND(image_handle_get_raw_color_profile, "heif_image_handle_get_raw_color_profile");
    BIND(image_handle_get_nclx_color_profile, "heif_image_handle_get_nclx_color_profile");
    BIND(nclx_color_profile_free, "heif_nclx_color_profile_free");
    BIND(image_handle_get_number_of_metadata_blocks, "heif_image_handle_get_number_of_metadata_blocks");
    BIND(image_handle_get_list_of_metadata_block_IDs, "heif_image_handle_get_list_of_metadata_block_IDs");
    BIND(image_handle_get_metadata_size, "heif_image_handle_get_metadata_size");
    BIND(image_handle_get_metadata, "heif_image_handle_get_metadata");

    BIND(decoding_options_alloc, "heif_decoding_options_alloc");
    BIND(decoding_options_free, "heif_decoding_options_free");
    BIND(decode_image, "heif_decode_image");

    BIND(image_release, "heif_image_release");
    BIND(image_get_width, "heif_image_get_width");
    BIND(image_get_height, "heif_image_get_height");
    BIND(image_get_plane_readonly, "heif_image_get_plane_readonly");

    BIND(get_decoder_descriptors, "heif_get_decoder_descriptors");
    BIND(decoder_descriptor_get_id_name, "heif_decoder_descriptor_get_id_name");

    #undef BIND

    // Validate version: require >= 1.14 (vcpkg provides 1.23.2)
    const uint32_t ver = g_api.get_version_number();
    if (ver < 0x010E0000) {
        FreeLibrary(h);
        g_api = {};
        return;
    }

    // Verify decoder descriptor for HEVC: must have libde265
    const heif_decoder_descriptor* descriptors[8]{};
    int count = g_api.get_decoder_descriptors(heif_compression_HEVC, descriptors, 8);
    bool found_de265 = false;
    for (int i = 0; i < count; ++i) {
        if (descriptors[i] != nullptr) {
            const char* id = g_api.decoder_descriptor_get_id_name(descriptors[i]);
            if (id != nullptr && std::strstr(id, "libde265") != nullptr) {
                found_de265 = true;
                break;
            }
        }
    }

    if (!found_de265) {
        FreeLibrary(h);
        g_api = {};
        return;
    }

    g_libheif_dll = h;
    g_available = true;
}

#endif // OZVIEWER_HAVE_HEIC

}  // namespace

bool is_heif_available() noexcept {
#if OZVIEWER_HAVE_HEIC
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (!g_initialized) {
        init_loader_unlocked();
    }
    return g_available;
#else
    return false;
#endif
}

bool is_heif_loaded() noexcept {
#if OZVIEWER_HAVE_HEIC
    std::lock_guard<std::mutex> lock(g_init_mutex);
    return g_initialized && g_available;
#else
    return false;
#endif
}

const libheif_api* get_api() noexcept {
#if OZVIEWER_HAVE_HEIC
    if (is_heif_available()) {
        return &g_api;
    }
    return nullptr;
#else
    return nullptr;
#endif
}

std::uint64_t load_attempt_count() noexcept {
    return g_load_attempts.load(std::memory_order_relaxed);
}

void reset_load_attempt_count() noexcept {
    g_load_attempts.store(0, std::memory_order_relaxed);
}

}  // namespace oz::viewer_core::heif_loader

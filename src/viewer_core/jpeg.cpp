#include "oz/viewer_core/jpeg.hpp"

#include <cstring>

#if OZVIEWER_HAVE_TURBOJPEG
#include <turbojpeg.h>
#endif

namespace oz::viewer_core {

bool has_jpeg_magic(const std::uint8_t* data, std::size_t size) noexcept {
    return data != nullptr && size >= 2 && data[0] == 0xFF && data[1] == 0xD8;
}

// decoded_size_within_limits lives in the codec registry (codec.cpp);
// the JPEG adapter reuses the shared bound unchanged.

#if OZVIEWER_HAVE_TURBOJPEG

jpeg_error_kind parse_jpeg_header(const std::uint8_t* data, std::size_t size,
                                  jpeg_header& out) noexcept {
    if (!has_jpeg_magic(data, size)) {
        return jpeg_error_kind::not_jpeg;
    }
    if (size > kMaxCompressedBytes) {
        return jpeg_error_kind::allocation_limit;
    }
    tjhandle handle = tj3Init(TJINIT_DECOMPRESS);
    if (handle == nullptr) {
        return jpeg_error_kind::invalid_header;
    }
    const int rc = tj3DecompressHeader(handle, data, size);
    const int w = (rc == 0) ? tj3Get(handle, TJPARAM_JPEGWIDTH) : 0;
    const int h = (rc == 0) ? tj3Get(handle, TJPARAM_JPEGHEIGHT) : 0;
    tj3Destroy(handle);
    if (rc != 0) {
        return jpeg_error_kind::invalid_header;
    }
    // TurboJPEG 3.x can return success with non-positive dimensions when the
    // stream has no start-of-frame marker (e.g. a bare SOI or truncated header);
    // a valid JPEG always reports positive dimensions.
    if (w <= 0 || h <= 0) {
        return jpeg_error_kind::invalid_header;
    }
    if (!decoded_size_within_limits(w, h)) {
        return jpeg_error_kind::dimension_limit;
    }
    out.width = w;
    out.height = h;
    return jpeg_error_kind::none;
}

jpeg_result decode_jpeg(const std::uint8_t* data, std::size_t size,
                        int scaling_denominator) noexcept {
    jpeg_result result;
    if (!has_jpeg_magic(data, size)) {
        result.error = jpeg_error_kind::not_jpeg;
        return result;
    }
    if (size > kMaxCompressedBytes) {
        result.error = jpeg_error_kind::allocation_limit;
        return result;
    }
    if (scaling_denominator != 1 && scaling_denominator != 2 &&
        scaling_denominator != 4 && scaling_denominator != 8) {
        scaling_denominator = 1;
    }

    tjhandle handle = tj3Init(TJINIT_DECOMPRESS);
    if (handle == nullptr) {
        result.error = jpeg_error_kind::invalid_header;
        return result;
    }

    int w = 0;
    int h = 0;
    if (tj3DecompressHeader(handle, data, size) != 0) {
        result.error = jpeg_error_kind::invalid_header;
        result.detail = tj3GetErrorStr(handle);
        tj3Destroy(handle);
        return result;
    }
    w = tj3Get(handle, TJPARAM_JPEGWIDTH);
    h = tj3Get(handle, TJPARAM_JPEGHEIGHT);
    if (!decoded_size_within_limits(w, h)) {
        result.error = jpeg_error_kind::dimension_limit;
        tj3Destroy(handle);
        return result;
    }

    // Apply the scaling factor (TurboJPEG scales to ceil(dim * num / denom)).
    tjscalingfactor sf{1, scaling_denominator};
    tj3SetScalingFactor(handle, sf);
    const int sw = TJSCALED(w, sf);
    const int sh = TJSCALED(h, sf);
    if (!decoded_size_within_limits(sw, sh)) {
        result.error = jpeg_error_kind::allocation_limit;
        tj3Destroy(handle);
        return result;
    }

    const std::size_t stride = static_cast<std::size_t>(sw) * 4u;
    const std::size_t bytes = stride * static_cast<std::size_t>(sh);
    if (bytes > kMaxDecodedBytes) {
        result.error = jpeg_error_kind::allocation_limit;
        tj3Destroy(handle);
        return result;
    }

    result.pixels.resize(bytes);
    if (tj3Decompress8(handle, data, size, result.pixels.data(), static_cast<int>(stride),
                       TJPF_BGRA) != 0) {
        result.error = jpeg_error_kind::decode_failed;
        result.detail = tj3GetErrorStr(handle);
        tj3Destroy(handle);
        result.pixels.clear();
        return result;
    }
    tj3Destroy(handle);

    // Force alpha to 255 (BGRA layout: B G R A).
    for (std::size_t i = 3; i < bytes; i += 4) {
        result.pixels[i] = 0xFF;
    }

    result.header.width = sw;
    result.header.height = sh;
    result.stride = static_cast<std::int32_t>(stride);
    return result;
}

#else  // !OZVIEWER_HAVE_TURBOJPEG -- dependency-free fallback (no decoder).

// The dependency-free core still validates magic, size and dimension bounds;
// actual header parsing / pixel decode require libjpeg-turbo (vcpkg). These
// stubs let ozviewer_core build and be tested without the codec present.
jpeg_error_kind parse_jpeg_header(const std::uint8_t* data, std::size_t size,
                                  jpeg_header& out) noexcept {
    (void)out;
    if (!has_jpeg_magic(data, size)) {
        return jpeg_error_kind::not_jpeg;
    }
    if (size > kMaxCompressedBytes) {
        return jpeg_error_kind::allocation_limit;
    }
    return jpeg_error_kind::decode_failed;  // no decoder linked
}

jpeg_result decode_jpeg(const std::uint8_t* data, std::size_t size,
                        int /*scaling_denominator*/) noexcept {
    jpeg_result result;
    if (!has_jpeg_magic(data, size)) {
        result.error = jpeg_error_kind::not_jpeg;
        return result;
    }
    if (size > kMaxCompressedBytes) {
        result.error = jpeg_error_kind::allocation_limit;
        return result;
    }
    result.error = jpeg_error_kind::decode_failed;
    result.detail = "libjpeg-turbo not available (build with the pinned vcpkg toolchain)";
    return result;
}

#endif  // OZVIEWER_HAVE_TURBOJPEG

}  // namespace oz::viewer_core
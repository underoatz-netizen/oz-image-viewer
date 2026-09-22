// oz/viewer_core/save_pipeline.cpp - bounded non-destructive Save/Save As
// export pipeline (Stage 5 Slice 6, Stage 6).
// In-box WIC encoders for PNG, JPEG, TIFF, BMP; static WebP::webp for lossless WebP.
// See save_pipeline.hpp for the contract.
#include "oz/viewer_core/save_pipeline.hpp"

#include <algorithm>
#include <climits>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <new>
#include <string>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <wincodec.h>
#include <wrl/client.h>

#if defined(OZVIEWER_HAVE_WEBP_ENCODER) && OZVIEWER_HAVE_WEBP_ENCODER
#include <webp/encode.h>
#endif

using Microsoft::WRL::ComPtr;

namespace oz::viewer_core {

namespace {

static std::atomic<bool> s_inject_reopen_failure{false};
static std::atomic<bool> s_inject_flush_failure{false};

save_result make_result(save_error error, std::string detail = {}) noexcept {
    save_result result;
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

// Ensure COM is initialized on the current thread (STA or MTA); balanced on
// the same thread. Mirrors the edit pipeline's lazy COM scope.
struct com_init_scope {
    HRESULT hr = E_FAIL;
    com_init_scope() noexcept {
        hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (hr == RPC_E_CHANGED_MODE) {
            // Already initialized as STA on this thread; keep it valid.
            hr = S_FALSE;
        }
    }
    ~com_init_scope() noexcept {
        if (SUCCEEDED(hr) && hr != S_FALSE) {
            CoUninitialize();
        }
    }
    [[nodiscard]] bool ok() const noexcept {
        return SUCCEEDED(hr);
    }
};

bool dims_within_limits(std::int32_t width, std::int32_t height) noexcept {
    return width > 0 && height > 0 &&
           width <= kMaxEditDimension && height <= kMaxEditDimension;
}

// 24bpp DIB/encoder rows are kept 4-byte aligned; the padding is zeroed.
std::size_t align4(std::size_t bytes) noexcept {
    return (bytes + 3u) & ~static_cast<std::size_t>(3u);
}

bool all_pixels_opaque(const std::uint8_t* pixels, std::int32_t width,
                       std::int32_t height, std::int32_t stride) noexcept {
    for (std::int32_t y = 0; y < height; ++y) {
        const std::uint8_t* row = pixels + static_cast<std::size_t>(y) * stride;
        for (std::int32_t x = 0; x < width; ++x) {
            if (row[static_cast<std::size_t>(x) * 4 + 3] != 255) {
                return false;
            }
        }
    }
    return true;
}

// Canonical premultiplied BGRA8 row -> straight (unassociated) BGRA8 row.
void unpremultiply_row(const std::uint8_t* src, std::uint8_t* dst,
                       std::int32_t width) noexcept {
    for (std::int32_t x = 0; x < width; ++x) {
        const std::uint32_t a = src[3];
        if (a == 0) {
            dst[0] = 0;
            dst[1] = 0;
            dst[2] = 0;
            dst[3] = 0;
        } else {
            const std::uint32_t half = a / 2u;
            dst[0] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(src[0]) * 255u + half) / a);
            dst[1] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(src[1]) * 255u + half) / a);
            dst[2] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(src[2]) * 255u + half) / a);
            dst[3] = static_cast<std::uint8_t>(a);
        }
        src += 4;
        dst += 4;
    }
}

// Convert canonical PBGRA8 to the encoder input layout for `format`.
// On success `out`/`out_stride` describe the converted buffer and `bytes_out`
// its exact byte count.
save_result convert_for_format(save_format format, const std::uint8_t* pixels,
                               std::int32_t width, std::int32_t height,
                               std::int32_t stride, std::vector<std::uint8_t>& out,
                               std::int32_t& out_stride, std::size_t& bytes_out) noexcept {
    if (!pixels || !dims_within_limits(width, height) || stride < width * 4) {
        return make_result(save_error::invalid_request, "invalid pixel buffer/dimensions for conversion");
    }

    if ((format == save_format::jpeg || format == save_format::bmp) &&
        !all_pixels_opaque(pixels, width, height, stride)) {
        return make_result(save_error::alpha_not_supported,
                           (format == save_format::jpeg)
                               ? "JPEG export requires fully opaque pixels; use PNG, WebP, or TIFF to keep alpha"
                               : "BMP export requires fully opaque pixels; use PNG, WebP, or TIFF to keep alpha");
    }

    const bool is_32bpp = (format == save_format::png || format == save_format::webp || format == save_format::tiff);
    const std::size_t row_bytes = is_32bpp
                                      ? static_cast<std::size_t>(width) * 4u
                                      : align4(static_cast<std::size_t>(width) * 3u);
    const std::size_t total_bytes = row_bytes * static_cast<std::size_t>(height);
    if (total_bytes > kMaxDecodedBytes) {
        return make_result(save_error::materialization_failed,
                           "converted export bytes exceed the decoded byte limit");
    }

    try {
        out.resize(total_bytes);
    } catch (const std::bad_alloc&) {
        return make_result(save_error::materialization_failed,
                           "allocation failed for the converted export buffer");
    }

    if (is_32bpp) {
        for (std::int32_t y = 0; y < height; ++y) {
            const std::uint8_t* src_row = pixels + static_cast<std::size_t>(y) * stride;
            std::uint8_t* dst_row = out.data() + static_cast<std::size_t>(y) * row_bytes;
            unpremultiply_row(src_row, dst_row, width);
        }
    } else {
        const std::size_t meaningful_row = static_cast<std::size_t>(width) * 3u;
        for (std::int32_t y = 0; y < height; ++y) {
            const std::uint8_t* src_row = pixels + static_cast<std::size_t>(y) * stride;
            std::uint8_t* dst_row = out.data() + static_cast<std::size_t>(y) * row_bytes;
            for (std::int32_t x = 0; x < width; ++x) {
                dst_row[0] = src_row[0];
                dst_row[1] = src_row[1];
                dst_row[2] = src_row[2];
                src_row += 4;
                dst_row += 3;
            }
            if (row_bytes > meaningful_row) {
                std::memset(dst_row, 0, row_bytes - meaningful_row);
            }
        }
    }

    out_stride = static_cast<std::int32_t>(row_bytes);
    bytes_out = total_bytes;
    return make_result(save_error::none);
}

// Configure the fixed JPEG quality on the frame property bag. The option is
// verified to exist as a VT_R4 property first, so a missing/unsupported option
// is an explicit configuration failure rather than a silent default quality.
save_result configure_jpeg_quality(IPropertyBag2* properties, float quality) noexcept {
    if (!properties) {
        return make_result(save_error::encoder_configuration_failed,
                           "JPEG quality option bag is unavailable");
    }

    bool option_found = false;
    ULONG property_count = 0;
    if (SUCCEEDED(properties->CountProperties(&property_count))) {
        for (ULONG index = 0; index < property_count && !option_found; ++index) {
            PROPBAG2 info{};
            ULONG fetched = 0;
            if (SUCCEEDED(properties->GetPropertyInfo(index, 1, &info, &fetched)) && fetched == 1) {
                if (info.pstrName != nullptr) {
                    option_found = (_wcsicmp(info.pstrName, L"ImageQuality") == 0) &&
                                   (info.vt == VT_R4);
                    CoTaskMemFree(info.pstrName);
                }
            }
        }
    }
    if (!option_found) {
        return make_result(save_error::encoder_configuration_failed,
                           "JPEG encoder does not expose the ImageQuality option");
    }

    // VARIANT and PROPVARIANT share the same layout; the property-bag APIs
    // take VARIANT*.
    VARIANT quality_value{};
    quality_value.vt = VT_R4;
    quality_value.fltVal = quality;

    // WIC property bags may be consumed through the legacy IPropertyBag name
    // write or the native IPropertyBag2 bulk write; accept either, but only a
    // successful write counts as an explicit configuration.
    ComPtr<IPropertyBag> legacy_bag;
    if (SUCCEEDED(properties->QueryInterface(IID_PPV_ARGS(&legacy_bag))) && legacy_bag) {
        if (SUCCEEDED(legacy_bag->Write(L"ImageQuality", &quality_value))) {
            return make_result(save_error::none);
        }
    }
    PROPBAG2 option{};
    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    if (SUCCEEDED(properties->Write(1, &option, &quality_value))) {
        return make_result(save_error::none);
    }
    return make_result(save_error::encoder_configuration_failed,
                       "JPEG encoder rejected the configured quality");
}

// Encode already-converted pixels to `destination` through the in-box WIC
// encoder for `format` (PNG, JPEG, TIFF, BMP). The destination must be an existing
// writable file (the caller reserves it with CREATE_NEW) or a fresh path.
save_result encode_with_wic(const std::filesystem::path& destination, save_format format,
                            const std::uint8_t* pixels, std::int32_t width,
                            std::int32_t height, std::int32_t stride,
                            float quality) noexcept {
    if (!pixels || !dims_within_limits(width, height) || stride <= 0) {
        return make_result(save_error::invalid_request, "invalid encoder input");
    }

    com_init_scope com;
    if (!com.ok()) {
        return make_result(save_error::encoder_unavailable,
                           "COM initialization failed for the WIC encoder");
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        return make_result(save_error::encoder_unavailable, "WIC imaging factory unavailable");
    }

    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr) || !stream) {
        return make_result(save_error::encoder_unavailable, "WIC stream creation failed");
    }
    hr = stream->InitializeFromFilename(destination.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) {
        return make_result(save_error::encode_failed, "temporary stream could not be opened");
    }

    GUID container_format = GUID_ContainerFormatPng;
    const GUID* vendor_guid = nullptr;
    if (format == save_format::png) {
        container_format = GUID_ContainerFormatPng;
    } else if (format == save_format::jpeg) {
        container_format = GUID_ContainerFormatJpeg;
    } else if (format == save_format::tiff) {
        container_format = GUID_ContainerFormatTiff;
        vendor_guid = &GUID_VendorMicrosoft;
    } else if (format == save_format::bmp) {
        container_format = GUID_ContainerFormatBmp;
        vendor_guid = &GUID_VendorMicrosoft;
    } else {
        return make_result(save_error::invalid_request, "unsupported WIC save format");
    }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(container_format, vendor_guid, &encoder);
    if (FAILED(hr) || !encoder) {
        return make_result(save_error::encoder_unavailable,
                           "requested in-box WIC encoder is not available");
    }
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        return make_result(save_error::encoder_configuration_failed,
                           "encoder initialization failed");
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    hr = encoder->CreateNewFrame(&frame, &properties);
    if (FAILED(hr) || !frame) {
        return make_result(save_error::encoder_configuration_failed,
                           "encoder frame creation failed");
    }

    if (format == save_format::jpeg) {
        // Fixed configured quality only: never silently fall back to a default
        // quality. If the encoder cannot take the option, the save fails.
        const save_result quality_result = configure_jpeg_quality(properties.Get(), quality);
        if (quality_result.error != save_error::none) {
            return quality_result;
        }
    } else if (format == save_format::tiff) {
        if (!properties) {
            return make_result(save_error::encoder_configuration_failed,
                               "TIFF frame properties unavailable");
        }
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"TiffCompressionMethod");
        VARIANT val{};
        val.vt = VT_UI1;
        val.bVal = WICTiffCompressionZIP;
        hr = properties->Write(1, &opt, &val);
        if (FAILED(hr)) {
            return make_result(save_error::encoder_configuration_failed,
                               "TIFF encoder rejected ZIP compression configuration");
        }
    }

    hr = frame->Initialize(properties.Get());
    if (FAILED(hr)) {
        return make_result(save_error::encoder_configuration_failed,
                           "encoder frame initialization failed");
    }
    hr = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));
    if (FAILED(hr)) {
        return make_result(save_error::encoder_configuration_failed,
                           "encoder rejected the output dimensions");
    }

    WICPixelFormatGUID pixel_format = (format == save_format::png || format == save_format::tiff)
                                          ? GUID_WICPixelFormat32bppBGRA
                                          : GUID_WICPixelFormat24bppBGR;
    const WICPixelFormatGUID requested_format = pixel_format;
    hr = frame->SetPixelFormat(&pixel_format);
    if (FAILED(hr) || !IsEqualGUID(pixel_format, requested_format)) {
        return make_result(save_error::encoder_configuration_failed,
                           "encoder rejected the requested pixel format");
    }

    const std::size_t buffer_bytes = static_cast<std::size_t>(stride) * static_cast<std::size_t>(height);
    if (buffer_bytes > static_cast<std::size_t>(UINT_MAX)) {
        return make_result(save_error::invalid_request, "encoder buffer exceeds 32-bit extent");
    }
    hr = frame->WritePixels(static_cast<UINT>(height), static_cast<UINT>(stride),
                            static_cast<UINT>(buffer_bytes),
                            const_cast<BYTE*>(pixels));
    if (FAILED(hr)) {
        return make_result(save_error::encode_failed, "pixel encoding failed");
    }

    hr = frame->Commit();
    if (FAILED(hr)) {
        return make_result(save_error::flush_failed, "encoder frame commit failed");
    }
    hr = encoder->Commit();
    if (FAILED(hr)) {
        return make_result(save_error::flush_failed, "encoder commit failed");
    }
    hr = stream->Commit(STGC_DEFAULT);
    if (FAILED(hr)) {
        return make_result(save_error::flush_failed, "encoded stream flush failed");
    }

    // Explicitly release WIC COM interfaces to close underlying file handles
    frame.Reset();
    encoder.Reset();
    stream.Reset();
    factory.Reset();

    // OS-level durability flush: open with write access and FlushFileBuffers.
    // Must fail closed if reopen or flush fails so unconfirmed writes are never committed.
    if (s_inject_reopen_failure.load(std::memory_order_relaxed)) {
        return make_result(save_error::flush_failed, "temporary file reopen failed (injected)");
    }

    const HANDLE hFile = CreateFileW(destination.c_str(), GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        return make_result(save_error::flush_failed,
                           "temporary file reopen for flush failed (win32 " + std::to_string(err) + ")");
    }

    if (s_inject_flush_failure.load(std::memory_order_relaxed)) {
        CloseHandle(hFile);
        return make_result(save_error::flush_failed, "FlushFileBuffers failed (injected)");
    }

    if (!FlushFileBuffers(hFile)) {
        const DWORD err = GetLastError();
        CloseHandle(hFile);
        return make_result(save_error::flush_failed,
                           "FlushFileBuffers failed (win32 " + std::to_string(err) + ")");
    }
    CloseHandle(hFile);

    return make_result(save_error::none);
}

// Encode already-converted straight BGRA pixels to `destination` using static WebP::webp.
save_result encode_with_webp(const std::filesystem::path& destination,
                             const std::uint8_t* pixels, std::int32_t width,
                             std::int32_t height, std::int32_t stride,
                             const std::function<bool()>& cancel_check) noexcept {
#if !defined(OZVIEWER_HAVE_WEBP_ENCODER) || !OZVIEWER_HAVE_WEBP_ENCODER
    (void)destination;
    (void)pixels;
    (void)width;
    (void)height;
    (void)stride;
    (void)cancel_check;
    return make_result(save_error::encoder_unavailable, "WebP encoder is not compiled into this build");
#else
    if (!pixels || !dims_within_limits(width, height) || stride <= 0) {
        return make_result(save_error::invalid_request, "invalid encoder input");
    }
    if (width > WEBP_MAX_DIMENSION || height > WEBP_MAX_DIMENSION) {
        return make_result(save_error::invalid_request, "dimensions exceed WebP limits");
    }

    WebPConfig config;
    if (!WebPConfigInit(&config)) {
        return make_result(save_error::encoder_unavailable, "WebPConfigInit failed");
    }
    if (!WebPConfigLosslessPreset(&config, 6)) {
        return make_result(save_error::encoder_configuration_failed, "WebPConfigLosslessPreset failed");
    }
    config.exact = 1;
    if (!WebPValidateConfig(&config)) {
        return make_result(save_error::encoder_configuration_failed, "WebPValidateConfig failed");
    }

    // Open destination file for writing
    HANDLE hFile = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        // Fall back to creating/overwriting if not pre-created
        hFile = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (hFile == INVALID_HANDLE_VALUE) {
        return make_result(save_error::encode_failed, "could not open temporary file for WebP output");
    }

    struct webp_writer_context {
        HANDLE file = INVALID_HANDLE_VALUE;
        bool write_failed = false;
        std::uint64_t total_written = 0;
    };

    webp_writer_context writer_ctx{hFile, false, 0};

    WebPPicture pic;
    if (!WebPPictureInit(&pic)) {
        CloseHandle(hFile);
        return make_result(save_error::encoder_unavailable, "WebPPictureInit failed");
    }

    pic.use_argb = 1;
    pic.width = width;
    pic.height = height;
    pic.writer = [](const uint8_t* data, size_t data_size, const WebPPicture* picture) -> int {
        if (!picture || !picture->custom_ptr || !data || data_size == 0) {
            return 0;
        }
        auto* ctx = static_cast<webp_writer_context*>(picture->custom_ptr);
        if (ctx->write_failed) {
            return 0;
        }
        size_t written_so_far = 0;
        while (written_so_far < data_size) {
            const DWORD to_write = static_cast<DWORD>(
                std::min<size_t>(data_size - written_so_far, static_cast<size_t>(UINT_MAX)));
            DWORD written = 0;
            if (!WriteFile(ctx->file, data + written_so_far, to_write, &written, nullptr) ||
                written != to_write) {
                ctx->write_failed = true;
                return 0;
            }
            written_so_far += written;
            ctx->total_written += written;
        }
        return 1;
    };
    pic.custom_ptr = &writer_ctx;

    if (cancel_check) {
        pic.user_data = const_cast<void*>(reinterpret_cast<const void*>(&cancel_check));
        pic.progress_hook = [](int percent, const WebPPicture* picture) -> int {
            (void)percent;
            if (picture && picture->user_data) {
                const auto& check = *reinterpret_cast<const std::function<bool()>*>(picture->user_data);
                if (check && check()) {
                    return 0; // abort encoding
                }
            }
            return 1; // continue
        };
    }

    if (!WebPPictureImportBGRA(&pic, pixels, stride)) {
        WebPPictureFree(&pic);
        CloseHandle(hFile);
        return make_result(save_error::materialization_failed, "WebPPictureImportBGRA failed");
    }

    const int encode_ok = WebPEncode(&config, &pic);
    WebPPictureFree(&pic);

    if (!encode_ok) {
        CloseHandle(hFile);
        if (pic.error_code == VP8_ENC_ERROR_USER_ABORT) {
            return make_result(save_error::cancelled, "WebP encoding cancelled by user");
        }
        if (writer_ctx.write_failed || pic.error_code == VP8_ENC_ERROR_BAD_WRITE) {
            return make_result(save_error::encode_failed, "WebP file write failed");
        }
        if (pic.error_code == VP8_ENC_ERROR_OUT_OF_MEMORY ||
            pic.error_code == VP8_ENC_ERROR_BITSTREAM_OUT_OF_MEMORY) {
            return make_result(save_error::materialization_failed, "WebP encoder out of memory");
        }
        if (pic.error_code == VP8_ENC_ERROR_INVALID_CONFIGURATION) {
            return make_result(save_error::encoder_configuration_failed, "WebP invalid configuration");
        }
        return make_result(save_error::encode_failed,
                           "WebP encoding failed (error code " + std::to_string(pic.error_code) + ")");
    }

    if (writer_ctx.write_failed) {
        CloseHandle(hFile);
        return make_result(save_error::encode_failed, "WebP stream write failed");
    }

    // OS-level durability flush
    if (s_inject_flush_failure.load(std::memory_order_relaxed)) {
        CloseHandle(hFile);
        return make_result(save_error::flush_failed, "FlushFileBuffers failed (injected)");
    }

    if (!FlushFileBuffers(hFile)) {
        const DWORD err = GetLastError();
        CloseHandle(hFile);
        return make_result(save_error::flush_failed,
                           "FlushFileBuffers failed (win32 " + std::to_string(err) + ")");
    }
    CloseHandle(hFile);

    if (s_inject_reopen_failure.load(std::memory_order_relaxed)) {
        return make_result(save_error::flush_failed, "temporary file reopen failed (injected)");
    }

    return make_result(save_error::none);
#endif
}

// Cryptographically random, same-directory, unpredictable temp file reserved
// with CREATE_NEW. Returns false with a bounded detail on failure.
bool create_unique_temp_file(const std::filesystem::path& destination,
                             std::filesystem::path& out_temp_path,
                             std::string& out_detail) noexcept {
    const std::filesystem::path directory = destination.parent_path();
    if (directory.empty() || destination.filename().empty()) {
        out_detail = "destination must include a directory and file name";
        return false;
    }

    static constexpr wchar_t kHexDigits[] = L"0123456789abcdef";
    const std::wstring base_name = destination.filename().wstring();

    for (int attempt = 0; attempt < 8; ++attempt) {
        std::uint8_t random_bytes[16]{};
        if (BCryptGenRandom(nullptr, random_bytes, static_cast<ULONG>(sizeof(random_bytes)),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            out_detail = "secure random generation failed";
            return false;
        }

        std::wstring name = base_name;
        name += L".ozsave-";
        for (const std::uint8_t byte : random_bytes) {
            name.push_back(kHexDigits[byte >> 4]);
            name.push_back(kHexDigits[byte & 0x0Fu]);
        }
        name += L".tmp";

        const std::filesystem::path candidate = directory / name;
        const HANDLE handle = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr,
                                          CREATE_NEW,
                                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                          nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            out_temp_path = candidate;
            return true;
        }

        const DWORD last_error = GetLastError();
        if (last_error != ERROR_FILE_EXISTS && last_error != ERROR_ALREADY_EXISTS) {
            out_detail = "temp file creation failed (win32 " + std::to_string(last_error) + ")";
            return false;
        }
    }

    out_detail = "temp file name collision after 8 attempts";
    return false;
}

struct temp_file_guard {
    std::filesystem::path path;
    bool active = true;

    explicit temp_file_guard(std::filesystem::path p) noexcept
        : path(std::move(p)) {}
    temp_file_guard(const temp_file_guard&) = delete;
    temp_file_guard& operator=(const temp_file_guard&) = delete;
    ~temp_file_guard() noexcept {
        if (active) {
            DeleteFileW(path.c_str());
        }
    }

    // Attempt the deletion and report whether the temp file is gone.
    bool cleanup() noexcept {
        if (!active) {
            return true;
        }
        const bool removed = DeleteFileW(path.c_str()) != FALSE;
        active = false;
        return removed;
    }

    void release() noexcept {
        active = false;
    }
};

// Revalidate the captured destination baseline and commit the encoded temp
// file into place. Existing target: ReplaceFileW after fingerprint
// revalidation. New target: MoveFileExW without REPLACE_EXISTING after
// absence revalidation. An ambiguous replacement is never reported as a
// clean-able success.
save_result commit_to_destination(const std::filesystem::path& destination,
                                  const std::filesystem::path& temp_path,
                                  const save_destination_baseline& baseline) noexcept {
    const file_fingerprint current = query_file_fingerprint(destination);

    if (baseline.existed) {
        if (!current.exists || current.file_size != baseline.file_size ||
            current.last_write_time != baseline.last_write_time) {
            return make_result(save_error::destination_conflict,
                               "destination changed since the save request was captured");
        }

        if (ReplaceFileW(destination.c_str(), temp_path.c_str(), nullptr, 0, nullptr, nullptr)) {
            save_result result;
            result.committed = true;
            return result;
        }

        const DWORD last_error = GetLastError();
        if (last_error == ERROR_FILE_NOT_FOUND || last_error == ERROR_PATH_NOT_FOUND) {
            return make_result(save_error::destination_conflict,
                               "destination disappeared before replacement");
        }
        if (last_error == ERROR_ACCESS_DENIED || last_error == ERROR_SHARING_VIOLATION ||
            last_error == ERROR_LOCK_VIOLATION) {
            return make_result(save_error::replace_failed,
                               "destination could not be replaced (win32 " +
                                   std::to_string(last_error) + ")");
        }
        return make_result(save_error::commit_state_uncertain,
                           "replacement outcome is ambiguous (win32 " +
                               std::to_string(last_error) + ")");
    }

    if (current.exists) {
        return make_result(save_error::destination_conflict,
                           "destination appeared since the save request was captured");
    }

    if (MoveFileExW(temp_path.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        save_result result;
        result.committed = true;
        return result;
    }

    const DWORD last_error = GetLastError();
    if (last_error == ERROR_ALREADY_EXISTS || last_error == ERROR_FILE_EXISTS) {
        return make_result(save_error::destination_conflict,
                           "destination appeared during the commit");
    }
    if (query_file_fingerprint(destination).exists) {
        return make_result(save_error::destination_conflict,
                           "destination appeared during the commit");
    }
    return make_result(save_error::replace_failed,
                       "encoded file could not be moved into place (win32 " +
                           std::to_string(last_error) + ")");
}

std::wstring normalized_path_key(const std::filesystem::path& path) {
    std::wstring key = path.lexically_normal().wstring();
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return key;
}

}  // namespace

std::string_view to_string(save_error error) noexcept {
    switch (error) {
        case save_error::none: return "none";
        case save_error::cancelled: return "cancelled";
        case save_error::invalid_request: return "invalid_request";
        case save_error::stale_source: return "stale_source";
        case save_error::destination_conflict: return "destination_conflict";
        case save_error::materialization_failed: return "materialization_failed";
        case save_error::alpha_not_supported: return "alpha_not_supported";
        case save_error::encoder_unavailable: return "encoder_unavailable";
        case save_error::encoder_configuration_failed: return "encoder_configuration_failed";
        case save_error::encode_failed: return "encode_failed";
        case save_error::temp_create_failed: return "temp_create_failed";
        case save_error::flush_failed: return "flush_failed";
        case save_error::replace_failed: return "replace_failed";
        case save_error::commit_state_uncertain: return "commit_state_uncertain";
        case save_error::cleanup_failed: return "cleanup_failed";
    }
    return "unknown";
}

std::string_view to_string(save_format format) noexcept {
    switch (format) {
        case save_format::png: return "png";
        case save_format::jpeg: return "jpeg";
        case save_format::webp: return "webp";
        case save_format::tiff: return "tiff";
        case save_format::bmp: return "bmp";
    }
    return "unknown";
}

bool webp_encoder_available() noexcept {
#if defined(OZVIEWER_HAVE_WEBP_ENCODER) && OZVIEWER_HAVE_WEBP_ENCODER
    return true;
#else
    return false;
#endif
}

std::optional<save_format> save_format_for_extension(std::wstring_view extension) noexcept {
    if (extension.empty()) {
        return std::nullopt;
    }
    std::wstring lower(extension);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    if (lower == L".png") {
        return save_format::png;
    }
    if (lower == L".jpg" || lower == L".jpeg") {
        return save_format::jpeg;
    }
    if (lower == L".webp") {
        return save_format::webp;
    }
    if (lower == L".tif" || lower == L".tiff") {
        return save_format::tiff;
    }
    if (lower == L".bmp") {
        return save_format::bmp;
    }
    return std::nullopt;
}

const wchar_t* preferred_extension(save_format format) noexcept {
    switch (format) {
        case save_format::png: return L".png";
        case save_format::jpeg: return L".jpg";
        case save_format::webp: return L".webp";
        case save_format::tiff: return L".tif";
        case save_format::bmp: return L".bmp";
    }
    return L".png";
}

bool codec_supports_in_place_save(codec_id codec) noexcept {
    if (codec == codec_id::png_wic || codec == codec_id::jpeg || codec == codec_id::bmp_wic) {
        return true;
    }
    if (codec == codec_id::webp) {
        return webp_encoder_available();
    }
    // Note: tiff_wic is Save-As-only per contract.
    return false;
}

save_routing route_save_request(codec_id source_codec, bool has_source_path,
                                bool save_as_requested) noexcept {
    save_routing routing;
    routing.source_supports_in_place = codec_supports_in_place_save(source_codec);

    if (save_as_requested) {
        routing.route = save_route::save_as;
        routing.format = save_format::png;
        return routing;
    }
    if (routing.source_supports_in_place && has_source_path) {
        routing.route = save_route::save_in_place;
        if (source_codec == codec_id::png_wic) {
            routing.format = save_format::png;
        } else if (source_codec == codec_id::jpeg) {
            routing.format = save_format::jpeg;
        } else if (source_codec == codec_id::bmp_wic) {
            routing.format = save_format::bmp;
        } else if (source_codec == codec_id::webp) {
            routing.format = save_format::webp;
        } else {
            routing.format = save_format::png;
        }
        return routing;
    }
    routing.route = save_route::save_as;
    routing.format = save_format::png;
    return routing;
}

file_fingerprint query_file_fingerprint(const std::filesystem::path& path) noexcept {
    file_fingerprint fingerprint;
    if (path.empty()) {
        return fingerprint;
    }

    const HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return fingerprint;
    }

    LARGE_INTEGER size{};
    FILETIME last_write{};
    const bool size_ok = GetFileSizeEx(handle, &size) && size.QuadPart >= 0;
    const bool time_ok = GetFileTime(handle, nullptr, nullptr, &last_write) != FALSE;
    CloseHandle(handle);

    if (!size_ok || !time_ok) {
        return fingerprint;
    }

    ULARGE_INTEGER write_ticks{};
    write_ticks.LowPart = last_write.dwLowDateTime;
    write_ticks.HighPart = last_write.dwHighDateTime;

    fingerprint.exists = true;
    fingerprint.file_size = static_cast<std::uint64_t>(size.QuadPart);
    fingerprint.last_write_time = write_ticks.QuadPart;
    return fingerprint;
}

bool same_file_identity(const std::filesystem::path& a,
                        const std::filesystem::path& b) noexcept {
    if (a.empty() || b.empty()) {
        return false;
    }
    if (normalized_path_key(a) == normalized_path_key(b)) {
        return true;
    }

    const HANDLE handle_a = CreateFileW(a.c_str(), 0,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_a == INVALID_HANDLE_VALUE) {
        return false;
    }
    const HANDLE handle_b = CreateFileW(b.c_str(), 0,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_b == INVALID_HANDLE_VALUE) {
        CloseHandle(handle_a);
        return false;
    }

    BY_HANDLE_FILE_INFORMATION info_a{};
    BY_HANDLE_FILE_INFORMATION info_b{};
    const bool ok = GetFileInformationByHandle(handle_a, &info_a) &&
                    GetFileInformationByHandle(handle_b, &info_b);
    CloseHandle(handle_a);
    CloseHandle(handle_b);

    if (!ok) {
        return false;
    }
    return info_a.dwVolumeSerialNumber == info_b.dwVolumeSerialNumber &&
           info_a.nFileIndexHigh == info_b.nFileIndexHigh &&
           info_a.nFileIndexLow == info_b.nFileIndexLow;
}

save_result encode_png_file(const std::filesystem::path& destination,
                            const std::uint8_t* pixels, std::int32_t width,
                            std::int32_t height, std::int32_t stride) noexcept {
    std::vector<std::uint8_t> converted;
    std::int32_t converted_stride = 0;
    std::size_t converted_bytes = 0;
    const save_result conversion = convert_for_format(
        save_format::png, pixels, width, height, stride, converted, converted_stride,
        converted_bytes);
    if (conversion.error != save_error::none) {
        return conversion;
    }
    return encode_with_wic(destination, save_format::png, converted.data(), width, height,
                           converted_stride, kDefaultJpegQuality);
}

save_result encode_jpeg_file(const std::filesystem::path& destination,
                             const std::uint8_t* pixels, std::int32_t width,
                             std::int32_t height, std::int32_t stride,
                             float quality) noexcept {
    if (!(quality > 0.0f && quality <= 1.0f)) {
        return make_result(save_error::invalid_request, "JPEG quality must be in (0, 1]");
    }
    std::vector<std::uint8_t> converted;
    std::int32_t converted_stride = 0;
    std::size_t converted_bytes = 0;
    const save_result conversion = convert_for_format(
        save_format::jpeg, pixels, width, height, stride, converted, converted_stride,
        converted_bytes);
    if (conversion.error != save_error::none) {
        return conversion;
    }
    return encode_with_wic(destination, save_format::jpeg, converted.data(), width, height,
                           converted_stride, quality);
}

save_result encode_webp_file(const std::filesystem::path& destination,
                             const std::uint8_t* pixels, std::int32_t width,
                             std::int32_t height, std::int32_t stride,
                             const std::function<bool()>& cancel_check) noexcept {
#if !defined(OZVIEWER_HAVE_WEBP_ENCODER) || !OZVIEWER_HAVE_WEBP_ENCODER
    (void)destination;
    (void)pixels;
    (void)width;
    (void)height;
    (void)stride;
    (void)cancel_check;
    return make_result(save_error::encoder_unavailable, "WebP encoder is not compiled into this build");
#else
    std::vector<std::uint8_t> converted;
    std::int32_t converted_stride = 0;
    std::size_t converted_bytes = 0;
    const save_result conversion = convert_for_format(
        save_format::webp, pixels, width, height, stride, converted, converted_stride,
        converted_bytes);
    if (conversion.error != save_error::none) {
        return conversion;
    }
    return encode_with_webp(destination, converted.data(), width, height,
                            converted_stride, cancel_check);
#endif
}

save_result encode_tiff_file(const std::filesystem::path& destination,
                             const std::uint8_t* pixels, std::int32_t width,
                             std::int32_t height, std::int32_t stride) noexcept {
    std::vector<std::uint8_t> converted;
    std::int32_t converted_stride = 0;
    std::size_t converted_bytes = 0;
    const save_result conversion = convert_for_format(
        save_format::tiff, pixels, width, height, stride, converted, converted_stride,
        converted_bytes);
    if (conversion.error != save_error::none) {
        return conversion;
    }
    return encode_with_wic(destination, save_format::tiff, converted.data(), width, height,
                           converted_stride, kDefaultJpegQuality);
}

save_result encode_bmp_file(const std::filesystem::path& destination,
                            const std::uint8_t* pixels, std::int32_t width,
                            std::int32_t height, std::int32_t stride) noexcept {
    std::vector<std::uint8_t> converted;
    std::int32_t converted_stride = 0;
    std::size_t converted_bytes = 0;
    const save_result conversion = convert_for_format(
        save_format::bmp, pixels, width, height, stride, converted, converted_stride,
        converted_bytes);
    if (conversion.error != save_error::none) {
        return conversion;
    }
    return encode_with_wic(destination, save_format::bmp, converted.data(), width, height,
                           converted_stride, kDefaultJpegQuality);
}

save_result execute_save(const save_request& request,
                         const std::function<bool()>& cancel_check) noexcept {
    const auto cancelled = [&cancel_check]() {
        return cancel_check && cancel_check();
    };
    const auto with_cleanup_failure = [](save_result result, bool cleanup_ok) {
        if (!cleanup_ok) {
            result.detail += (result.detail.empty() ? "" : "; ");
            result.detail += "cleanup_failed: temporary file could not be removed";
        }
        return result;
    };

    try {
        // --- request validation -------------------------------------------------
        if (!request.source.is_valid()) {
            return make_result(save_error::invalid_request,
                               "invalid canonical source view (dimensions/stride/buffer)");
        }
        if (request.destination.empty() || request.destination.parent_path().empty() ||
            request.destination.filename().empty()) {
            return make_result(save_error::invalid_request,
                               "destination must be a full path including a directory");
        }
        if (request.format == save_format::jpeg &&
            !(request.jpeg_quality > 0.0f && request.jpeg_quality <= 1.0f)) {
            return make_result(save_error::invalid_request, "invalid JPEG quality");
        }
        if (cancelled()) {
            return make_result(save_error::cancelled, "cancelled before materialization");
        }

        // --- captured source identity revalidation ------------------------------
        if (request.source_fingerprint.expected) {
            const file_fingerprint current =
                query_file_fingerprint(request.source_fingerprint.path);
            if (request.source_fingerprint.path.empty() || !current.exists ||
                current.file_size != request.source_fingerprint.file_size ||
                current.last_write_time != request.source_fingerprint.last_write_time) {
                return make_result(save_error::stale_source,
                                   "source file identity changed before saving");
            }
        }

        // --- exactly one bounded materialization of the captured commands -------
        edit_pipeline_result materialized =
            materialize_edits(request.source, request.commands, request.memory_budget);
        if (materialized.error != edit_pipeline_error::none) {
            const save_error mapped =
                (materialized.error == edit_pipeline_error::invalid_source ||
                 materialized.error == edit_pipeline_error::invalid_command)
                    ? save_error::invalid_request
                    : save_error::materialization_failed;
            return make_result(mapped, materialized.detail.empty()
                                           ? std::string("edit materialization failed")
                                           : materialized.detail);
        }
        if (!materialized.dimensions.is_valid() ||
            materialized.stride < materialized.dimensions.width * 4) {
            return make_result(save_error::materialization_failed,
                               "materialized dimensions or stride are invalid");
        }
        const std::uint64_t materialized_bytes =
            static_cast<std::uint64_t>(materialized.stride) *
            static_cast<std::uint64_t>(materialized.dimensions.height);
        if (materialized.pixels.size() < materialized_bytes) {
            return make_result(save_error::materialization_failed,
                               "materialized pixel buffer is shorter than its extent");
        }

        // Admission for the additional conversion buffer (the materializer's
        // budget covers only its own intermediates).
        const std::size_t budget = request.memory_budget > 0
                                       ? request.memory_budget
                                       : get_system_edit_admission_budget();
        const bool is_32bpp = (request.format == save_format::png ||
                               request.format == save_format::webp ||
                               request.format == save_format::tiff);
        const std::uint64_t converted_stride =
            is_32bpp
                ? static_cast<std::uint64_t>(materialized.dimensions.width) * 4ull
                : (static_cast<std::uint64_t>(materialized.dimensions.width) * 3ull + 3ull) & ~3ull;
        const std::uint64_t converted_bytes =
            converted_stride * static_cast<std::uint64_t>(materialized.dimensions.height);
        // For WebP, WebPPictureImportBGRA copies 32bpp argb pixels internally in WebPPictureAlloc
        const std::uint64_t webp_internal_bytes =
            (request.format == save_format::webp)
                ? (static_cast<std::uint64_t>(materialized.dimensions.width) * 4ull *
                   static_cast<std::uint64_t>(materialized.dimensions.height))
                : 0ull;
        if (converted_bytes > kMaxDecodedBytes ||
            materialized_bytes + converted_bytes + webp_internal_bytes > static_cast<std::uint64_t>(budget)) {
            return make_result(save_error::materialization_failed,
                               "save conversion exceeds the memory admission budget");
        }
        if (cancelled()) {
            return make_result(save_error::cancelled, "cancelled before encoding");
        }

        // --- unique same-directory temp file ------------------------------------
        std::filesystem::path temp_path;
        std::string temp_detail;
        if (!create_unique_temp_file(request.destination, temp_path, temp_detail)) {
            return make_result(save_error::temp_create_failed, std::move(temp_detail));
        }
        temp_file_guard temp_guard(temp_path);

        // --- encode only into the temporary file --------------------------------
        save_result encoded;
        switch (request.format) {
            case save_format::png:
                encoded = encode_png_file(temp_path, materialized.pixels.data(),
                                          materialized.dimensions.width,
                                          materialized.dimensions.height, materialized.stride);
                break;
            case save_format::jpeg:
                encoded = encode_jpeg_file(temp_path, materialized.pixels.data(),
                                           materialized.dimensions.width,
                                           materialized.dimensions.height, materialized.stride,
                                           request.jpeg_quality);
                break;
            case save_format::webp:
                encoded = encode_webp_file(temp_path, materialized.pixels.data(),
                                           materialized.dimensions.width,
                                           materialized.dimensions.height, materialized.stride,
                                           cancel_check);
                break;
            case save_format::tiff:
                encoded = encode_tiff_file(temp_path, materialized.pixels.data(),
                                           materialized.dimensions.width,
                                           materialized.dimensions.height, materialized.stride);
                break;
            case save_format::bmp:
                encoded = encode_bmp_file(temp_path, materialized.pixels.data(),
                                          materialized.dimensions.width,
                                          materialized.dimensions.height, materialized.stride);
                break;
        }
        if (encoded.error != save_error::none) {
            return with_cleanup_failure(std::move(encoded), temp_guard.cleanup());
        }

        if (cancelled()) {
            return with_cleanup_failure(
                make_result(save_error::cancelled, "cancelled before the replacement step"),
                temp_guard.cleanup());
        }

        // --- revalidate source identity just before commit ----------------------
        if (request.source_fingerprint.expected) {
            const file_fingerprint current =
                query_file_fingerprint(request.source_fingerprint.path);
            if (!current.exists ||
                current.file_size != request.source_fingerprint.file_size ||
                current.last_write_time != request.source_fingerprint.last_write_time) {
                return with_cleanup_failure(
                    make_result(save_error::stale_source,
                                "source file identity changed before the replacement step"),
                    temp_guard.cleanup());
            }
        }

        // --- atomic namespace replacement/move ----------------------------------
        save_result committed =
            commit_to_destination(request.destination, temp_path, request.destination_baseline);
        if (committed.committed) {
            temp_guard.release();  // consumed by ReplaceFileW/MoveFileExW
            committed.committed_fingerprint = query_file_fingerprint(request.destination);
            if (!committed.committed_fingerprint.exists) {
                return make_result(save_error::commit_state_uncertain,
                                   "destination verification failed after commit");
            }
            return committed;
        }
        return with_cleanup_failure(std::move(committed), temp_guard.cleanup());
    } catch (const std::bad_alloc&) {
        return make_result(save_error::materialization_failed,
                           "allocation failed while saving");
    } catch (...) {
        return make_result(save_error::encode_failed, "unexpected failure while saving");
    }
}

save_completion_action evaluate_save_completion_policy(
    const save_completion_policy_input& input) noexcept {
    if (!input.committed || input.error != save_error::none || !input.document_matches) {
        return save_completion_action::none;
    }
    if (!input.same_source) {
        return save_completion_action::saved_silent;
    }
    const bool commands_match =
        input.active_commands.size() == input.captured_commands.size() &&
        std::equal(input.active_commands.begin(), input.active_commands.end(),
                   input.captured_commands.begin());
    if (!commands_match) {
        return save_completion_action::saved_commands_changed;
    }
    if (!input.committed_fingerprint.exists) {
        return save_completion_action::saved_unverified_baseline;
    }
    return save_completion_action::checkpoint;
}

bool save_completion_is_for_pending(bool has_pending, std::uint64_t pending_request_id,
                                       std::uint64_t completion_request_id) noexcept {
    return has_pending && pending_request_id == completion_request_id;
}

bool save_pre_submit_eligible(const document_source_baseline& baseline,
                              const file_fingerprint& destination_fingerprint) noexcept {
    if (!baseline.valid) {
        return true;
    }
    return destination_fingerprint.file_size == baseline.file_size &&
           destination_fingerprint.last_write_time == baseline.last_write_time;
}

scoped_durability_flush_failure::scoped_durability_flush_failure(bool fail_reopen) {
    if (fail_reopen) {
        s_inject_reopen_failure.store(true, std::memory_order_relaxed);
    } else {
        s_inject_flush_failure.store(true, std::memory_order_relaxed);
    }
}

scoped_durability_flush_failure::~scoped_durability_flush_failure() {
    s_inject_reopen_failure.store(false, std::memory_order_relaxed);
    s_inject_flush_failure.store(false, std::memory_order_relaxed);
}

}  // namespace oz::viewer_core

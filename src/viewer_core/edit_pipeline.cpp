// oz/viewer_core/edit_pipeline.cpp - bounded pure CPU edit materialization pipeline (Stage 5 Slice 2).
#include "oz/viewer_core/edit_pipeline.hpp"
#include "oz/viewer_core/codec.hpp"

#include <algorithm>
#include <climits>
#include <cstring>
#include <new>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace oz::viewer_core {

namespace {

constexpr std::size_t kMaxMaterializationBudget = 1024ull * 1024ull * 1024ull; // 1 GiB

bool compute_plane_metrics(std::int32_t w, std::int32_t h, std::int32_t& stride, std::size_t& bytes) noexcept {
    if (w <= 0 || h <= 0 || w > kMaxEditDimension || h > kMaxEditDimension) {
        return false;
    }
    const std::uint64_t w64 = static_cast<std::uint64_t>(w);
    const std::uint64_t h64 = static_cast<std::uint64_t>(h);
    const std::uint64_t s64 = w64 * 4ull;
    if (s64 > static_cast<std::uint64_t>(INT32_MAX)) {
        return false;
    }
    const std::uint64_t b64 = s64 * h64;
    if (b64 > kMaxDecodedBytes || b64 > static_cast<std::uint64_t>(SIZE_MAX)) {
        return false;
    }
    stride = static_cast<std::int32_t>(s64);
    bytes = static_cast<std::size_t>(b64);
    return true;
}

// Ensure COM is initialized on the current thread (STA or MTA)
struct com_init_scope {
    HRESULT hr = E_FAIL;
    com_init_scope() noexcept {
        hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (hr == RPC_E_CHANGED_MODE) {
            // Already initialized as STA on this thread; keep it valid
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

// Intermediate buffer holder for ping-pong execution
struct intermediate_buffer {
    std::vector<std::uint8_t> pixels;
    edit_dimensions dims{};
    std::int32_t stride = 0;

    void reset() noexcept {
        pixels.clear();
        pixels.shrink_to_fit();
        dims = {};
        stride = 0;
    }
};

void cpu_rotate_90_cw(
    const std::uint8_t* src, std::int32_t src_w, std::int32_t src_h, std::int32_t src_stride,
    std::uint8_t* dst, std::int32_t /*dst_w*/, std::int32_t /*dst_h*/, std::int32_t dst_stride) noexcept {
    // dst_w = src_h, dst_h = src_w
    // dst(x', y') where x' in [0, dst_w), y' in [0, dst_h)
    // x' = src_h - 1 - y, y' = x
    // => for each src pixel at (x, y):
    //    dst_x = src_h - 1 - y;
    //    dst_y = x;
    for (std::int32_t y = 0; y < src_h; ++y) {
        const std::uint32_t* src_row = reinterpret_cast<const std::uint32_t*>(src + static_cast<std::size_t>(y) * src_stride);
        const std::int32_t dst_x = src_h - 1 - y;
        for (std::int32_t x = 0; x < src_w; ++x) {
            const std::int32_t dst_y = x;
            std::uint32_t* dst_ptr = reinterpret_cast<std::uint32_t*>(dst + static_cast<std::size_t>(dst_y) * dst_stride);
            dst_ptr[dst_x] = src_row[x];
        }
    }
}

void cpu_rotate_90_ccw(
    const std::uint8_t* src, std::int32_t src_w, std::int32_t src_h, std::int32_t src_stride,
    std::uint8_t* dst, std::int32_t /*dst_w*/, std::int32_t /*dst_h*/, std::int32_t dst_stride) noexcept {
    // dst_w = src_h, dst_h = src_w
    // x' = y, y' = src_w - 1 - x
    for (std::int32_t y = 0; y < src_h; ++y) {
        const std::uint32_t* src_row = reinterpret_cast<const std::uint32_t*>(src + static_cast<std::size_t>(y) * src_stride);
        const std::int32_t dst_x = y;
        for (std::int32_t x = 0; x < src_w; ++x) {
            const std::int32_t dst_y = src_w - 1 - x;
            std::uint32_t* dst_ptr = reinterpret_cast<std::uint32_t*>(dst + static_cast<std::size_t>(dst_y) * dst_stride);
            dst_ptr[dst_x] = src_row[x];
        }
    }
}

void cpu_flip_h(
    const std::uint8_t* src, std::int32_t w, std::int32_t h, std::int32_t src_stride,
    std::uint8_t* dst, std::int32_t dst_stride) noexcept {
    for (std::int32_t y = 0; y < h; ++y) {
        const std::uint32_t* src_row = reinterpret_cast<const std::uint32_t*>(src + static_cast<std::size_t>(y) * src_stride);
        std::uint32_t* dst_row = reinterpret_cast<std::uint32_t*>(dst + static_cast<std::size_t>(y) * dst_stride);
        for (std::int32_t x = 0; x < w; ++x) {
            dst_row[w - 1 - x] = src_row[x];
        }
    }
}

void cpu_flip_v(
    const std::uint8_t* src, std::int32_t w, std::int32_t h, std::int32_t src_stride,
    std::uint8_t* dst, std::int32_t dst_stride) noexcept {
    const std::size_t row_bytes = static_cast<std::size_t>(w) * 4;
    for (std::int32_t y = 0; y < h; ++y) {
        const std::uint8_t* src_row = src + static_cast<std::size_t>(y) * src_stride;
        std::uint8_t* dst_row = dst + static_cast<std::size_t>(h - 1 - y) * dst_stride;
        std::memcpy(dst_row, src_row, row_bytes);
    }
}

void cpu_crop(
    const std::uint8_t* src, std::int32_t src_stride,
    std::uint8_t* dst, std::int32_t dst_stride,
    const edit_rect& rect) noexcept {
    const std::size_t row_bytes = static_cast<std::size_t>(rect.width) * 4;
    for (std::int32_t y = 0; y < rect.height; ++y) {
        const std::size_t src_y = static_cast<std::size_t>(rect.y + y);
        const std::size_t src_offset = src_y * src_stride + static_cast<std::size_t>(rect.x) * 4;
        const std::uint8_t* src_row = src + src_offset;
        std::uint8_t* dst_row = dst + static_cast<std::size_t>(y) * dst_stride;
        std::memcpy(dst_row, src_row, row_bytes);
    }
}

}  // namespace

std::size_t compute_edit_admission_budget(std::uint64_t available_phys_bytes) noexcept {
    const std::uint64_t quarter = available_phys_bytes / 4ull;
    const std::uint64_t clamped = (std::min)(static_cast<std::uint64_t>(kMaxMaterializationBudget), quarter);
    return static_cast<std::size_t>(clamped);
}

std::size_t get_system_edit_admission_budget() noexcept {
    MEMORYSTATUSEX mem_status{};
    mem_status.dwLength = sizeof(mem_status);
    if (GlobalMemoryStatusEx(&mem_status)) {
        return compute_edit_admission_budget(mem_status.ullAvailPhys);
    }
    // Fallback conservative 256 MiB
    return 256ull * 1024ull * 1024ull;
}

edit_pipeline_result materialize_edits(
    const edit_source_view& source,
    std::span<const edit_command> active_commands,
    std::size_t memory_budget_bytes) noexcept {
    edit_pipeline_result result;

    if (!source.is_valid()) {
        result.error = edit_pipeline_error::invalid_source;
        result.detail = "Invalid source view dimensions, stride, or pixel buffer";
        return result;
    }

    if (memory_budget_bytes == 0) {
        memory_budget_bytes = get_system_edit_admission_budget();
    }

    // Ping-pong buffers: up to two owned intermediate allocations
    intermediate_buffer buf_a;
    intermediate_buffer buf_b;
    intermediate_buffer* cur_buf = nullptr; // nullptr means source is current

    // Track active intermediate dimensions and pointers
    edit_dimensions cur_dims = source.dimensions;
    std::int32_t cur_stride = source.stride;
    const std::uint8_t* cur_pixels = source.pixels.data();

    // Lazy COM init for WIC resize
    std::unique_ptr<com_init_scope> com_scope;
    ComPtr<IWICImagingFactory> wic_factory;

    for (std::size_t cmd_idx = 0; cmd_idx < active_commands.size(); ++cmd_idx) {
        const auto& cmd = active_commands[cmd_idx];

        // Evaluate target dimensions
        auto target_dims_opt = evaluate_command_dimensions(cur_dims, cmd);
        if (!target_dims_opt.has_value() || !target_dims_opt->is_valid()) {
            result.error = edit_pipeline_error::invalid_command;
            result.detail = "Command is invalid for current intermediate canvas";
            return result;
        }
        const edit_dimensions next_dims = *target_dims_opt;
        std::int32_t next_stride = 0;
        std::size_t next_bytes = 0;
        if (!compute_plane_metrics(next_dims.width, next_dims.height, next_stride, next_bytes)) {
            result.error = edit_pipeline_error::memory_limit;
            result.detail = "Dimensions exceed decoded plane bounds (2 GiB / 65535)";
            return result;
        }

        // Determine destination intermediate buffer (ping-pong)
        intermediate_buffer* next_buf = (cur_buf == &buf_a) ? &buf_b : &buf_a;

        // Account for total owned edit intermediate bytes and WIC estimate
        std::size_t current_owned_bytes = 0;
        if (cur_buf != nullptr) {
            current_owned_bytes = cur_buf->pixels.capacity();
        }

        std::size_t wic_overhead_estimate = 0;
        if (std::holds_alternative<resize_command>(cmd)) {
            // WIC scaler may allocate internal scanline or intermediate tile buffer proportional to target
            wic_overhead_estimate = next_bytes;
        }

        const std::uint64_t total_needed_estimate =
            static_cast<std::uint64_t>(current_owned_bytes) +
            static_cast<std::uint64_t>(next_bytes) +
            static_cast<std::uint64_t>(wic_overhead_estimate);

        if (total_needed_estimate > memory_budget_bytes) {
            result.error = edit_pipeline_error::memory_limit;
            result.detail = "Command execution would exceed memory admission budget";
            return result;
        }

        // Allocate destination buffer
        try {
            if (next_buf->pixels.size() != next_bytes) {
                next_buf->pixels.resize(next_bytes);
            }
        } catch (const std::bad_alloc&) {
            result.error = edit_pipeline_error::allocation_failed;
            result.detail = "Allocation failed during intermediate buffer sizing";
            return result;
        }
        next_buf->dims = next_dims;
        next_buf->stride = next_stride;

        // Execute command
        bool cmd_ok = true;
        std::visit(
            [&](const auto& c) {
                using T = std::decay_t<decltype(c)>;
                if constexpr (std::is_same_v<T, rotate_command>) {
                    if (c.direction == rotate_direction::right_90) {
                        cpu_rotate_90_cw(
                            cur_pixels, cur_dims.width, cur_dims.height, cur_stride,
                            next_buf->pixels.data(), next_dims.width, next_dims.height, next_stride);
                    } else {
                        cpu_rotate_90_ccw(
                            cur_pixels, cur_dims.width, cur_dims.height, cur_stride,
                            next_buf->pixels.data(), next_dims.width, next_dims.height, next_stride);
                    }
                } else if constexpr (std::is_same_v<T, flip_command>) {
                    if (c.direction == flip_direction::horizontal) {
                        cpu_flip_h(
                            cur_pixels, cur_dims.width, cur_dims.height, cur_stride,
                            next_buf->pixels.data(), next_stride);
                    } else {
                        cpu_flip_v(
                            cur_pixels, cur_dims.width, cur_dims.height, cur_stride,
                            next_buf->pixels.data(), next_stride);
                    }
                } else if constexpr (std::is_same_v<T, crop_command>) {
                    cpu_crop(
                        cur_pixels, cur_stride,
                        next_buf->pixels.data(), next_stride,
                        c.rect);
                } else if constexpr (std::is_same_v<T, resize_command>) {
                    if (cur_dims == next_dims) {
                        // Exact dimensions copy
                        const std::size_t row_bytes = static_cast<std::size_t>(cur_dims.width) * 4;
                        for (std::int32_t y = 0; y < cur_dims.height; ++y) {
                            std::memcpy(
                                next_buf->pixels.data() + static_cast<std::size_t>(y) * next_stride,
                                cur_pixels + static_cast<std::size_t>(y) * cur_stride,
                                row_bytes);
                        }
                    } else {
                        // WIC high-quality resize on 32bppPBGRA
                        if (!com_scope) {
                            com_scope = std::make_unique<com_init_scope>();
                            if (!com_scope->ok()) {
                                result.error = edit_pipeline_error::scaler_failed;
                                result.detail = "Failed to initialize COM for WIC scaler";
                                cmd_ok = false;
                                return;
                            }
                        }

                        if (!wic_factory) {
                            HRESULT hr = CoCreateInstance(
                                CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic_factory));
                            if (FAILED(hr) || !wic_factory) {
                                result.error = edit_pipeline_error::scaler_failed;
                                result.detail = "Failed to create WIC Imaging Factory";
                                cmd_ok = false;
                                return;
                            }
                        }

                        // Wrap cur_pixels in IWICBitmap
                        ComPtr<IWICBitmap> src_wic_bmp;
                        const std::size_t cur_buf_bytes = static_cast<std::size_t>(cur_stride) * static_cast<std::size_t>(cur_dims.height);
                        HRESULT hr = wic_factory->CreateBitmapFromMemory(
                            static_cast<UINT>(cur_dims.width),
                            static_cast<UINT>(cur_dims.height),
                            GUID_WICPixelFormat32bppPBGRA,
                            static_cast<UINT>(cur_stride),
                            static_cast<UINT>(cur_buf_bytes),
                            const_cast<BYTE*>(cur_pixels),
                            &src_wic_bmp);
                        if (FAILED(hr) || !src_wic_bmp) {
                            result.error = edit_pipeline_error::scaler_failed;
                            result.detail = "Failed to wrap memory in WIC bitmap";
                            cmd_ok = false;
                            return;
                        }

                        // Determine interpolation mode:
                        // Downscale uses Fant; Upscale uses HighQualityCubic
                        const bool is_downscale = (next_dims.width < cur_dims.width) || (next_dims.height < cur_dims.height);
                        const WICBitmapInterpolationMode filter_mode =
                            is_downscale ? WICBitmapInterpolationModeFant : WICBitmapInterpolationModeHighQualityCubic;

                        ComPtr<IWICBitmapScaler> scaler;
                        hr = wic_factory->CreateBitmapScaler(&scaler);
                        if (FAILED(hr) || !scaler) {
                            result.error = edit_pipeline_error::scaler_failed;
                            result.detail = "Failed to create WIC bitmap scaler";
                            cmd_ok = false;
                            return;
                        }

                        hr = scaler->Initialize(
                            src_wic_bmp.Get(),
                            static_cast<UINT>(next_dims.width),
                            static_cast<UINT>(next_dims.height),
                            filter_mode);
                        if (FAILED(hr)) {
                            result.error = edit_pipeline_error::scaler_failed;
                            result.detail = "Failed to initialize WIC bitmap scaler";
                            cmd_ok = false;
                            return;
                        }

                        hr = scaler->CopyPixels(
                            nullptr,
                            static_cast<UINT>(next_stride),
                            static_cast<UINT>(next_bytes),
                            next_buf->pixels.data());
                        if (FAILED(hr)) {
                            result.error = edit_pipeline_error::scaler_failed;
                            result.detail = "WIC scaler CopyPixels failed";
                            cmd_ok = false;
                            return;
                        }
                    }
                }
            },
            cmd);

        if (!cmd_ok) {
            return result;
        }

        // Release superseded buffer if there was an older one
        if (cur_buf != nullptr) {
            cur_buf->reset();
        }

        // Advance current buffer pointer to newly produced buffer
        cur_buf = next_buf;
        cur_dims = next_dims;
        cur_stride = next_stride;
        cur_pixels = cur_buf->pixels.data();
    }

    if (cur_buf == nullptr) {
        // No commands applied: copy source to result
        std::int32_t out_stride = 0;
        std::size_t out_bytes = 0;
        if (!compute_plane_metrics(source.dimensions.width, source.dimensions.height, out_stride, out_bytes)) {
            result.error = edit_pipeline_error::memory_limit;
            result.detail = "Source dimensions exceed decoded bounds";
            return result;
        }
        if (out_bytes > memory_budget_bytes) {
            result.error = edit_pipeline_error::memory_limit;
            result.detail = "Source copy exceeds memory admission budget";
            return result;
        }
        try {
            result.pixels.resize(out_bytes);
        } catch (const std::bad_alloc&) {
            result.error = edit_pipeline_error::allocation_failed;
            result.detail = "Allocation failed during source copy";
            return result;
        }
        const std::size_t row_bytes = static_cast<std::size_t>(source.dimensions.width) * 4;
        for (std::int32_t y = 0; y < source.dimensions.height; ++y) {
            std::memcpy(
                result.pixels.data() + static_cast<std::size_t>(y) * out_stride,
                source.pixels.data() + static_cast<std::size_t>(y) * source.stride,
                row_bytes);
        }
        result.dimensions = source.dimensions;
        result.stride = out_stride;
        result.error = edit_pipeline_error::none;
        return result;
    }

    // Move materialized pixels to result
    result.dimensions = cur_buf->dims;
    result.stride = cur_buf->stride;
    result.pixels = std::move(cur_buf->pixels);
    result.error = edit_pipeline_error::none;
    return result;
}

}  // namespace oz::viewer_core

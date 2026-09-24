// oz/viewer/print_job.hpp - Stage 9.8 pure single-page fit-to-page print seams.
//
// Dependency-free policy layer between the interactive Print wiring in
// main.cpp and the single-flight print_controller worker. Everything here is
// pure (no HWND, no HDC, no GDI, no filesystem, no threads) so the
// eligibility, page-layout, premultiplied alpha-white composition, request
// identity, and GDI spool-call sequencing contracts are deterministically
// testable without a printer:
//
//   - print_command_enabled: interactive-only eligibility. Enabled only with a
//     presented image, a committed document, a usable source AND a file path
//     (pathless clipboard documents are ineligible), and no modal/busy state
//     (crop, resize, save, copy, print). Benchmark/noninteractive callers pass
//     interactive=false and are always rejected.
//   - compute_print_page_layout: one-page fit-to-page destination in device
//     pixels, aspect-preserving and centered on HORZRES x VERTRES.
//   - compose_pbgra_over_white_to_bgr24: canonical premultiplied BGRA8 over
//     opaque white into a validated top-down 24-bpp BGR DIB with DWORD-aligned
//     rows. Exact integer matte: out = premul + (255 - alpha).
//   - print_completion_is_for_pending: production request-identity gate; a
//     completion is accepted only for the pending request id.
//   - run_single_page_spool: spool-call sequencer over an injectable seam.
//     StartDoc/StartPage/StretchDIBits/EndPage/EndDoc with AbortDoc on every
//     post-start failure or cancellation. Cancellation before StartDoc makes
//     no spool call at all.
//
// Never throws. Single page only: no preview, no pagination, no color
// management, no file export, no renderer readback.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace oz::viewer {

// Bounded error taxonomy for the Print worker. `none` is the only success
// value. `dialog_cancelled` is produced on the UI thread only (the user
// dismissed PrintDlgEx); the worker never reports it.
enum class print_error {
    none,
    cancelled,         // cooperative cancellation (worker teardown / user cancel mid-job)
    dialog_cancelled,  // PrintDlgEx dismissed without PD_RESULT_PRINT (quiet, no spool call)
    invalid_request,   // missing source/path, bad capture, fail-closed Editing-OFF commands
    admission_denied,  // composed DIB exceeds the memory budget
    acquire_failed,    // stable source acquisition/decode/identity check failed
    materialize_failed,// bounded edit materialization or DIB composition failed
    invalid_caps,      // printer HORZRES/VERTRES caps invalid
    spool_failed,      // StartDoc/StartPage/StretchDIBits/EndPage/EndDoc failed
};

[[nodiscard]] inline std::string_view to_string(print_error error) noexcept {
    switch (error) {
        case print_error::none: return "none";
        case print_error::cancelled: return "cancelled";
        case print_error::dialog_cancelled: return "dialog_cancelled";
        case print_error::invalid_request: return "invalid_request";
        case print_error::admission_denied: return "admission_denied";
        case print_error::acquire_failed: return "acquire_failed";
        case print_error::materialize_failed: return "materialize_failed";
        case print_error::invalid_caps: return "invalid_caps";
        case print_error::spool_failed: return "spool_failed";
    }
    return "unknown";
}

// --- printer caps -----------------------------------------------------------

// Raw device caps snapshot. LOGPIXELS values are diagnostics only; the page
// layout is computed in device pixels (HORZRES x VERTRES).
struct print_caps {
    int horzres = 0;
    int vertres = 0;
    int logpixelsx = 0;
    int logpixelsy = 0;
};

// A caps snapshot is usable only with positive printable dimensions.
[[nodiscard]] inline bool print_caps_valid(const print_caps& caps) noexcept {
    return caps.horzres > 0 && caps.vertres > 0;
}

// Diagnostic DPI only. Invalid LOGPIXELS values fall back to 300 DPI so a
// broken driver string can never poison the layout (which never consumes DPI).
[[nodiscard]] inline int print_effective_dpi(int logpixels) noexcept {
    return (logpixels > 0 && logpixels <= 4800) ? logpixels : 300;
}

// --- page layout ------------------------------------------------------------

struct print_page_rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool valid = false;
};

// Single-page fit-to-page destination: the source aspect is preserved, scaled
// to the largest size that fits HORZRES x VERTRES, centered. Invalid when any
// input dimension is not positive or the scaled size underflows.
[[nodiscard]] inline print_page_rect compute_print_page_layout(
    int src_width, int src_height, int horzres, int vertres) noexcept {
    print_page_rect out;
    try {
        if (src_width <= 0 || src_height <= 0 || horzres <= 0 || vertres <= 0) {
            return out;
        }
        const double scale_w = static_cast<double>(horzres) / static_cast<double>(src_width);
        const double scale_h = static_cast<double>(vertres) / static_cast<double>(src_height);
        double scale = (scale_w < scale_h) ? scale_w : scale_h;
        if (!(scale > 0.0)) {
            return out;
        }
        const long long dw = static_cast<long long>(static_cast<double>(src_width) * scale);
        const long long dh = static_cast<long long>(static_cast<double>(src_height) * scale);
        if (dw < 1 || dh < 1 || dw > horzres || dh > vertres) {
            return out;
        }
        out.width = static_cast<int>(dw);
        out.height = static_cast<int>(dh);
        out.x = (horzres - out.width) / 2;
        out.y = (vertres - out.height) / 2;
        out.valid = true;
        return out;
    } catch (...) {
        print_page_rect invalid;
        return invalid;
    }
}

// --- premultiplied alpha-white composition ----------------------------------

struct print_bgr24_dib {
    print_error error = print_error::none;
    std::vector<std::uint8_t> pixels;  // top-down 24-bpp BGR, DWORD-aligned rows
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t stride = 0;  // bytes per row, multiple of 4

    [[nodiscard]] bool is_valid() const noexcept {
        return error == print_error::none && width > 0 && height > 0 && stride >= width * 3 &&
               (stride % 4) == 0 && !pixels.empty() &&
               pixels.size() >= static_cast<std::size_t>(stride) * static_cast<std::size_t>(height);
    }
};

// Composite canonical top-down premultiplied BGRA8 over opaque white into a
// validated top-down 24-bpp BGR DIB. Exact integer matte per channel:
// out = premul + (255 - alpha), which equals white blended under the
// premultiplied source with no rounding error. `memory_budget` bounds the
// composed allocation (0 = unbounded); exceeding it is admission_denied.
// Never throws; allocation faults are materialize_failed.
[[nodiscard]] inline print_bgr24_dib compose_pbgra_over_white_to_bgr24(
    const std::uint8_t* pbgra, std::int32_t width, std::int32_t height,
    std::int32_t src_stride, std::size_t memory_budget = 0) noexcept {
    print_bgr24_dib out;
    try {
        if (pbgra == nullptr || width <= 0 || height <= 0 || width > 65535 || height > 65535) {
            out.error = print_error::invalid_request;
            return out;
        }
        if (src_stride < width * 4) {
            out.error = print_error::invalid_request;
            return out;
        }
        const std::uint64_t row_bytes = static_cast<std::uint64_t>(width) * 3u;
        const std::uint64_t aligned_row = (row_bytes + 3u) & ~std::uint64_t{3u};
        const std::uint64_t required =
            aligned_row * static_cast<std::uint64_t>(height);
        constexpr std::uint64_t kMaxDibBytes = std::uint64_t{2u} << 30u;  // 2 GiB, mirrors the pipeline ceiling
        if (required == 0 || required > kMaxDibBytes ||
            aligned_row > static_cast<std::uint64_t>(INT32_MAX)) {
            out.error = print_error::invalid_request;
            return out;
        }
        if (memory_budget > 0 && required > memory_budget) {
            out.error = print_error::admission_denied;
            return out;
        }
        std::vector<std::uint8_t> bgr;
        try {
            bgr.resize(static_cast<std::size_t>(required), 0);
        } catch (const std::bad_alloc&) {
            out.error = print_error::materialize_failed;
            return out;
        } catch (const std::length_error&) {
            out.error = print_error::materialize_failed;
            return out;
        }
        const std::size_t dst_stride = static_cast<std::size_t>(aligned_row);
        for (std::int32_t y = 0; y < height; ++y) {
            const std::uint8_t* src_row = pbgra + static_cast<std::size_t>(y) * static_cast<std::size_t>(src_stride);
            std::uint8_t* dst_row = bgr.data() + static_cast<std::size_t>(y) * dst_stride;
            for (std::int32_t x = 0; x < width; ++x) {
                const std::uint8_t b = src_row[x * 4 + 0];
                const std::uint8_t g = src_row[x * 4 + 1];
                const std::uint8_t r = src_row[x * 4 + 2];
                const std::uint8_t a = src_row[x * 4 + 3];
                const unsigned inv = static_cast<unsigned>(255u - a);
                // Clamp is structural: premultiplied channels never exceed
                // alpha, so b/g/r + (255 - a) <= 255 always.
                unsigned cb = static_cast<unsigned>(b) + inv;
                unsigned cg = static_cast<unsigned>(g) + inv;
                unsigned cr = static_cast<unsigned>(r) + inv;
                dst_row[x * 3 + 0] = static_cast<std::uint8_t>(cb > 255u ? 255u : cb);
                dst_row[x * 3 + 1] = static_cast<std::uint8_t>(cg > 255u ? 255u : cg);
                dst_row[x * 3 + 2] = static_cast<std::uint8_t>(cr > 255u ? 255u : cr);
            }
        }
        out.pixels = std::move(bgr);
        out.width = width;
        out.height = height;
        out.stride = static_cast<std::int32_t>(aligned_row);
        out.error = print_error::none;
        return out;
    } catch (...) {
        print_bgr24_dib failed;
        failed.error = print_error::materialize_failed;
        return failed;
    }
}

// --- eligibility ------------------------------------------------------------

struct print_eligibility_input {
    bool interactive = false;  // false in --benchmark / --navigation-benchmark
    bool have_image = false;
    bool has_document = false;
    bool has_source = false;
    bool has_file_path = false;  // pathless (clipboard) documents are ineligible
    bool crop_active = false;
    bool resize_busy = false;
    bool save_busy = false;
    bool copy_busy = false;
    bool print_busy = false;  // single-flight: at most one pending/in-flight print
};

// Pure pre-submit eligibility for the Print command. Mirrors the UI-thread
// gate in the viewer's start_print exactly so it is deterministically
// testable. Never throws.
[[nodiscard]] inline bool print_command_enabled(const print_eligibility_input& in) noexcept {
    if (!in.interactive) {
        return false;
    }
    if (!in.have_image || !in.has_document) {
        return false;
    }
    if (!in.has_source || !in.has_file_path) {
        return false;
    }
    if (in.crop_active || in.resize_busy || in.save_busy || in.copy_busy || in.print_busy) {
        return false;
    }
    return true;
}

// --- request identity -------------------------------------------------------

// Production request-identity gate for print completions. Mirrors the
// UI-thread guard in the viewer's handle_print_completion exactly: a
// completion is eligible only when a print is pending AND its request ID
// matches the pending request. Never throws.
[[nodiscard]] inline bool print_completion_is_for_pending(
    bool has_pending, std::uint64_t pending_request_id,
    std::uint64_t completion_request_id) noexcept {
    return has_pending && pending_request_id != 0 &&
           completion_request_id == pending_request_id;
}

// --- GDI spool-call seam ----------------------------------------------------

enum class print_spool_outcome {
    printed,
    cancelled,
    spool_failed,
};

// Injectable single-page spool calls. Production binds these to the real GDI
// printer-DC functions; tests bind recording fakes. Never throws by contract;
// a null entry is treated as a spool failure.
struct print_spool_seam {
    std::function<bool()> begin_doc;   // StartDoc
    std::function<bool()> begin_page;  // StartPage
    std::function<bool()> stretch;     // StretchDIBits for the single page
    std::function<bool()> end_page;    // EndPage
    std::function<bool()> end_doc;     // EndDoc
    std::function<void()> abort;       // AbortDoc
};

// Run the one-page spool sequence: StartDoc, StartPage, StretchDIBits,
// EndPage, EndDoc. Cancellation before StartDoc makes no spool call and
// reports cancelled. AbortDoc runs on every post-StartDoc failure or cancel
// (including EndDoc failure) so a partial job never lingers in the spooler.
// Never throws.
[[nodiscard]] inline print_spool_outcome run_single_page_spool(
    const print_spool_seam& seam,
    const std::function<bool()>& cancel_check) noexcept {
    try {
        const auto cancelled = [&]() noexcept -> bool {
            try {
                return static_cast<bool>(cancel_check) && cancel_check();
            } catch (...) {
                return false;
            }
        };
        const auto abort_now = [&]() noexcept {
            try {
                if (static_cast<bool>(seam.abort)) {
                    seam.abort();
                }
            } catch (...) {
            }
        };
        const auto call = [&](const std::function<bool()>& fn) noexcept -> bool {
            try {
                return static_cast<bool>(fn) && fn();
            } catch (...) {
                return false;
            }
        };

        if (cancelled()) {
            return print_spool_outcome::cancelled;
        }
        if (!call(seam.begin_doc)) {
            return print_spool_outcome::spool_failed;
        }
        if (cancelled()) {
            abort_now();
            return print_spool_outcome::cancelled;
        }
        if (!call(seam.begin_page)) {
            abort_now();
            return print_spool_outcome::spool_failed;
        }
        if (cancelled()) {
            abort_now();
            return print_spool_outcome::cancelled;
        }
        if (!call(seam.stretch)) {
            abort_now();
            return print_spool_outcome::spool_failed;
        }
        if (!call(seam.end_page)) {
            abort_now();
            return print_spool_outcome::spool_failed;
        }
        if (cancelled()) {
            abort_now();
            return print_spool_outcome::cancelled;
        }
        if (!call(seam.end_doc)) {
            abort_now();
            return print_spool_outcome::spool_failed;
        }
        return print_spool_outcome::printed;
    } catch (...) {
        return print_spool_outcome::spool_failed;
    }
}

}  // namespace oz::viewer

// oz/viewer/print_controller.cpp - single-flight Print worker implementation
// (Stage 9.8). Materializes once from the immutable canonical source,
// composites over opaque white to a 24-bpp BGR DIB, and spools one
// fit-to-page page. Never touches the renderer, history, dirty state, or the
// save checkpoint.
#include "print_controller.hpp"

#include "oz/viewer_core/codec.hpp"
#include "viewer_strings.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wingdi.h>

#if OZVIEWER_ENABLE_EDITING
#include "oz/viewer_core/edit_pipeline.hpp"
#endif

namespace oz::viewer {

namespace {

// Admission policy mirror for the composed DIB when the caller passes no
// budget (Editing-OFF always passes 0 so the edit-pipeline budget helper,
// which is excluded from OFF builds, is never linked): min(1 GiB,
// available physical / 4), 256 MiB fallback. Same numbers as the Stage 5
// admission policy; computed locally so both Editing configurations link.
std::size_t print_default_memory_budget() noexcept {
    constexpr std::uint64_t kCap = 1024ull * 1024ull * 1024ull;  // 1 GiB
    MEMORYSTATUSEX mem_status{};
    mem_status.dwLength = sizeof(mem_status);
    if (GlobalMemoryStatusEx(&mem_status)) {
        const std::uint64_t quarter = mem_status.ullAvailPhys / 4ull;
        return static_cast<std::size_t>(quarter < kCap ? quarter : kCap);
    }
    return 256ull * 1024ull * 1024ull;
}

}  // namespace

void release_print_dc(void* printer_dc) noexcept {
    try {
        if (printer_dc) {
            DeleteDC(static_cast<HDC>(printer_dc));
        }
    } catch (...) {
    }
}

void print_controller::worker_loop() {
    while (true) {
        print_work_request req;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return stop_requested_.load(std::memory_order_relaxed) || has_pending_;
            });
            if (stop_requested_.load(std::memory_order_relaxed)) {
                break;
            }
            if (!has_pending_) {
                continue;
            }
            req = std::move(pending_request_);
            has_pending_ = false;
            pending_flag_.store(false, std::memory_order_release);
        }

        const auto cancel_check = [this] {
            return stop_requested_.load(std::memory_order_relaxed) ||
                   cancel_requested_.load(std::memory_order_relaxed);
        };

        print_completion_msg comp;
        comp.request_id = req.request_id;
        comp.document_instance_id = req.document_instance_id;
        comp.diagnostic_revision = req.diagnostic_revision;
        comp.document_generation = req.document_generation;

        // Exclusive HDC ownership: released exactly once on every path below.
        HDC hdc = static_cast<HDC>(req.printer_dc);
        struct dc_guard {
            HDC h = nullptr;
            ~dc_guard() {
                if (h) {
                    DeleteDC(h);
                }
            }
        };
        dc_guard guard{hdc};
        req.printer_dc = nullptr;

        auto finish = [&](print_completion_msg c) {
            std::lock_guard<std::mutex> lock(comp_mutex_);
            completions_.push_back(std::move(c));
            if (hwnd_) {
                PostMessageW(static_cast<HWND>(hwnd_), notify_msg_, 0, 0);
            }
        };

        if (cancel_check()) {
            comp.ok = false;
            comp.error = print_error::cancelled;
            comp.detail = "cancelled before the print started";
            finish(std::move(comp));
            continue;
        }
        if (hdc == nullptr) {
            comp.ok = false;
            comp.error = print_error::invalid_request;
            comp.detail = "no printer device context transferred to the print worker";
            finish(std::move(comp));
            continue;
        }

        std::shared_ptr<const edit_source_buffer> source = req.existing_source;

        // Optional source-only full-resolution acquisition: an immutable
        // decode of the original file, never a materialized edit result.
        if (req.needs_source_acquisition) {
            std::vector<std::uint8_t> encoded;
            std::string read_error;
            std::uint64_t stable_size = 0;
            std::uint64_t stable_write = 0;
            if (!read_file_stable_identity(req.source_path, encoded, req.expected_file_size,
                                           req.expected_last_write_time, stable_size,
                                           stable_write, read_error, cancel_check)) {
                if (cancel_check()) {
                    comp.ok = false;
                    comp.error = print_error::cancelled;
                    comp.detail = "cancelled during source acquisition";
                } else {
                    comp.ok = false;
                    comp.error = print_error::acquire_failed;
                    comp.detail = read_error.empty() ? "source acquisition failed" : read_error;
                }
                finish(std::move(comp));
                continue;
            }
            if (cancel_check()) {
                comp.ok = false;
                comp.error = print_error::cancelled;
                comp.detail = "cancelled after source acquisition";
                finish(std::move(comp));
                continue;
            }
            auto decoded = oz::viewer_core::decode_image(encoded.data(), encoded.size(), 1);
            if (decoded.error != oz::viewer_core::codec_error::none) {
                comp.ok = false;
                comp.error = print_error::acquire_failed;
                comp.detail = decoded.detail.empty() ? "full-resolution decode failed"
                                                     : decoded.detail;
                finish(std::move(comp));
                continue;
            }
            auto buffer = std::make_shared<edit_source_buffer>();
            buffer->document_instance_id = req.document_instance_id;
            buffer->source_path = req.source_path;
            buffer->file_size = stable_size;
            buffer->last_write_time = stable_write;
            buffer->dimensions = {decoded.image.width, decoded.image.height};
            buffer->stride = decoded.image.stride;
            buffer->pixels = std::move(decoded.image.pixels);

            if (req.expected_dimensions.is_valid() &&
                buffer->dimensions != req.expected_dimensions) {
                comp.ok = false;
                comp.error = print_error::acquire_failed;
                comp.detail = "decoded source dimensions do not match document metadata";
                finish(std::move(comp));
                continue;
            }
            source = std::move(buffer);
        }

        if (!source || !source->is_valid()) {
            comp.ok = false;
            comp.error = print_error::invalid_request;
            comp.detail = "no valid immutable source available for printing";
            finish(std::move(comp));
            continue;
        }

        // Materialize the captured command sequence exactly once into
        // canonical PBGRA8. The source and caller state are never mutated.
        const std::uint8_t* pbgr_pixels = nullptr;
        std::int32_t pbgr_width = 0;
        std::int32_t pbgr_height = 0;
        std::int32_t pbgr_stride = 0;
        std::vector<std::uint8_t> materialized;
#if OZVIEWER_ENABLE_EDITING
        if (req.commands.empty()) {
            pbgr_pixels = source->pixels.data();
            pbgr_width = source->dimensions.width;
            pbgr_height = source->dimensions.height;
            pbgr_stride = source->stride;
        } else {
            const auto src_view = source->make_view();
            const std::size_t budget =
                req.memory_budget > 0 ? req.memory_budget : print_default_memory_budget();
            auto mat_res = oz::viewer_core::materialize_edits(
                src_view,
                std::span<const oz::viewer_core::edit_command>(req.commands.data(),
                                                               req.commands.size()),
                budget);
            if (mat_res.error != oz::viewer_core::edit_pipeline_error::none) {
                comp.ok = false;
                comp.error = print_error::materialize_failed;
                comp.detail =
                    mat_res.detail.empty() ? "edit materialization failed" : mat_res.detail;
                finish(std::move(comp));
                continue;
            }
            materialized = std::move(mat_res.pixels);
            pbgr_width = mat_res.dimensions.width;
            pbgr_height = mat_res.dimensions.height;
            pbgr_stride = mat_res.stride;
            pbgr_pixels = materialized.data();
        }
#else
        if (!req.commands.empty()) {
            // Editing-OFF builds exclude the edit pipeline, and the
            // Editing-OFF print path never submits commands, so fail closed
            // without linking the materialization engine.
            comp.ok = false;
            comp.error = print_error::invalid_request;
            comp.detail = "edit commands not supported when editing is disabled";
            finish(std::move(comp));
            continue;
        }
        pbgr_pixels = source->pixels.data();
        pbgr_width = source->dimensions.width;
        pbgr_height = source->dimensions.height;
        pbgr_stride = source->stride;
#endif

        // Composite over opaque white to a validated top-down 24-bpp BGR DIB.
        const std::size_t dib_budget =
            req.memory_budget > 0 ? req.memory_budget : print_default_memory_budget();
        print_bgr24_dib dib =
            compose_pbgra_over_white_to_bgr24(pbgr_pixels, pbgr_width, pbgr_height,
                                              pbgr_stride, dib_budget);
        // Worker-local materialized pixels are no longer needed past composition.
        materialized.clear();
        materialized.shrink_to_fit();
        if (!dib.is_valid()) {
            comp.ok = false;
            comp.error = dib.error;
            if (dib.error == print_error::admission_denied) {
                comp.detail = "print image exceeds the memory budget";
            } else if (dib.error == print_error::invalid_request) {
                comp.detail = "invalid print image dimensions";
            } else {
                comp.detail = "print image composition failed";
            }
            finish(std::move(comp));
            continue;
        }

        // Printer caps: one-page fit-to-page over HORZRES x VERTRES.
        // LOGPIXELSX/Y feed diagnostics only, with a 300-DPI fallback when
        // the driver reports invalid values.
        print_caps caps;
        caps.horzres = GetDeviceCaps(hdc, HORZRES);
        caps.vertres = GetDeviceCaps(hdc, VERTRES);
        caps.logpixelsx = GetDeviceCaps(hdc, LOGPIXELSX);
        caps.logpixelsy = GetDeviceCaps(hdc, LOGPIXELSY);
        if (!print_caps_valid(caps)) {
            comp.ok = false;
            comp.error = print_error::invalid_caps;
            comp.detail = "printer reports invalid page dimensions";
            finish(std::move(comp));
            continue;
        }
        (void)print_effective_dpi(caps.logpixelsx);
        (void)print_effective_dpi(caps.logpixelsy);

        const print_page_rect dest =
            compute_print_page_layout(dib.width, dib.height, caps.horzres, caps.vertres);
        if (!dest.valid) {
            comp.ok = false;
            comp.error = print_error::invalid_caps;
            comp.detail = "print page layout failed for the reported printer caps";
            finish(std::move(comp));
            continue;
        }

        // Spool the single page through the pure sequencer over real GDI
        // calls. AbortDoc runs on every post-StartDoc failure or cancel.
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = dib.width;
        bmi.bmiHeader.biHeight = -dib.height;  // top-down DIB
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 24;
        bmi.bmiHeader.biCompression = BI_RGB;

        // Owning local: StartDocW consumes lpszDocName synchronously, but the
        // pointer is retained in DOCINFOW across the seam calls below.
        const std::wstring doc_name = viewer_string(IDS_APP_TITLE);
        DOCINFOW doc_info{};
        doc_info.cbSize = sizeof(doc_info);
        doc_info.lpszDocName = doc_name.c_str();

        std::string fail_stage;
        print_spool_seam seam;
        seam.begin_doc = [&]() -> bool {
            const int job = StartDocW(hdc, &doc_info);
            if (job <= 0) {
                fail_stage = "StartDoc";
            }
            return job > 0;
        };
        seam.begin_page = [&]() -> bool {
            const int rc = StartPage(hdc);
            if (rc <= 0) {
                fail_stage = "StartPage";
            }
            return rc > 0;
        };
        seam.stretch = [&]() -> bool {
            const int rows = StretchDIBits(hdc, dest.x, dest.y, dest.width, dest.height,
                                           0, 0, dib.width, dib.height, dib.pixels.data(),
                                           &bmi, DIB_RGB_COLORS, SRCCOPY);
            if (rows == GDI_ERROR) {
                fail_stage = "StretchDIBits";
            }
            return rows != GDI_ERROR;
        };
        seam.end_page = [&]() -> bool {
            const int rc = EndPage(hdc);
            if (rc <= 0) {
                fail_stage = "EndPage";
            }
            return rc > 0;
        };
        seam.end_doc = [&]() -> bool {
            const int rc = EndDoc(hdc);
            if (rc <= 0) {
                fail_stage = "EndDoc";
            }
            return rc > 0;
        };
        seam.abort = [&]() noexcept {
            try {
                AbortDoc(hdc);
            } catch (...) {
            }
        };

        const print_spool_outcome spool_out =
            run_single_page_spool(seam, cancel_check);
        if (spool_out == print_spool_outcome::printed) {
            comp.ok = true;
            comp.error = print_error::none;
            comp.detail.clear();
        } else if (spool_out == print_spool_outcome::cancelled) {
            comp.ok = false;
            comp.error = print_error::cancelled;
            comp.detail = "cancelled during printing";
        } else {
            comp.ok = false;
            comp.error = print_error::spool_failed;
            comp.detail = fail_stage.empty() ? "printer spool call failed"
                                             : ("printer spool failed at " + fail_stage);
        }
        finish(std::move(comp));
    }
}

}  // namespace oz::viewer

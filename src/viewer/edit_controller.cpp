// oz/viewer/edit_controller.cpp - dedicated edit materialization controller implementation (Stage 5 Slice 3).
// Note: the shared stable file-identity helpers now live in the always-built
// source_identity.cpp so the Editing-OFF copy path links; this TU keeps only
// the edit worker loop.
#include "edit_controller.hpp"
#include "oz/viewer_core/codec.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace oz::viewer {

void edit_controller::worker_loop() {
    while (true) {
        edit_work_request req;
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
        }

        auto check_superseded = [this, current_req_id = req.request_id] {
            if (stop_requested_.load(std::memory_order_relaxed)) return true;
            std::lock_guard<std::mutex> lock(mutex_);
            return has_pending_ && pending_request_.request_id > current_req_id;
        };

        if (check_superseded()) {
            continue;
        }

        edit_completion_msg comp;
        comp.document_instance_id = req.document_instance_id;
        comp.edit_revision = req.edit_revision;
        comp.request_id = req.request_id;
        comp.ok = true;

        std::shared_ptr<const edit_source_buffer> active_source = req.existing_source;

        // 1. Source acquisition if requested
        if (req.needs_source_acquisition) {
            std::vector<std::uint8_t> encoded;
            std::string read_err;
            std::uint64_t stable_sz = 0;
            std::uint64_t stable_ft = 0;
            if (!read_file_stable_identity(req.source_path, encoded, req.expected_file_size,
                                           req.expected_last_write_time, stable_sz, stable_ft,
                                           read_err, check_superseded)) {
                if (check_superseded()) continue;
                comp.ok = false;
                comp.error_detail = read_err;
            } else if (!check_superseded()) {
                // Full resolution decode (scale_den = 1)
                auto reg = oz::viewer_core::decode_image(encoded.data(), encoded.size(), 1);
                if (reg.error != oz::viewer_core::codec_error::none) {
                    comp.ok = false;
                    comp.error_detail = reg.detail.empty() ? "Full decode failed" : reg.detail;
                } else {
                    auto buf = std::make_shared<edit_source_buffer>();
                    buf->document_instance_id = req.document_instance_id;
                    buf->source_path = req.source_path;
                    buf->file_size = stable_sz;
                    buf->last_write_time = stable_ft;
                    buf->dimensions = {reg.image.width, reg.image.height};
                    buf->stride = reg.image.stride;
                    buf->pixels = std::move(reg.image.pixels);

                    if (req.expected_dimensions.is_valid() &&
                        buf->dimensions != req.expected_dimensions) {
                        comp.ok = false;
                        comp.error_detail = "Decoded source dimensions do not match expected metadata";
                    } else {
                        active_source = std::move(buf);
                        comp.source = active_source;
                    }
                }
            }
        }

        if (check_superseded()) {
            continue;
        }

        // 2. Materialization if requested and source is available
        if (comp.ok && req.needs_materialization) {
            if (!active_source || !active_source->is_valid()) {
                comp.ok = false;
                comp.error_detail = "No valid full-resolution source available for materialization";
            } else {
                const auto src_view = active_source->make_view();
                const std::size_t budget = (req.memory_budget > 0)
                    ? req.memory_budget
                    : oz::viewer_core::get_system_edit_admission_budget();

                comp.result = oz::viewer_core::materialize_edits(src_view, req.commands, budget);
                if (comp.result.error != oz::viewer_core::edit_pipeline_error::none) {
                    comp.ok = false;
                    comp.error_detail = comp.result.detail.empty() ? "Edit materialization failed" : comp.result.detail;
                }
            }
        }

        if (check_superseded()) {
            continue;
        }

        // Push completion and wake up UI thread
        {
            std::lock_guard<std::mutex> lock(comp_mutex_);
            completions_.push_back(std::move(comp));
        }

        if (hwnd_) {
            PostMessageW(static_cast<HWND>(hwnd_), notify_msg_, 0, 0);
        }
    }
}

}  // namespace oz::viewer

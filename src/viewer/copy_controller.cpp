// oz/viewer/copy_controller.cpp - dedicated latest-only Copy Image worker implementation (Stage 9.4).
#include "copy_controller.hpp"

#include "oz/viewer_core/codec.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace oz::viewer {

void copy_controller::worker_loop() {
    while (true) {
        copy_work_request req;
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

        const auto cancel_check = [this] {
            return stop_requested_.load(std::memory_order_relaxed) ||
                   cancel_requested_.load(std::memory_order_relaxed);
        };

        copy_completion_msg comp;
        comp.request_id = req.request_id;
        comp.document_instance_id = req.document_instance_id;
        comp.diagnostic_revision = req.diagnostic_revision;
        comp.document_generation = req.document_generation;

        if (cancel_check()) {
            comp.ok = false;
            comp.error_detail = "cancelled before copy started";
        } else {
            std::shared_ptr<const edit_source_buffer> source = req.existing_source;

            // Acquire source if needed
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
                        comp.error_detail = "cancelled during source acquisition";
                    } else {
                        comp.ok = false;
                        comp.error_detail = read_error;
                    }
                } else if (cancel_check()) {
                    comp.ok = false;
                    comp.error_detail = "cancelled after source acquisition";
                } else {
                    auto decoded = oz::viewer_core::decode_image(encoded.data(), encoded.size(), 1);
                    if (decoded.error != oz::viewer_core::codec_error::none) {
                        comp.ok = false;
                        comp.error_detail = decoded.detail.empty() ? "full-resolution decode failed" : decoded.detail;
                    } else {
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
                            comp.error_detail = "decoded source dimensions do not match document metadata";
                        } else {
                            source = std::move(buffer);
                        }
                    }
                }
            }

            if (source && source->is_valid() && comp.error_detail.empty()) {
                if (req.commands.empty()) {
                    // Orientation-applied full-res source directly
                    comp.dimensions = source->dimensions;
                    comp.stride = source->stride;
                    comp.pixels = source->pixels;
                    comp.ok = true;
                }
#if OZVIEWER_ENABLE_EDITING
                else {
                    // Materialize active edit commands (Editing-ON only; the
                    // Editing-OFF copy path never submits commands).
                    const auto src_view = source->make_view();
                    const std::size_t budget = req.memory_budget > 0
                        ? req.memory_budget
                        : oz::viewer_core::get_system_edit_admission_budget();
                    auto mat_res = oz::viewer_core::materialize_edits(src_view, req.commands, budget);
                    if (mat_res.error != oz::viewer_core::edit_pipeline_error::none) {
                        comp.ok = false;
                        comp.error_detail = mat_res.detail.empty() ? "edit materialization failed" : mat_res.detail;
                    } else {
                        comp.dimensions = mat_res.dimensions;
                        comp.stride = mat_res.stride;
                        comp.pixels = std::move(mat_res.pixels);
                        comp.ok = true;
                    }
                }
#else
                else {
                    // Editing-OFF builds exclude the edit pipeline, and the
                    // Editing-OFF copy path never submits commands, so fail
                    // closed without linking the materialization engine.
                    comp.ok = false;
                    comp.error_detail = "edit commands not supported when editing is disabled";
                }
#endif
            } else if (comp.error_detail.empty()) {
                comp.ok = false;
                comp.error_detail = "no valid immutable source available for copy";
            }
        }

        // Post completion to UI thread
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

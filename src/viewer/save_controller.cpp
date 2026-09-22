// oz/viewer/save_controller.cpp - single-flight Save worker implementation
// (Stage 5 Slice 6). Materializes once from the immutable canonical source,
// encodes to a unique same-directory temp file and atomically commits.
#include "save_controller.hpp"

#include "oz/viewer_core/codec.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>

namespace oz::viewer {

namespace {

// Bounded chunked read with cooperative cancellation, used only when the
// worker has to acquire the immutable full-resolution source.
bool read_file_all_bounded(const std::filesystem::path& path,
                           std::vector<std::uint8_t>& out_data,
                           std::string& out_detail,
                           const std::function<bool()>& check_cancel) {
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        out_detail = "source file open failed";
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > oz::viewer_core::kMaxEncodedBytes) {
        CloseHandle(handle);
        out_detail = "source file too large";
        return false;
    }

    try {
        out_data.resize(static_cast<std::size_t>(size.QuadPart));
    } catch (const std::bad_alloc&) {
        CloseHandle(handle);
        out_detail = "source allocation failed";
        return false;
    }

    DWORD total = 0;
    const DWORD want = static_cast<DWORD>(out_data.size());
    constexpr DWORD kChunkSize = 64 * 1024;
    while (total < want) {
        if (check_cancel()) {
            CloseHandle(handle);
            out_detail = "cancelled";
            return false;
        }
        const DWORD to_read = (want - total > kChunkSize) ? kChunkSize : (want - total);
        DWORD read_now = 0;
        if (!ReadFile(handle, out_data.data() + total, to_read, &read_now, nullptr) ||
            read_now == 0) {
            CloseHandle(handle);
            out_detail = "source file read failed";
            return false;
        }
        total += read_now;
    }
    CloseHandle(handle);
    return true;
}

}  // namespace

void save_controller::worker_loop() {
    while (true) {
        save_work_request req;
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

        save_completion_msg comp;
        comp.request_id = req.request_id;
        comp.document_instance_id = req.document_instance_id;
        comp.diagnostic_revision = req.diagnostic_revision;
        comp.destination = req.request.destination;

        if (cancel_check()) {
            comp.error = oz::viewer_core::save_error::cancelled;
            comp.detail = "cancelled before the save started";
        } else {
            std::shared_ptr<const edit_source_buffer> source = req.existing_source;

            // Optional source-only full-resolution acquisition. This is an
            // immutable decode of the original file, never a materialized edit
            // result: it may be promoted as the renderer base.
            if (req.needs_source_acquisition) {
                std::vector<std::uint8_t> encoded;
                std::string read_error;
                std::uint64_t stable_size = 0;
                std::uint64_t stable_write = 0;
                if (!read_file_stable_identity(req.source_path, encoded, req.expected_file_size,
                                               req.expected_last_write_time, stable_size,
                                               stable_write, read_error, cancel_check)) {
                    if (cancel_check()) {
                        comp.error = oz::viewer_core::save_error::cancelled;
                        comp.detail = "cancelled during source acquisition";
                    } else if (read_error.find("modified") != std::string::npos) {
                        comp.error = oz::viewer_core::save_error::stale_source;
                        comp.detail = read_error;
                    } else {
                        comp.error = oz::viewer_core::save_error::materialization_failed;
                        comp.detail = read_error;
                    }
                } else if (cancel_check()) {
                    comp.error = oz::viewer_core::save_error::cancelled;
                    comp.detail = "cancelled after source acquisition";
                } else {
                    auto decoded = oz::viewer_core::decode_image(encoded.data(), encoded.size(), 1);
                    if (decoded.error != oz::viewer_core::codec_error::none) {
                        comp.error = oz::viewer_core::save_error::materialization_failed;
                        comp.detail = decoded.detail.empty()
                                          ? "full-resolution decode failed"
                                          : decoded.detail;
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
                            comp.error = oz::viewer_core::save_error::stale_source;
                            comp.detail =
                                "decoded source dimensions do not match document metadata";
                        } else {
                            source = std::move(buffer);
                            comp.acquired_source = source;
                        }
                    }
                }
            }

            if (comp.error == oz::viewer_core::save_error::none) {
                if (!source || !source->is_valid()) {
                    comp.error = oz::viewer_core::save_error::invalid_request;
                    comp.detail = "no valid immutable source available for saving";
                } else {
                    // The request borrows the source pixels and the copied
                    // command sequence for the duration of execute_save only.
                    req.request.source = source->make_view();
                    req.request.commands =
                        std::span<const oz::viewer_core::edit_command>(req.commands.data(),
                                                                       req.commands.size());
                    if (req.request.memory_budget == 0) {
                        req.request.memory_budget = oz::viewer_core::get_system_edit_admission_budget();
                    }

                    const oz::viewer_core::save_result result =
                        oz::viewer_core::execute_save(req.request, cancel_check);
                    comp.error = result.error;
                    comp.detail = result.detail;
                    comp.committed = result.committed;
                    comp.committed_fingerprint = result.committed_fingerprint;
                }
            }
        }

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

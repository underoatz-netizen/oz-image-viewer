// ozviewer - Stages 1-4 minimal image viewer with async sibling navigation,
// adjacent +-2 preload, bounded adaptive CPU cache, view interaction (zoom/pan/fit/100%/fullscreen),
// and minimal open usability (zero-arg launch, Open button/menu, Ctrl+O, drag & drop).
// Stage 5 editing is gated by OZVIEWER_ENABLE_EDITING: non-destructive
// rotate/flip/crop/resize with a saved-active-command checkpoint, plus a
// conservative single-flight File -> Save / Save As that writes only PNG and
// JPEG through in-box WIC encoders (see save_pipeline.hpp for the contract).
// Supported still images: JPEG (.jpg/.jpeg, TurboJPEG), PNG (.png, Windows WIC),
// static WebP (.webp, libwebp decoder+demux; no animation) when enabled,
// static AVIF (.avif) when enabled, static HEIC/HEIF (.heic/.heif) when enabled/loaded,
// and Common Raster Decode Profile v1 Phase A/B: BMP (.bmp), static GIF
// (.gif, still images only), classic TIFF page 0 (.tif/.tiff, RAW-gated),
// ICO (.ico, deterministic largest-safe entry), CUR (.cur, deterministic
// cursor entry; hotspot ignored for presentation) and TGA (.tga, native
// bounded uncompressed/RLE/color-map/grayscale adapter). Decode routing is
// signature-first; file extensions never select a codec.
//
// Usage:
//   ozviewer.exe                    open interactive window with centered Open button
//   ozviewer.exe <image>            open and display; window stays open
//   ozviewer.exe --benchmark <image> identical path; emits Stage 0 timing
//                                   markers as JSONL on stdout and exits
//                                   automatically after a successful FirstPresent
//   ozviewer.exe --navigation-benchmark --corpus <dir> --out <file.jsonl> [--overwrite] ...
//                                   test-only navigation benchmark mode
//
// Threading:
// - UI thread owns the HWND, D3D immediate context, swap chain, D2D context,
//   retained source bitmap, ViewState, presentation, resizing, CPU decoded-image cache,
//   navigation list state, document state, and completion dispatch.
// - Fast-first initial decode uses a single startup worker thread.
// - Sibling enumeration occurs in a separate background worker launched strictly
//   after the first successful Present of a document.
// - In interactive mode, once enumeration completes, a single persistent decode
//   worker processes navigation and preload requests using a replaceable latest-plan
//   queue. Results are communicated via a mutex-protected bounded completion queue
//   and a WM_APP wakeup message. Stale or duplicate messages are safely validated
//   against current UI epoch/generation/path/profile.
#include "oz/viewer_core/cache.hpp"
#include "oz/viewer_core/codec.hpp"
#include "oz/viewer_core/document_state.hpp"
#include "oz/viewer_core/fit.hpp"
#include "oz/viewer_core/marker_emit.hpp"
#include "oz/viewer_core/navigation.hpp"
#include "oz/viewer_core/navigation_benchmark.hpp"
#include "oz/viewer_core/view_state.hpp"
#include "oz/viewer_core/work_queue.hpp"
#include "renderer.hpp"
#include "modern_dialogs.hpp"
#include "crop_strip.hpp"
#include "context_menu.hpp"
#include "clipboard_output.hpp"
#include "clipboard_input.hpp"
#include "paste_install.hpp"
#include "copy_controller.hpp"
#include "shortcut_routing.hpp"
#include "shell_integration.hpp"
#include "ui_chrome.hpp"
#include "ui_command_state.hpp"
#include "ui_layout.hpp"
#include "ui_theme.hpp"
#include "ozviewer_resources.h"

#if OZVIEWER_ENABLE_EDITING
#include "oz/viewer_core/crop_interaction.hpp"
#include "oz/viewer_core/image_document.hpp"
#include "oz/viewer_core/save_pipeline.hpp"
#include "presentation_edits.hpp"
#include "edit_controller.hpp"
#include "resize_dialog.hpp"
#include "save_controller.hpp"

namespace {
bool verify_source_file_metadata(const std::filesystem::path& path,
                                std::uint64_t expected_size,
                                std::uint64_t expected_last_write,
                                std::uint64_t* out_current_size = nullptr,
                                std::uint64_t* out_current_last_write = nullptr) noexcept {
    return oz::viewer::verify_source_file_metadata(path, expected_size, expected_last_write,
                                                  out_current_size, out_current_last_write);
}
}
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>  // GET_X_LPARAM, GET_Y_LPARAM
#include <commdlg.h>
#include <shellapi.h>  // CommandLineToArgvW, DragAcceptFiles, DragQueryFileW, DragFinish

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr UINT kMsgDecodeComplete = WM_APP + 1;
constexpr UINT kMsgEnumComplete   = WM_APP + 2;
#if OZVIEWER_ENABLE_EDITING
constexpr UINT kMsgEditComplete   = WM_APP + 3;
constexpr UINT kMsgSaveComplete   = WM_APP + 4;
#endif
constexpr UINT kMsgCopyComplete   = WM_APP + 5;
constexpr UINT_PTR kNavBenchTimerId = 1001;

constexpr int kIdFileOpen = 101;
constexpr int kIdBtnOpen  = 102;
constexpr int kIdFileSave = 103;
constexpr int kIdFileSaveAs = 104;
constexpr int kIdFileOpenLocation = 105;
constexpr int kIdFileCopy = 106;
constexpr int kIdFileCopyPath = 107;
constexpr int kIdFileCopyImage = 108;

#if OZVIEWER_ENABLE_EDITING
constexpr int kIdEditRotateLeft  = 201;
constexpr int kIdEditRotateRight = 202;
constexpr int kIdEditFlipH       = 203;
constexpr int kIdEditFlipV       = 204;
constexpr int kIdEditUndo        = 205;
constexpr int kIdEditRedo        = 206;
constexpr int kIdEditCrop        = 207;
constexpr int kIdEditCropApply   = 208;
constexpr int kIdEditCropCancel  = 209;
constexpr int kIdEditCropReset   = 210;
constexpr int kIdEditResize      = 211;
constexpr int kIdCropRatioFree     = 221;
constexpr int kIdCropRatioOriginal = 222;
constexpr int kIdCropRatio1x1      = 223;
constexpr int kIdCropRatio3x2      = 224;
constexpr int kIdCropRatio4x3      = 225;
constexpr int kIdCropRatio5x4      = 226;
constexpr int kIdCropRatio16x9     = 227;
constexpr int kIdCropRatio9x16     = 228;
constexpr int kIdCropRatioASeries  = 229;
#endif

// Forward declaration
struct app_state;
static void schedule_navigation_plan(app_state& app);
static void start_open_file_dialog(app_state& app);
static void open_new_document(app_state& app, const std::filesystem::path& path);
static bool open_pasted_document(app_state& app, oz::viewer::clipboard_bitmap&& bitmap);
static void start_paste_from_clipboard(app_state& app);
static void update_open_button_layout(app_state& app);
static oz::viewer::ui_layout_config current_ui_layout_config(const app_state& app) noexcept;
static oz::viewer::ui_layout_result current_ui_layout(const app_state& app) noexcept;
static oz::viewer::presentation_plan compute_active_presentation_plan(app_state& app, int view_w, int view_h);
static bool upload_and_present(app_state& app, std::uint64_t* out_present_ticks = nullptr);
static void redraw_current_view(app_state& app);
static bool check_dirty_discard_allowed(app_state& app);
static void update_menu_command_state(app_state& app);
#if OZVIEWER_ENABLE_EDITING
static const char* get_codec_name_str(oz::viewer_core::codec_id id) noexcept {
    switch (id) {
        case oz::viewer_core::codec_id::jpeg: return "jpeg";
        case oz::viewer_core::codec_id::png_wic: return "png";
        case oz::viewer_core::codec_id::webp: return "webp";
        case oz::viewer_core::codec_id::bmp_wic: return "bmp";
        case oz::viewer_core::codec_id::gif_wic: return "gif";
        case oz::viewer_core::codec_id::tiff_wic: return "tiff";
        case oz::viewer_core::codec_id::ico_wic: return "ico";
        case oz::viewer_core::codec_id::cur_wic: return "cur";
        case oz::viewer_core::codec_id::tga: return "tga";
        case oz::viewer_core::codec_id::avif: return "avif";
        case oz::viewer_core::codec_id::heic: return "heic";
        default: return "unknown";
    }
}
static void update_edit_menu_state(app_state& app);
static bool apply_edit_command(app_state& app, const oz::viewer_core::edit_command& cmd);
static void apply_undo(app_state& app);
static void apply_redo(app_state& app);
static void begin_crop_mode(app_state& app);
static void cancel_crop_mode(app_state& app);
static void apply_crop_mode(app_state& app);
static void reset_crop_mode(app_state& app);
static void set_crop_ratio(app_state& app, oz::viewer_core::crop_ratio_preset preset);
static bool submit_resize_validation(app_state& app, HWND dialog,
                                     const oz::viewer_core::edit_dimensions& target);
static bool edit_transition_blocked(app_state& app);
static void start_save(app_state& app, bool save_as);
static bool resolve_active_save_for_action(app_state& app);
#endif

// Initial decode outcome structure for the fast-first startup path.
// Carries the actual decode profile so the cache key describes the pixels.
struct initial_decode_outcome {
    bool ok = false;
    std::uint64_t generation = 0;
    std::shared_ptr<oz::viewer_core::decoded_image_data> image;
    std::filesystem::path path;
    std::string detail;
    int scale_den = 1;
    std::int32_t src_width = 0;
    std::int32_t src_height = 0;
    std::uint64_t source_file_size = 0;
    std::uint64_t source_last_write_time = 0;
};

// Application state owned by the UI thread; background workers only touch
// mutex-protected queue/result slots and atomic cancellation flags.
struct app_state {
    HWND hwnd = nullptr;
    HWND btn_open = nullptr;
    oz::viewer_core::marker_emitter* markers = nullptr;
    bool benchmark = false;
    bool nav_benchmark = false;
    oz::viewer_core::navigation_benchmark_session nav_bench_session;
    std::uint64_t current_nav_bench_req_id = 0;
    // Most recent await_cache target from the scenario state machine; its
    // cache residency is what the session verifies before a measured step.
    std::size_t nav_bench_await_index = 0;
    bool nav_bench_await_valid = false;
    // Latch so a re-arm plan is scheduled once while the session re-emits the
    // same setup action until the target index is committed.
    std::size_t nav_bench_rearm_index = 0;
    bool nav_bench_rearm_started = false;
    std::filesystem::path initial_path;

    // Stage 4 UI Document state
    oz::viewer_core::document_state doc;
    HMENU hMenuFile = nullptr;

#if OZVIEWER_ENABLE_EDITING
    // Stage 5 Slice 3: Domain image document state & edit controller
    // Lazily created in interactive mode only so benchmark mode allocates zero edit structures
    std::unique_ptr<oz::viewer_core::image_document> image_doc;
    std::unique_ptr<oz::viewer::edit_controller> edit_ctrl;
    std::shared_ptr<const oz::viewer::edit_source_buffer> full_res_source;
    bool edit_ctrl_started = false;
    // True only while a decoded candidate for a *different* document is being
    // transactionally presented. The previous document's command log must not
    // transform that candidate before its own image_document commits.
    bool suppress_edit_presentation = false;
    HMENU hMenuEdit = nullptr;
    HMENU hMenuCropRatio = nullptr;
    std::optional<oz::viewer_core::crop_interaction_state> crop;
    std::optional<oz::viewer_core::edit_dimensions> draft_resize;

    // Stage 5 Slice 5: exactly one in-flight Resize validation request. Apply
    // captures the document instance, base edit revision, source fingerprint,
    // target and proposed command sequence; the completion is accepted only
    // when every one of those still matches.
    struct pending_resize_validation {
        std::uint64_t request_id = 0;
        std::uint64_t document_instance_id = 0;
        std::uint64_t base_revision = 0;
        oz::viewer_core::edit_dimensions target{};
        bool source_was_acquired = false;
        // Snapshot of the source identity the request was submitted against.
        std::filesystem::path source_path{};
        std::uint64_t source_file_size = 0;
        std::uint64_t source_last_write_time = 0;
        oz::viewer_core::edit_dimensions source_dimensions{};
        bool source_identity_expected = false;
    };
    std::optional<pending_resize_validation> pending_resize;
    // Modal dialog HWND while the Resize dialog owns the UI thread, else null.
    HWND resize_dlg = nullptr;

    // Stage 5 Slice 6: single-flight Save/Save As worker and its captured
    // request identity. Save and Save As are disabled while a save is active,
    // and document-switching actions wait for (or cooperatively cancel) the
    // worker before they proceed, so a late write can never surprise a
    // different document.
    std::unique_ptr<oz::viewer::save_controller> save_ctrl;
    bool save_ctrl_started = false;
    struct pending_save_state {
        std::uint64_t request_id = 0;
        std::uint64_t document_instance_id = 0;
        // Exact active-command snapshot captured at submit time; this becomes
        // the saved checkpoint only on a matching successful same-source save.
        std::vector<oz::viewer_core::edit_command> captured_commands;
        std::filesystem::path destination;
        bool same_source = false;
        oz::viewer_core::save_format format = oz::viewer_core::save_format::png;
    };
    std::optional<pending_save_state> pending_save;
#endif

    // Stage 9.4: Lazy, latest-only Copy Image worker & controller
    std::unique_ptr<oz::viewer::copy_controller> copy_ctrl;
    bool copy_ctrl_started = false;
    struct pending_copy_state {
        std::uint64_t request_id = 0;
        std::uint64_t document_instance_id = 0;
        std::uint64_t diagnostic_revision = 0;
        std::uint64_t document_generation = 0;
    };
    std::optional<pending_copy_state> pending_copy;

    // Stage 4 ViewState and interaction tracking
    oz::viewer_core::view_state view;
    bool is_panning = false;
    POINT last_mouse_pos{0, 0};

    // Stage 4 Fullscreen state tracking
    bool is_fullscreen = false;
    WINDOWPLACEMENT saved_placement{sizeof(WINDOWPLACEMENT)};
    LONG saved_style = 0;
    LONG saved_exstyle = 0;

    // Fast-first initial decode state
    std::mutex initial_decode_mutex;
    std::shared_ptr<initial_decode_outcome> initial_result;
    std::atomic<bool> cancel_initial_decode{false};
    std::jthread initial_decode_thread;

    // Stage 2.1 Enumeration worker state
    std::mutex enum_mutex;
    std::uint64_t enum_generation = 0;
    std::unique_ptr<oz::viewer_core::navigation_list> enum_result;
    std::atomic<bool> cancel_enum{false};
    std::jthread enum_thread;
    bool enum_started = false;
    oz::viewer_core::navigation_list nav;

    // Stage 2.2 Navigation and persistent decode worker state
    std::size_t desired_index = 0;
    std::size_t committed_index = 0;
    std::uint64_t current_epoch = 1;
    bool persistent_worker_started = false;
    oz::viewer_core::decode_work_queue work_queue;
    oz::viewer_core::completion_queue comp_queue;
    std::jthread persistent_worker_thread;

    // Stage 2.2 CPU cache (UI-thread owned)
    oz::viewer_core::cpu_image_cache cache;

    oz::viewer::renderer renderer;
    bool have_image = false;
    std::shared_ptr<oz::viewer_core::decoded_image_data> current;
    bool first_present_done = false;
    bool device_recovered = false;

    // Stage 7 Modern Chrome manager (interactive non-benchmark mode only)
    std::unique_ptr<oz::viewer::ui_chrome> chrome;
    std::unique_ptr<oz::viewer::crop_strip> crop_strip_ui;
    HFONT font_open_btn = nullptr;
};

// True in both automated benchmark modes (--benchmark and
// --navigation-benchmark): no interactive UI may be created or shown, including
// on decode/present failure paths. Single shared predicate so construction and
// failure branches cannot drift apart.
static bool noninteractive_mode(const app_state& app) noexcept {
    return app.benchmark || app.nav_benchmark;
}

// The logical canvas may differ from the decoded bitmap dimensions while an
// edit is previewed (for example a 90-degree rotation or a crop on a scaled
// fast-first decode). ViewState always operates in this logical canvas space.
struct active_canvas_dimensions {
    std::int32_t width = 0;
    std::int32_t height = 0;

    [[nodiscard]] bool is_valid() const noexcept {
        return width > 0 && height > 0;
    }
};

static active_canvas_dimensions active_view_dimensions(const app_state& app) noexcept {
#if OZVIEWER_ENABLE_EDITING
    if (app.draft_resize.has_value() && app.draft_resize->is_valid()) {
        return {app.draft_resize->width, app.draft_resize->height};
    }
    if (app.image_doc) {
        if (const auto dims = app.image_doc->current_dimensions(); dims.has_value() && dims->is_valid()) {
            return {dims->width, dims->height};
        }
    }
#endif
    if (app.current && app.current->src_width > 0 && app.current->src_height > 0) {
        return {app.current->src_width, app.current->src_height};
    }
    return {};
}

#if OZVIEWER_ENABLE_EDITING
// Convert a logical source rectangle into the retained bitmap's physical pixel
// coordinates. Fast-first images can be decoder-scaled; floor/ceil preserves
// coverage until the asynchronous full-resolution source upgrade arrives.
static oz::viewer_core::edit_rect logical_rect_to_bitmap_rect(
    const oz::viewer_core::edit_rect& logical_rect,
    const oz::viewer_core::edit_dimensions& logical_dims,
    std::int32_t bitmap_width, std::int32_t bitmap_height) noexcept {
    if (!logical_rect.is_valid() || !logical_dims.is_valid() || bitmap_width <= 0 || bitmap_height <= 0) {
        return {};
    }
    if (logical_rect.x == 0 && logical_rect.y == 0 &&
        logical_rect.width == logical_dims.width && logical_rect.height == logical_dims.height) {
        return {};
    }

    const auto scale_floor = [](std::int32_t value, std::int32_t from,
                                std::int32_t to) noexcept {
        return static_cast<std::int32_t>((static_cast<std::int64_t>(value) * to) / from);
    };
    const auto scale_ceil = [](std::int32_t value, std::int32_t from,
                               std::int32_t to) noexcept {
        const std::int64_t numerator = static_cast<std::int64_t>(value) * to;
        return static_cast<std::int32_t>((numerator + from - 1) / from);
    };

    const std::int32_t left = std::clamp(scale_floor(logical_rect.x, logical_dims.width, bitmap_width),
                                         0, bitmap_width - 1);
    const std::int32_t top = std::clamp(scale_floor(logical_rect.y, logical_dims.height, bitmap_height),
                                        0, bitmap_height - 1);
    const std::int32_t right = std::clamp(scale_ceil(logical_rect.right(), logical_dims.width, bitmap_width),
                                          left + 1, bitmap_width);
    const std::int32_t bottom = std::clamp(scale_ceil(logical_rect.bottom(), logical_dims.height, bitmap_height),
                                           top + 1, bitmap_height);
    return {left, top, right - left, bottom - top};
}

static void update_crop_window_title(app_state& app) {
    if (!app.hwnd || app.benchmark || app.nav_benchmark) {
        return;
    }
    const bool active = app.crop.has_value() && app.crop->active;
    SetWindowTextW(app.hwnd, active
        ? L"OZ Image Viewer — Crop (Enter: Apply, Esc: Cancel)"
        : L"OZ Image Viewer");
}

static std::optional<oz::viewer::crop_overlay> current_crop_overlay(
    app_state& app, const oz::viewer::presentation_plan& plan) {
    if (!app.crop.has_value() || !app.crop->active || !app.crop->selection.is_valid() ||
        plan.geometry.scale <= 0.0) {
        return std::nullopt;
    }

    const auto image_rect = oz::viewer_core::view_geometry_to_dip(plan.geometry, app.renderer.dpi);
    const float dip_scale = app.renderer.dpi > 0.0f ? 96.0f / app.renderer.dpi : 1.0f;
    const auto& selection = app.crop->selection;
    oz::viewer::crop_overlay overlay;
    overlay.visible = true;
    overlay.image_rect = image_rect;
    overlay.selection_rect.x = static_cast<float>(plan.geometry.dst_x +
                                                  static_cast<double>(selection.x) * plan.geometry.scale) * dip_scale;
    overlay.selection_rect.y = static_cast<float>(plan.geometry.dst_y +
                                                  static_cast<double>(selection.y) * plan.geometry.scale) * dip_scale;
    overlay.selection_rect.width = static_cast<float>(static_cast<double>(selection.width) * plan.geometry.scale) * dip_scale;
    overlay.selection_rect.height = static_cast<float>(static_cast<double>(selection.height) * plan.geometry.scale) * dip_scale;
    overlay.handle_radius_dip = 4.0f;
    return overlay;
}

static bool crop_canvas_point_for_client(app_state& app, int client_x, int client_y,
                                         double& out_x, double& out_y,
                                         double& out_handle_radius) {
    if (!app.crop.has_value() || !app.crop->active) {
        return false;
    }
    const auto layout = current_ui_layout(app);
    const int view_w = layout.canvas_physical.width;
    const int view_h = layout.canvas_physical.height;
    const auto plan = compute_active_presentation_plan(app, view_w, view_h);
    if (plan.geometry.scale <= 0.0) {
        return false;
    }
    const double pt_in_canvas_x = static_cast<double>(client_x - layout.canvas_physical.x);
    const double pt_in_canvas_y = static_cast<double>(client_y - layout.canvas_physical.y);
    out_x = std::clamp((pt_in_canvas_x - plan.geometry.dst_x) / plan.geometry.scale,
                       0.0, static_cast<double>(app.crop->canvas.width));
    out_y = std::clamp((pt_in_canvas_y - plan.geometry.dst_y) / plan.geometry.scale,
                       0.0, static_cast<double>(app.crop->canvas.height));
    // Eight physical pixels is easy to grab at 100% while still scaling with
    // zoom, with a lower bound so fit previews remain usable.
    out_handle_radius = (std::max)(1.0, 8.0 / plan.geometry.scale);
    return true;
}
#endif

// Helper to query available physical memory and refresh cache budget
void refresh_cache_budget(app_state& app) {
    MEMORYSTATUSEX mem_status{};
    mem_status.dwLength = sizeof(mem_status);
    if (GlobalMemoryStatusEx(&mem_status)) {
        const std::size_t budget = oz::viewer_core::compute_cpu_cache_budget(mem_status.ullAvailPhys);
        app.cache.set_budget(budget);
    } else {
        // Fallback default 128 MiB
        app.cache.set_budget(128ull * 1024ull * 1024ull);
    }
}

// Read file contents with chunked cooperative cancellation support, capturing
// and verifying file size and last write time across the read boundary.
bool read_file_chunked_stable(const std::filesystem::path& path,
                              std::vector<std::uint8_t>& out_data,
                              std::string& err_detail,
                              std::uint64_t& out_size,
                              std::uint64_t& out_last_write,
                              const std::function<bool()>& check_cancel) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err_detail = "file open failed";
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > oz::viewer_core::kMaxEncodedBytes) {
        CloseHandle(h);
        err_detail = "file too large";
        return false;
    }

    FILETIME ft_before{};
    if (!GetFileTime(h, nullptr, nullptr, &ft_before)) {
        CloseHandle(h);
        err_detail = "file metadata query failed";
        return false;
    }

    out_data.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD total = 0;
    const DWORD want = static_cast<DWORD>(out_data.size());
    constexpr DWORD kChunkSize = 64 * 1024;  // 64 KiB chunks for cooperative check

    while (total < want) {
        if (check_cancel()) {
            CloseHandle(h);
            err_detail = "cancelled";
            return false;
        }
        DWORD to_read = (want - total > kChunkSize) ? kChunkSize : (want - total);
        DWORD n = 0;
        if (!ReadFile(h, out_data.data() + total, to_read, &n, nullptr) || n == 0) {
            CloseHandle(h);
            err_detail = "file read failed";
            return false;
        }
        total += n;
    }

    LARGE_INTEGER size_after{};
    FILETIME ft_after{};
    if (!GetFileSizeEx(h, &size_after) || !GetFileTime(h, nullptr, nullptr, &ft_after) ||
        size_after.QuadPart != size.QuadPart ||
        ft_after.dwLowDateTime != ft_before.dwLowDateTime ||
        ft_after.dwHighDateTime != ft_before.dwHighDateTime) {
        CloseHandle(h);
        err_detail = "file modified during read";
        return false;
    }

    CloseHandle(h);

    ULARGE_INTEGER ft_val{};
    ft_val.LowPart = ft_before.dwLowDateTime;
    ft_val.HighPart = ft_before.dwHighDateTime;
    out_size = static_cast<std::uint64_t>(size.QuadPart);
    out_last_write = ft_val.QuadPart;
    return true;
}

// Read file contents with chunked cooperative cancellation support
bool read_file_chunked(const std::filesystem::path& path,
                       std::vector<std::uint8_t>& out_data,
                       std::string& err_detail,
                       const std::function<bool()>& check_cancel) {
    std::uint64_t sz = 0;
    std::uint64_t ft = 0;
    return read_file_chunked_stable(path, out_data, err_detail, sz, ft, check_cancel);
}

// Shared registry-based decode pipeline (Stage 3.2): signature-first header
// parse, viewport scale selection on orientation-adjusted source dims, then
// canonical decode.
std::shared_ptr<oz::viewer_core::decoded_image_data> wrap_registry_result(
    oz::viewer_core::decode_result& reg) {
    auto img = std::make_shared<oz::viewer_core::decoded_image_data>();
    img->pixels = std::move(reg.image.pixels);
    img->width = reg.image.width;
    img->height = reg.image.height;
    img->stride = reg.image.stride;
    img->scale_den = reg.image.scale_den;
    img->src_width = reg.image.src_width;
    img->src_height = reg.image.src_height;
    img->codec = reg.image.codec;
    return img;
}

// Shared key -> navigation intent decoding (single policy model).
oz::viewer_core::bench_nav_direction bench_direction_for_key(WPARAM vk_key) noexcept {
    switch (vk_key) {
        case VK_LEFT:
        case VK_BACK:
            return oz::viewer_core::bench_nav_direction::backward;
        case VK_RIGHT:
        case VK_SPACE:
            return oz::viewer_core::bench_nav_direction::forward;
        case VK_HOME:
            return oz::viewer_core::bench_nav_direction::home;
        case VK_END:
            return oz::viewer_core::bench_nav_direction::end;
        default:
            return oz::viewer_core::bench_nav_direction::none;
    }
}

// Shared UI navigation request implementation.
bool execute_navigation_request(app_state& app, WPARAM vk_key, const std::string& scenario_label = "interactive") {
    if (app.nav.empty()) {
        return false;
    }
#if OZVIEWER_ENABLE_EDITING
    // A crop draft is deliberately modal: navigation must not silently abandon
    // the user's uncommitted selection.
    if (app.crop.has_value() && app.crop->active) {
        return false;
    }
#endif
    const oz::viewer_core::bench_nav_direction dir = bench_direction_for_key(vk_key);
    if (dir == oz::viewer_core::bench_nav_direction::none) {
        return false;
    }
    const auto expected = oz::viewer_core::expected_target_index(dir, app.desired_index, app.nav.size());
    if (!expected.has_value()) {
        return false;
    }
    const std::size_t new_desired = *expected;
    if (new_desired == app.desired_index) {
        return false;
    }

    if (!check_dirty_discard_allowed(app)) {
        return false;
    }
    const std::string direction(oz::viewer_core::to_string(dir));

    const std::size_t from_index = app.desired_index;
    const std::filesystem::path* from_path = app.nav.at(from_index);
    const std::string from_alias = from_path ? from_path->filename().string() : "";

    const std::filesystem::path* target_path = app.nav.at(new_desired);
    const std::string target_alias = target_path ? target_path->filename().string() : "";

    const auto layout = current_ui_layout(app);
    const int view_w = layout.canvas_physical.width;
    const int view_h = layout.canvas_physical.height;
    const std::wstring target_norm = target_path ? oz::viewer_core::normalize_path_key(*target_path) : L"";

    const bool is_hit = (app.cache.find_for_viewport(target_norm, view_w, view_h) != nullptr);
    const auto cache_state = is_hit ? oz::viewer_core::bench_cache_state::hit_at_request
                                    : oz::viewer_core::bench_cache_state::miss_at_request;

    if (app.nav_benchmark && app.current_nav_bench_req_id != 0) {
        app.nav_bench_session.record_request_superseded(
            app.current_nav_bench_req_id, app.current_epoch, "superseded_by_newer_navigation");
        app.current_nav_bench_req_id = 0;
    }

    app.desired_index = new_desired;
    ++app.current_epoch;

    if (app.nav_benchmark) {
        app.current_nav_bench_req_id = app.nav_bench_session.record_navigation_request(
            scenario_label,
            direction,
            new_desired,
            target_alias,
            from_index,
            from_alias,
            cache_state,
            app.current_epoch);
    }

    schedule_navigation_plan(app);
    return true;
}

// --- Initial fast-first decode worker ---------------------------------------

void initial_decode_worker_func(app_state& app, std::filesystem::path path,
                                std::int32_t view_w, std::int32_t view_h,
                                std::uint64_t launch_gen) {
    auto outcome = std::make_shared<initial_decode_outcome>();
    outcome->path = path;
    outcome->generation = launch_gen;

    std::vector<std::uint8_t> data;
    std::uint64_t stable_size = 0;
    std::uint64_t stable_last_write = 0;
    if (!read_file_chunked_stable(path, data, outcome->detail, stable_size, stable_last_write, [&app] {
        return app.cancel_initial_decode.load(std::memory_order_relaxed);
    })) {
        std::lock_guard<std::mutex> lock(app.initial_decode_mutex);
        app.initial_result = std::move(outcome);
        PostMessageW(app.hwnd, kMsgDecodeComplete, 0, 0);
        return;
    }
    outcome->source_file_size = stable_size;
    outcome->source_last_write_time = stable_last_write;

    if (app.markers) {
        app.markers->record(oz::markers::marker_id::file_opened);
    }

    if (app.cancel_initial_decode.load(std::memory_order_relaxed)) {
        return;
    }

    if (!oz::viewer_core::detect_codec(data.data(), data.size()).has_value()) {
        outcome->detail = "unsupported image format";
        std::lock_guard<std::mutex> lock(app.initial_decode_mutex);
        app.initial_result = std::move(outcome);
        PostMessageW(app.hwnd, kMsgDecodeComplete, 0, 0);
        return;
    }
    if (app.markers) {
        app.markers->record(oz::markers::marker_id::codec_detected);
    }

    oz::viewer_core::image_info info;
    const auto herr = oz::viewer_core::parse_image_header(data.data(), data.size(), info);
    if (herr != oz::viewer_core::codec_error::none) {
        outcome->detail = "header parse failed";
        std::lock_guard<std::mutex> lock(app.initial_decode_mutex);
        app.initial_result = std::move(outcome);
        PostMessageW(app.hwnd, kMsgDecodeComplete, 0, 0);
        return;
    }
    if (app.markers) {
        app.markers->record(oz::markers::marker_id::header_parsed);
    }

    const int den = oz::viewer_core::choose_scaling_denominator(info.src_width, info.src_height,
                                                                view_w, view_h);
    if (app.markers) {
        app.markers->record(oz::markers::marker_id::decode_start);
    }
    auto reg = oz::viewer_core::decode_image(data.data(), data.size(), den);
    if (app.markers) {
        app.markers->record(oz::markers::marker_id::decode_complete);
    }

    if (reg.error != oz::viewer_core::codec_error::none) {
        outcome->detail = reg.detail.empty() ? "decode failed" : reg.detail;
        std::lock_guard<std::mutex> lock(app.initial_decode_mutex);
        app.initial_result = std::move(outcome);
        PostMessageW(app.hwnd, kMsgDecodeComplete, 0, 0);
        return;
    }

    auto img = wrap_registry_result(reg);
    img->source_file_size = stable_size;
    img->source_last_write_time = stable_last_write;
    outcome->ok = true;
    outcome->image = std::move(img);
    outcome->scale_den = reg.image.scale_den;
    outcome->src_width = reg.image.src_width;
    outcome->src_height = reg.image.src_height;

    std::lock_guard<std::mutex> lock(app.initial_decode_mutex);
    app.initial_result = std::move(outcome);
    PostMessageW(app.hwnd, kMsgDecodeComplete, 0, 0);
}

// --- Persistent decode worker for Stage 2.2 ---------------------------------

void persistent_worker_func(app_state& app) {
    while (true) {
        std::uint64_t pop_epoch = 0;
        auto opt_req = app.work_queue.pop_work(pop_epoch);
        if (!opt_req.has_value()) {
            break;
        }

        oz::viewer_core::decode_request req = std::move(*opt_req);

        if (req.epoch < app.work_queue.get_epoch() && req.priority != oz::viewer_core::work_priority::p0_selected) {
            continue;
        }

        std::vector<std::uint8_t> data;
        std::string err_detail;
        std::uint64_t stable_size = 0;
        std::uint64_t stable_last_write = 0;
        const bool read_ok = read_file_chunked_stable(req.path, data, err_detail, stable_size, stable_last_write, [&app, req_epoch = req.epoch, prio = req.priority] {
            if (app.work_queue.is_stopped()) return true;
            if (prio != oz::viewer_core::work_priority::p0_selected && req_epoch < app.work_queue.get_epoch()) {
                return true;
            }
            return false;
        });

        if (!read_ok) {
            if (err_detail == "cancelled") {
                continue;
            }
            oz::viewer_core::decode_result_msg msg;
            msg.epoch = req.epoch;
            msg.priority = req.priority;
            msg.key = req.key;
            msg.path = req.path;
            msg.ok = false;
            msg.detail = err_detail;
            app.comp_queue.push(std::move(msg));
            PostMessageW(app.hwnd, kMsgDecodeComplete, 0, 0);
            continue;
        }

        if (req.priority != oz::viewer_core::work_priority::p0_selected && req.epoch < app.work_queue.get_epoch()) {
            continue;
        }

        if (!oz::viewer_core::detect_codec(data.data(), data.size()).has_value()) {
            oz::viewer_core::decode_result_msg msg;
            msg.epoch = req.epoch;
            msg.priority = req.priority;
            msg.key = req.key;
            msg.path = req.path;
            msg.ok = false;
            msg.detail = "unsupported image format";
            app.comp_queue.push(std::move(msg));
            PostMessageW(app.hwnd, kMsgDecodeComplete, 0, 0);
            continue;
        }

        oz::viewer_core::image_info info;
        const auto herr = oz::viewer_core::parse_image_header(data.data(), data.size(), info);
        if (herr != oz::viewer_core::codec_error::none) {
            oz::viewer_core::decode_result_msg msg;
            msg.epoch = req.epoch;
            msg.priority = req.priority;
            msg.key = req.key;
            msg.path = req.path;
            msg.ok = false;
            msg.detail = "header parse failed";
            app.comp_queue.push(std::move(msg));
            PostMessageW(app.hwnd, kMsgDecodeComplete, 0, 0);
            continue;
        }

        if (req.priority != oz::viewer_core::work_priority::p0_selected && req.epoch < app.work_queue.get_epoch()) {
            continue;
        }

        const int actual_den = oz::viewer_core::choose_scaling_denominator(
            info.src_width, info.src_height, req.view_w, req.view_h);
        auto reg = oz::viewer_core::decode_image(data.data(), data.size(), actual_den);

        if (reg.error != oz::viewer_core::codec_error::none) {
            oz::viewer_core::decode_result_msg msg;
            msg.epoch = req.epoch;
            msg.priority = req.priority;
            msg.key = req.key;
            msg.path = req.path;
            msg.ok = false;
            msg.detail = reg.detail.empty() ? "decode failed" : reg.detail;
            app.comp_queue.push(std::move(msg));
            PostMessageW(app.hwnd, kMsgDecodeComplete, 0, 0);
            continue;
        }

        auto img = wrap_registry_result(reg);
        const std::int32_t src_w = reg.image.src_width;
        const std::int32_t src_h = reg.image.src_height;

        img->source_file_size = stable_size;
        img->source_last_write_time = stable_last_write;

        oz::viewer_core::decode_result_msg msg;
        msg.epoch = req.epoch;
        msg.priority = req.priority;
        msg.key = req.key;
        msg.key.scale_den = actual_den;
        msg.path = req.path;
        msg.ok = true;
        msg.image = std::move(img);
        msg.src_width = src_w;
        msg.src_height = src_h;
        msg.actual_scale_den = actual_den;
        msg.source_file_size = stable_size;
        msg.source_last_write_time = stable_last_write;

        app.comp_queue.push(std::move(msg));
        PostMessageW(app.hwnd, kMsgDecodeComplete, 0, 0);
    }
}

// --- Stage 2.1 enumeration worker -------------------------------------------

void enum_worker_func(app_state& app, std::filesystem::path target_path, std::uint64_t launch_gen) {
    auto list = std::make_unique<oz::viewer_core::navigation_list>(
        oz::viewer_core::enumerate_sibling_images(target_path));

    if (app.cancel_enum.load(std::memory_order_relaxed)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(app.enum_mutex);
        app.enum_result = std::move(list);
        app.enum_generation = launch_gen;
    }
    PostMessageW(app.hwnd, kMsgEnumComplete, 0, 0);
}

// --- render + present on the UI thread --------------------------------------
bool upload_and_present(app_state& app, std::uint64_t* out_present_ticks /* = nullptr */) {
    if (!app.current) {
        return false;
    }
    const bool is_first = !app.first_present_done;
    if (is_first && app.markers) {
        app.markers->record(oz::markers::marker_id::gpu_upload_start);
    }

    const auto layout = current_ui_layout(app);
    const int view_w = layout.canvas_physical.width;
    const int view_h = layout.canvas_physical.height;

#if OZVIEWER_ENABLE_EDITING
    if (!noninteractive_mode(app) && !app.suppress_edit_presentation && app.image_doc &&
        app.image_doc->history().total_commands() > 0) {
        const auto plan = compute_active_presentation_plan(app, view_w, view_h);
        const bool ok = oz::viewer::render_and_present_candidate_plan(
            app.renderer, app.current->pixels.data(), app.current->stride,
            app.current->width, app.current->height,
            app.current->src_width, app.current->src_height,
            plan, out_present_ticks);
        if (!ok) return false;
        if (is_first && app.markers) {
            app.markers->record(oz::markers::marker_id::gpu_upload_complete);
            app.markers->record(oz::markers::marker_id::first_present);
            app.first_present_done = true;
        }
        return true;
    }
#endif

    const auto plan = compute_active_presentation_plan(app, view_w, view_h);

    const bool ok = oz::viewer::render_and_present_candidate_plan(
        app.renderer, app.current->pixels.data(), app.current->stride,
        app.current->width, app.current->height,
        app.current->src_width, app.current->src_height,
        plan, out_present_ticks);

    if (!ok) {
        return false;
    }
    if (is_first && app.markers) {
        app.markers->record(oz::markers::marker_id::gpu_upload_complete);
        app.markers->record(oz::markers::marker_id::first_present);
        app.first_present_done = true;
    }
    return true;
}

// Attempt one device recovery + re-upload after a device-removed failure.
bool recover_and_present(app_state& app, std::uint64_t* out_present_ticks = nullptr) {
    if (app.device_recovered) {
        return false;
    }
    app.device_recovered = true;
    RECT rc{};
    GetClientRect(app.hwnd, &rc);
    if (!oz::viewer::create_renderer(app.hwnd, app.renderer)) {
        return false;
    }
    if (!oz::viewer::resize_renderer(app.renderer, static_cast<std::uint32_t>(rc.right),
                                     static_cast<std::uint32_t>(rc.bottom))) {
        return false;
    }
    return upload_and_present(app, out_present_ticks);
}

// Guess the decode profile for a path from cached source metadata when
// available (no file I/O); otherwise 1 as a hint the worker re-derives.
int guess_den_for_path(app_state& app, const std::wstring& norm_path, int view_w,
                       int view_h) {
    std::int32_t sw = 0;
    std::int32_t sh = 0;
    if (app.cache.try_get_source_dims(norm_path, sw, sh)) {
        return oz::viewer_core::choose_scaling_denominator(sw, sh, view_w, view_h);
    }
    return 1;
}

// Path-based window membership (ignores scale_den) so worker-derived profiles
// remain protected without UI-side header I/O.
bool path_in_window(const std::vector<oz::viewer_core::cache_key>& window,
                    const std::wstring& norm_path) {
    for (const auto& w : window) {
        if (w.normalized_path == norm_path) {
            return true;
        }
    }
    return false;
}

// Corpus alias/hash for a navigation index.
const oz::viewer_core::corpus_manifest_entry* corpus_entry_for_index(app_state& app,
                                                                    std::size_t index) {
    const auto& entries = app.nav_bench_session.corpus().entries;
    const std::filesystem::path* p = app.nav.at(index);
    if (!p) {
        return nullptr;
    }
    const std::string filename = p->filename().string();
    if (index < entries.size() && entries[index].rel_filename == filename) {
        return &entries[index];
    }
    for (const auto& e : entries) {
        if (e.rel_filename == filename) {
            return &e;
        }
    }
    return nullptr;
}

// Transactional candidate presentation.
bool try_commit_candidate(app_state& app,
                          const std::shared_ptr<oz::viewer_core::decoded_image_data>& candidate,
                          const oz::viewer_core::cache_key& candidate_key,
                          std::size_t candidate_index,
                          const std::vector<oz::viewer_core::cache_key>& window) {
    if (!candidate) {
        return false;
    }
#if OZVIEWER_ENABLE_EDITING
    // A modal Resize draft belongs to the document being replaced. Abandon it
    // (Cancel semantics clear the presentation draft) before committing a new
    // document so no stale draft or pending validation survives the swap.
    if (app.resize_dlg != nullptr && IsWindow(app.resize_dlg)) {
        SendMessageW(app.resize_dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
    }
#endif
    auto old_current = app.current;
    const std::size_t old_committed = app.committed_index;
    const auto old_pin = app.cache.pinned_key();
    const auto old_view = app.view;
#if OZVIEWER_ENABLE_EDITING
    const bool old_suppress_edit_presentation = app.suppress_edit_presentation;
    app.suppress_edit_presentation = true;
#endif

    const auto layout = current_ui_layout(app);
    const int view_w = layout.canvas_physical.width;
    const int view_h = layout.canvas_physical.height;

    // Reset ViewState on committed new document/image
    oz::viewer_core::set_fit(app.view, candidate->src_width, candidate->src_height, view_w, view_h);

    std::uint64_t present_ticks = 0;
    app.current = candidate;
    bool presented = upload_and_present(app, &present_ticks);
    if (!presented) {
        presented = recover_and_present(app, &present_ticks);
    }
    if (presented) {
#if OZVIEWER_ENABLE_EDITING
        app.suppress_edit_presentation = old_suppress_edit_presentation;
#endif
        app.cache.insert(candidate_key, candidate, true, window);
        app.committed_index = candidate_index;

        const std::filesystem::path* nav_committed_path = app.nav.at(candidate_index);
        if (nav_committed_path) {
            app.doc.commit_document(*nav_committed_path);
        }

#if OZVIEWER_ENABLE_EDITING
        if (!app.benchmark && !app.nav_benchmark) {
            const std::filesystem::path* committed_path = app.nav.at(candidate_index);
            if (committed_path) {
                oz::viewer_core::document_metadata meta;
                meta.source_path = *committed_path;
                meta.source_dimensions = {candidate->src_width, candidate->src_height};
                meta.codec = candidate->codec;
                meta.codec_name = get_codec_name_str(candidate->codec);
                meta.file_size = candidate->source_file_size;
                meta.last_write_time = candidate->source_last_write_time;
                if (!app.image_doc) {
                    app.image_doc = std::make_unique<oz::viewer_core::image_document>(meta);
                } else {
                    app.image_doc->reset(meta);
                }

                if (candidate->scale_den == 1) {
                    auto buf = std::make_shared<oz::viewer::edit_source_buffer>();
                    buf->document_instance_id = app.image_doc->instance_id();
                    buf->source_path = *committed_path;
                    buf->file_size = meta.file_size;
                    buf->last_write_time = meta.last_write_time;
                    buf->dimensions = {candidate->width, candidate->height};
                    buf->stride = candidate->stride;
                    buf->pixels = candidate->pixels;
                    app.full_res_source = std::move(buf);
                } else {
                    app.full_res_source.reset();
                }
                update_edit_menu_state(app);
            }
        }
#else
        update_menu_command_state(app);
#endif

        if (app.nav_benchmark && app.current_nav_bench_req_id != 0) {
            const auto* corpus_entry = corpus_entry_for_index(app, candidate_index);
            const std::filesystem::path* committed_path = app.nav.at(candidate_index);
            const std::string committed_alias =
                corpus_entry != nullptr ? corpus_entry->alias
                                        : (committed_path ? committed_path->filename().string()
                                                          : std::string{});
            const std::string committed_sha256 =
                corpus_entry != nullptr ? corpus_entry->sha256 : std::string{};
            const std::size_t cache_entries = app.cache.size();
            const std::size_t resident_decoded = app.cache.resident_image_count();
            app.nav_bench_session.update_cache_stats(
                app.cache.used_bytes(), app.cache.budget(), cache_entries, resident_decoded);
            app.nav_bench_session.record_successful_present(
                app.current_nav_bench_req_id,
                present_ticks,
                candidate_index,
                committed_alias,
                committed_sha256,
                app.cache.budget(),
                app.cache.used_bytes(),
                cache_entries,
                resident_decoded);
            app.current_nav_bench_req_id = 0;
        }

        return true;
    }
    // Restore consistent old state; keep candidate only as preload if possible.
#if OZVIEWER_ENABLE_EDITING
    app.suppress_edit_presentation = old_suppress_edit_presentation;
#endif
    app.current = old_current;
    app.committed_index = old_committed;
    app.view = old_view;
    if (old_pin.has_value()) {
        app.cache.pin_current(*old_pin);
    } else {
        app.cache.unpin();
    }
    if (app.cache.can_admit_preload(candidate->byte_size())) {
        app.cache.insert(candidate_key, candidate, false, window);
    }
    return false;
}

// Build desired cache keys vector for the current desired window.
std::vector<oz::viewer_core::cache_key> build_desired_window_keys(app_state& app, int view_w,
                                                                 int view_h) {
    std::vector<oz::viewer_core::cache_key> window;
    const auto plan = oz::viewer_core::compute_preload_plan(
        app.nav.size(), app.desired_index, app.nav.items(), 1);
    window.reserve(plan.size());
    for (const auto& item : plan) {
        const std::wstring norm = oz::viewer_core::normalize_path_key(item.path);
        window.push_back({norm, guess_den_for_path(app, norm, view_w, view_h)});
    }
    return window;
}

// Schedule decode plan for the desired navigation state
static void schedule_navigation_plan(app_state& app) {
    if (app.nav.empty() || !app.persistent_worker_started) {
        return;
    }

    const auto layout = current_ui_layout(app);
    const int view_w = layout.canvas_physical.width;
    const int view_h = layout.canvas_physical.height;

    refresh_cache_budget(app);

    const auto window = build_desired_window_keys(app, view_w, view_h);
    const auto plan = oz::viewer_core::compute_preload_plan(
        app.nav.size(), app.desired_index, app.nav.items(), 1);

    std::vector<oz::viewer_core::decode_request> requests;
    requests.reserve(plan.size());

    bool p0_committed = false;
    if (!plan.empty()) {
        const auto& p0 = plan[0];
        const std::wstring p0_norm = oz::viewer_core::normalize_path_key(p0.path);
        auto cached = app.cache.find_for_viewport(p0_norm, view_w, view_h);
        if (cached) {
            const auto key_opt = app.cache.find_key_for_viewport(p0_norm, view_w, view_h);
            const oz::viewer_core::cache_key key =
                key_opt.has_value() ? *key_opt
                                    : oz::viewer_core::cache_key{p0_norm, cached->scale_den};
            p0_committed = try_commit_candidate(app, cached, key, p0.index, window);
        } else if (app.comp_queue.has_valid_for_path(p0_norm, view_w, view_h)) {
            p0_committed = true;
        }
    }

    for (const auto& item : plan) {
        const std::wstring norm = oz::viewer_core::normalize_path_key(item.path);
        const int den = guess_den_for_path(app, norm, view_w, view_h);
        oz::viewer_core::cache_key key{norm, den};
        if (app.cache.find_for_viewport(norm, view_w, view_h)) {
            continue;
        }
        if (app.cache.preloads_disabled() && item.priority != oz::viewer_core::work_priority::p0_selected) {
            continue;
        }
        if (p0_committed && item.priority == oz::viewer_core::work_priority::p0_selected) {
            continue;
        }

        oz::viewer_core::decode_request req;
        req.epoch = app.current_epoch;
        req.priority = item.priority;
        req.key = key;
        req.path = item.path;
        req.view_w = view_w;
        req.view_h = view_h;
        req.is_initial = false;
        requests.push_back(std::move(req));
    }

    app.work_queue.replace_plan(app.current_epoch, std::move(requests));
}

void execute_benchmark_rearm(app_state& app, std::size_t index) {
    if (app.nav.empty() || index >= app.nav.size()) {
        return;
    }
    app.desired_index = index;
    schedule_navigation_plan(app);
}

// Helper to get active presentation plan for current document/view
static oz::viewer::presentation_plan compute_active_presentation_plan(app_state& app, int view_w, int view_h) {
    oz::viewer::presentation_plan plan;
    if (!app.have_image || !app.current) {
        return plan;
    }

    const auto layout = current_ui_layout(app);
    plan.canvas_offset_x = layout.canvas_physical.x;
    plan.canvas_offset_y = layout.canvas_physical.y;

#if OZVIEWER_ENABLE_EDITING
    // Suppression wins over edit state: during a transactional document swap
    // (file open / navigation commit / clipboard paste) app.image_doc still
    // describes the document being replaced, so its history/draft must not
    // transform the incoming bitmap. should_use_edit_presentation checks
    // suppression before touching history/draft; when it is false the plain
    // fit path below presents the incoming bitmap unmodified.
    if (oz::viewer::should_use_edit_presentation(app.suppress_edit_presentation, app.image_doc.get(),
                                                 app.draft_resize)) {
        const auto source_dims = app.image_doc->metadata().source_dimensions;
        if (!source_dims.is_valid()) {
            return plan;
        }

        const auto active_cmds = app.image_doc->history().active_commands();
        const auto proxy = oz::viewer_core::compute_affine_proxy_plan(
            source_dims,
            std::span<const oz::viewer_core::edit_command>(active_cmds.data(), active_cmds.size()),
            app.draft_resize);

        if (!proxy.valid) {
            return plan;
        }

        plan.logical_width = proxy.logical_dimensions.width;
        plan.logical_height = proxy.logical_dimensions.height;
        plan.transform = proxy.transform;

        // Map subpixel source rectangle from full-res source coordinates into current retained bitmap coordinates
        if (app.current->width > 0 && app.current->height > 0) {
            const float sx = static_cast<float>(app.current->width) / static_cast<float>(source_dims.width);
            const float sy = static_cast<float>(app.current->height) / static_cast<float>(source_dims.height);
            plan.source_rectf = oz::viewer_core::edit_rectf{
                proxy.source_rectf.x * sx,
                proxy.source_rectf.y * sy,
                proxy.source_rectf.width * sx,
                proxy.source_rectf.height * sy
            };
        }

        plan.geometry = oz::viewer_core::compute_view_geometry(
            app.view, plan.logical_width, plan.logical_height, view_w, view_h);
        return plan;
    }
#endif

    plan.logical_width = app.current->src_width;
    plan.logical_height = app.current->src_height;
    plan.transform = oz::viewer_core::d4_transform::make_identity();
    plan.geometry = oz::viewer_core::compute_view_geometry(
        app.view, app.current->src_width, app.current->src_height, view_w, view_h);
    return plan;
}

// Redraw current view without re-reading or re-uploading the source bitmap.
static void redraw_current_view(app_state& app) {
    if (!app.have_image || !app.current) {
        oz::viewer::render_blank(app.renderer);
        return;
    }
    const auto layout = current_ui_layout(app);
    const int view_w = layout.canvas_physical.width;
    const int view_h = layout.canvas_physical.height;
    const auto plan = compute_active_presentation_plan(app, view_w, view_h);
    #if OZVIEWER_ENABLE_EDITING
    const auto overlay = current_crop_overlay(app, plan);
    const auto* overlay_ptr = overlay.has_value() ? &*overlay : nullptr;
    #else
    const oz::viewer::crop_overlay* overlay_ptr = nullptr;
    #endif
    if (!oz::viewer::redraw_retained_plan(app.renderer, plan, overlay_ptr)) {
        // Fallback: if retained bitmap missing or device lost, attempt upload
        upload_and_present(app);
    }
}

// Dirty document safety check (Stage 5 Slice 3).
// Prompts user with "Discard edits" or "Cancel" when document has unsaved modifications.
// Returns true if operation is allowed to proceed (not dirty, or user confirmed Discard).
// Returns false if user clicked Cancel.
static bool check_dirty_discard_allowed(app_state& app) {
#if OZVIEWER_ENABLE_EDITING
    if (app.benchmark || app.nav_benchmark) {
        return true;
    }
    // A pending/in-flight save must finish (or be cancelled and fully cleaned
    // up) before the document can be replaced: never allow a late write after
    // a document switch.
    if (!resolve_active_save_for_action(app)) {
        return false;
    }
    const bool crop_active = app.crop.has_value() && app.crop->active;
    const bool document_dirty = app.image_doc && app.image_doc->is_dirty();
    if (crop_active || document_dirty) {
        const auto prompt_rc = oz::viewer::show_prompt(
            app.hwnd,
            L"OZ Image Viewer",
            crop_active && !document_dirty
                ? L"A crop selection is in progress."
                : L"You have unsaved edits in this document.",
            crop_active && !document_dirty
                ? L"Cancel crop and continue?"
                : L"Discard edits and continue?",
            MB_OKCANCEL,
            MB_ICONWARNING);
        if (prompt_rc != oz::viewer::prompt_result::ok) {
            return false; // User cancelled
        }
        if (crop_active) {
            app.crop.reset();
            update_crop_window_title(app);
            update_edit_menu_state(app);
            if (app.have_image && app.current) {
                redraw_current_view(app);
            }
        }
    }
#else
    (void)app;
#endif
    return true;
}

static oz::viewer::ui_layout_config current_ui_layout_config(const app_state& app) noexcept {
    oz::viewer::ui_layout_config config{};
    const bool is_crop_active =
#if OZVIEWER_ENABLE_EDITING
        app.crop.has_value() && app.crop->active;
#else
        false;
#endif

    if (app.benchmark || app.nav_benchmark) {
        config.show_toolbar = false;
        config.toolbar_height_dip = 0.0f;
        config.show_crop_strip = false;
        config.crop_strip_height_dip = 0.0f;
        config.show_status = false;
        config.status_height_dip = 0.0f;
    } else if (app.is_fullscreen) {
        // Fullscreen: toolbar and status bar hide; crop strip stays visible while crop is active
        config.show_toolbar = false;
        config.toolbar_height_dip = 0.0f;
        config.show_crop_strip = is_crop_active;
        config.crop_strip_height_dip = is_crop_active ? 32.0f : 0.0f;
        config.show_status = false;
        config.status_height_dip = 0.0f;
    } else {
        config.show_toolbar = true;
        config.toolbar_height_dip = 32.0f;
        config.show_crop_strip = is_crop_active;
        config.crop_strip_height_dip = is_crop_active ? 32.0f : 0.0f;
        config.show_status = true;
        config.status_height_dip = 24.0f;
    }
    config.open_btn_width_dip = 160.0f;
    config.open_btn_height_dip = 44.0f;
    return config;
}

static oz::viewer::ui_layout_result current_ui_layout(const app_state& app) noexcept {
    if (!app.hwnd) {
        return {};
    }
    RECT rc{};
    GetClientRect(app.hwnd, &rc);
    const float dpi = (app.renderer.dpi > 0.0f) ? app.renderer.dpi : 96.0f;
    return oz::viewer::compute_ui_layout(static_cast<std::int32_t>(rc.right),
                                         static_cast<std::int32_t>(rc.bottom),
                                         dpi,
                                         current_ui_layout_config(app));
}

static oz::viewer::ui_command_context current_ui_command_context(const app_state& app) noexcept {
    oz::viewer::ui_command_context ctx{};
    ctx.benchmark_mode = app.benchmark || app.nav_benchmark;
    ctx.have_image = app.have_image && (app.current != nullptr);
    ctx.has_document = app.doc.has_document;
    ctx.has_file_path = app.doc.has_document && !app.doc.active_path.empty();
#if OZVIEWER_ENABLE_EDITING
    if (app.image_doc) {
        const bool have_source =
            (app.full_res_source && app.full_res_source->is_valid() &&
             app.full_res_source->document_instance_id == app.image_doc->instance_id()) ||
            !app.image_doc->metadata().source_path.empty();
        ctx.has_source = have_source;
        ctx.is_dirty = app.image_doc->is_dirty();
        ctx.can_undo = app.image_doc->history().can_undo();
        ctx.can_redo = app.image_doc->history().can_redo();
    }
    if (app.crop.has_value()) {
        ctx.crop_active = app.crop->active;
        ctx.crop_preset = app.crop->preset;
    }
    ctx.resize_busy = (app.resize_dlg != nullptr) || app.pending_resize.has_value();
    ctx.save_busy = app.pending_save.has_value() || (app.save_ctrl && app.save_ctrl->is_active());
#else
    // Editing-OFF builds retain no full-resolution source buffer: the
    // presentable retained bitmap itself is the Copy Image source (mirroring
    // the OFF copy submission path, which publishes app.current directly).
    // Without this a temporary clipboard document (no file path) would leave
    // Copy Image disabled even though its pixels are fully copyable.
    ctx.has_source = app.have_image && (app.current != nullptr);
#endif
    ctx.copy_busy = app.pending_copy.has_value() || (app.copy_ctrl && app.copy_ctrl->is_active());

    ctx.is_fit_mode = (app.view.mode == oz::viewer_core::view_mode::fit);
    ctx.is_fullscreen = app.is_fullscreen;
    ctx.has_navigation_items = !app.nav.empty();
    if (app.nav.active_index().has_value() && !app.nav.empty()) {
        const std::size_t idx = *app.nav.active_index();
        ctx.can_navigate_prev = (idx > 0);
        ctx.can_navigate_next = (idx + 1 < app.nav.size());
    }
    return ctx;
}

static void update_chrome_status(app_state& app) {
    if (!app.chrome || !app.chrome->is_created() || app.benchmark || app.nav_benchmark) {
        return;
    }
    oz::viewer::status_bar_info info;
    if (app.have_image && app.current) {
        if (!app.doc.active_path.empty()) {
            info.file_name = app.doc.active_path.filename().wstring();
        } else if (!app.initial_path.empty()) {
            info.file_name = app.initial_path.filename().wstring();
        } else {
            info.file_name = L"Image";
        }

        const auto dims = active_view_dimensions(app);
        if (dims.is_valid()) {
            wchar_t dim_buf[64];
            swprintf_s(dim_buf, L"%d × %d", dims.width, dims.height);
            info.dimensions = dim_buf;
        }

        const double zoom_pct = (app.view.scale * 100.0);
        wchar_t zoom_buf[64];
        if (app.view.mode == oz::viewer_core::view_mode::fit) {
            swprintf_s(zoom_buf, L"Fit (%.0f%%)", zoom_pct);
        } else {
            swprintf_s(zoom_buf, L"%.0f%%", zoom_pct);
        }
        info.zoom = zoom_buf;

        if (!app.nav.empty() && app.nav.active_index().has_value()) {
            wchar_t nav_buf[64];
            swprintf_s(nav_buf, L"%zu / %zu", (*app.nav.active_index()) + 1, app.nav.size());
            info.navigation = nav_buf;
        }

#if OZVIEWER_ENABLE_EDITING
        if (app.crop.has_value() && app.crop->active) {
            info.state_label = L"Crop mode";
        } else if (app.pending_save.has_value() || (app.save_ctrl && app.save_ctrl->is_active())) {
            info.state_label = L"Saving...";
        } else if (app.pending_resize.has_value() || app.resize_dlg != nullptr) {
            info.state_label = L"Resizing...";
        } else if (app.image_doc && app.image_doc->is_dirty()) {
            info.state_label = L"Modified";
        }
#endif
    } else {
        info.file_name = L"No image open";
    }
    app.chrome->update_status(info);
}

static void update_menu_command_state(app_state& app) {
    if (app.benchmark || app.nav_benchmark) {
        return;
    }

    const auto ctx = current_ui_command_context(app);

    if (app.chrome && app.chrome->is_created()) {
        app.chrome->update_command_states(ctx);
        update_chrome_status(app);
    }

#if OZVIEWER_ENABLE_EDITING
    if (app.crop_strip_ui && app.crop_strip_ui->is_created()) {
        const auto layout = current_ui_layout(app);
        const bool crop_active = app.crop.has_value() && app.crop->active;
        app.crop_strip_ui->update_layout(layout, crop_active);
        app.crop_strip_ui->sync_state(app.crop.has_value() ? &*app.crop : nullptr);
    }
#endif

    if (app.hMenuFile) {
        const auto copy_img_st = oz::viewer::derive_command_state(oz::viewer::ui_command_id::file_copy_image, ctx);
        EnableMenuItem(app.hMenuFile, kIdFileCopyImage,
                       MF_BYCOMMAND | (copy_img_st.enabled ? MF_ENABLED : MF_GRAYED));

        const auto copy_file_st = oz::viewer::derive_command_state(oz::viewer::ui_command_id::file_copy, ctx);
        EnableMenuItem(app.hMenuFile, kIdFileCopy,
                       MF_BYCOMMAND | (copy_file_st.enabled ? MF_ENABLED : MF_GRAYED));

        const auto copy_path_st = oz::viewer::derive_command_state(oz::viewer::ui_command_id::file_copy_path, ctx);
        EnableMenuItem(app.hMenuFile, kIdFileCopyPath,
                       MF_BYCOMMAND | (copy_path_st.enabled ? MF_ENABLED : MF_GRAYED));

        const auto open_loc_st = oz::viewer::derive_command_state(oz::viewer::ui_command_id::file_open_location, ctx);
        EnableMenuItem(app.hMenuFile, kIdFileOpenLocation,
                       MF_BYCOMMAND | (open_loc_st.enabled ? MF_ENABLED : MF_GRAYED));

#if OZVIEWER_ENABLE_EDITING
        if (app.image_doc) {
            const auto save_st = oz::viewer::derive_command_state(oz::viewer::ui_command_id::file_save, ctx);
            const auto save_as_st = oz::viewer::derive_command_state(oz::viewer::ui_command_id::file_save_as, ctx);
            EnableMenuItem(app.hMenuFile, kIdFileSave,
                           MF_BYCOMMAND | (save_st.enabled ? MF_ENABLED : MF_GRAYED));
            EnableMenuItem(app.hMenuFile, kIdFileSaveAs,
                           MF_BYCOMMAND | (save_as_st.enabled ? MF_ENABLED : MF_GRAYED));
        }
#endif
    }

#if OZVIEWER_ENABLE_EDITING
    if (!app.hMenuEdit) {
        return;
    }

    const auto rot_l_st = oz::viewer::derive_command_state(oz::viewer::ui_command_id::edit_rotate_left, ctx);
    const auto rot_r_st = oz::viewer::derive_command_state(oz::viewer::ui_command_id::edit_rotate_right, ctx);
    const auto flip_h_st = oz::viewer::derive_command_state(oz::viewer::ui_command_id::edit_flip_h, ctx);
    const auto flip_v_st = oz::viewer::derive_command_state(oz::viewer::ui_command_id::edit_flip_v, ctx);
    const auto crop_st = oz::viewer::derive_command_state(oz::viewer::ui_command_id::edit_crop, ctx);
    const auto resize_st = oz::viewer::derive_command_state(oz::viewer::ui_command_id::edit_resize, ctx);

    EnableMenuItem(app.hMenuEdit, kIdEditRotateLeft, MF_BYCOMMAND | (rot_l_st.enabled ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(app.hMenuEdit, kIdEditRotateRight, MF_BYCOMMAND | (rot_r_st.enabled ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(app.hMenuEdit, kIdEditFlipH, MF_BYCOMMAND | (flip_h_st.enabled ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(app.hMenuEdit, kIdEditFlipV, MF_BYCOMMAND | (flip_v_st.enabled ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(app.hMenuEdit, kIdEditCrop, MF_BYCOMMAND | (crop_st.enabled ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(app.hMenuEdit, kIdEditResize, MF_BYCOMMAND | (resize_st.enabled ? MF_ENABLED : MF_GRAYED));

    const auto undo_st = oz::viewer::derive_command_state(oz::viewer::ui_command_id::edit_undo, ctx);
    const auto redo_st = oz::viewer::derive_command_state(oz::viewer::ui_command_id::edit_redo, ctx);

    EnableMenuItem(app.hMenuEdit, kIdEditUndo, MF_BYCOMMAND | (undo_st.enabled ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(app.hMenuEdit, kIdEditRedo, MF_BYCOMMAND | (redo_st.enabled ? MF_ENABLED : MF_GRAYED));

    const auto crop_apply_st = oz::viewer::derive_command_state(oz::viewer::ui_command_id::edit_crop_apply, ctx);
    const auto crop_cancel_st = oz::viewer::derive_command_state(oz::viewer::ui_command_id::edit_crop_cancel, ctx);
    const auto crop_reset_st = oz::viewer::derive_command_state(oz::viewer::ui_command_id::edit_crop_reset, ctx);

    EnableMenuItem(app.hMenuEdit, kIdEditCropApply, MF_BYCOMMAND | (crop_apply_st.enabled ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(app.hMenuEdit, kIdEditCropCancel, MF_BYCOMMAND | (crop_cancel_st.enabled ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(app.hMenuEdit, kIdEditCropReset, MF_BYCOMMAND | (crop_reset_st.enabled ? MF_ENABLED : MF_GRAYED));

    if (app.hMenuCropRatio) {
        static constexpr struct {
            int menu_id;
            oz::viewer::ui_command_id cmd_id;
        } kRatioMap[] = {
            {kIdCropRatioFree, oz::viewer::ui_command_id::crop_ratio_free},
            {kIdCropRatioOriginal, oz::viewer::ui_command_id::crop_ratio_original},
            {kIdCropRatio1x1, oz::viewer::ui_command_id::crop_ratio_1x1},
            {kIdCropRatio3x2, oz::viewer::ui_command_id::crop_ratio_3x2},
            {kIdCropRatio4x3, oz::viewer::ui_command_id::crop_ratio_4x3},
            {kIdCropRatio5x4, oz::viewer::ui_command_id::crop_ratio_5x4},
            {kIdCropRatio16x9, oz::viewer::ui_command_id::crop_ratio_16x9},
            {kIdCropRatio9x16, oz::viewer::ui_command_id::crop_ratio_9x16},
            {kIdCropRatioASeries, oz::viewer::ui_command_id::crop_ratio_a_series}
        };

        for (const auto& item : kRatioMap) {
            const auto st = oz::viewer::derive_command_state(item.cmd_id, ctx);
            EnableMenuItem(app.hMenuCropRatio, item.menu_id, MF_BYCOMMAND | (st.enabled ? MF_ENABLED : MF_GRAYED));
            CheckMenuItem(app.hMenuCropRatio, item.menu_id, MF_BYCOMMAND | (st.checked ? MF_CHECKED : MF_UNCHECKED));
        }
    }
#endif
}

#if OZVIEWER_ENABLE_EDITING
static void update_edit_menu_state(app_state& app) {
    update_menu_command_state(app);
}
#else
static void update_edit_menu_state(app_state& app) {
    update_menu_command_state(app);
}
#endif

#if OZVIEWER_ENABLE_EDITING
// Request background materialization if needed
static void schedule_edit_materialization(app_state& app) {
    if (!app.have_image || !app.current || !app.image_doc || !app.edit_ctrl) {
        return;
    }

    oz::viewer::edit_work_request req;
    req.document_instance_id = app.image_doc->instance_id();
    req.edit_revision = app.image_doc->edit_revision();

    if (!app.full_res_source || !app.full_res_source->is_valid()) {
        req.needs_source_acquisition = true;
        req.source_path = app.image_doc->metadata().source_path;
        const auto& baseline = app.image_doc->source_baseline();
        if (baseline.valid) {
            req.expected_file_size = baseline.file_size;
            req.expected_last_write_time = baseline.last_write_time;
        } else {
            req.expected_file_size = app.image_doc->metadata().file_size;
            req.expected_last_write_time = app.image_doc->metadata().last_write_time;
        }
        req.expected_dimensions = app.image_doc->metadata().source_dimensions;
    } else {
        req.needs_source_acquisition = false;
        req.existing_source = app.full_res_source;
    }

    req.needs_materialization = true;
    const auto active_cmds = app.image_doc->history().active_commands();
    req.commands.assign(active_cmds.begin(), active_cmds.end());
    req.memory_budget = oz::viewer_core::get_system_edit_admission_budget();

    app.edit_ctrl->submit_request(std::move(req));
}

// Apply a committed non-destructive command (Rotate/Flip/Crop in this slice).
static bool apply_edit_command(app_state& app, const oz::viewer_core::edit_command& cmd) {
    if (!app.have_image || !app.current || !app.image_doc || app.benchmark || app.nav_benchmark) {
        return false;
    }
    if (edit_transition_blocked(app)) {
        return false;  // A pending/in-flight Save owns the document.
    }

    // Try push command into history
    if (!app.image_doc->history().push_command(cmd)) {
        return false;
    }

    // Presentation plan now reflects the updated transform/dimensions
    // Redraw immediately using GPU geometry without CPU re-upload
    const auto layout = current_ui_layout(app);
    const int view_w = layout.canvas_physical.width;
    const int view_h = layout.canvas_physical.height;
    const auto plan = compute_active_presentation_plan(app, view_w, view_h);

    if (!oz::viewer::redraw_retained_plan(app.renderer, plan)) {
        // Device loss or error: attempt recovery
        if (!upload_and_present(app)) {
            // Failed: rollback command from history
            app.image_doc->history().undo();
            return false;
        }
    }

    update_edit_menu_state(app);
    schedule_edit_materialization(app);
    return true;
}

// Promote a worker-acquired immutable full-resolution *source* decode into the
// retained bitmap when the current retained decode is scaled or otherwise
// differs. Only `edit_source_buffer` (an immutable decode of the original
// file) is eligible here: a materialized edit result must never become
// `renderer.source_bitmap` or `app.current`.
static bool adopt_acquired_source(app_state& app, const oz::viewer::edit_source_buffer& source) {
    if (!app.have_image || !app.current || !source.is_valid()) {
        return false;
    }
    if (app.current->scale_den == 1 &&
        app.current->width == source.dimensions.width &&
        app.current->height == source.dimensions.height) {
        return true;  // Retained bitmap already is the full-resolution decode.
    }

    auto upgraded = std::make_shared<oz::viewer_core::decoded_image_data>();
    upgraded->pixels = source.pixels;
    upgraded->width = source.dimensions.width;
    upgraded->height = source.dimensions.height;
    upgraded->stride = source.stride;
    upgraded->src_width = source.dimensions.width;
    upgraded->src_height = source.dimensions.height;
    upgraded->scale_den = 1;
    upgraded->codec = app.current->codec;

    const auto previous = app.current;
    app.current = upgraded;
    const auto layout = current_ui_layout(app);
    const int view_w = layout.canvas_physical.width;
    const int view_h = layout.canvas_physical.height;
    const auto plan = compute_active_presentation_plan(app, view_w, view_h);
    const bool presented = oz::viewer::render_and_present_candidate_plan(
        app.renderer, upgraded->pixels.data(), upgraded->stride,
        upgraded->width, upgraded->height,
        upgraded->src_width, upgraded->src_height, plan);
    if (!presented) {
        app.current = previous;
        return false;
    }
    return true;
}

// Bounded wide rendering of a worker error detail. Pipeline details are ASCII
// engineering strings; anything longer is truncated before it reaches the UI.
static std::wstring widen_bounded_error(const std::string& text, const wchar_t* fallback) {
    constexpr std::size_t kMaxChars = 120;
    if (text.empty()) {
        return fallback;
    }
    std::wstring wide(text.begin(), text.end());
    if (wide.size() > kMaxChars) {
        wide.resize(kMaxChars);
    }
    return wide;
}

// Verifies that a completion for the pending Resize validation was produced
// from the same source identity the request was submitted against. This is an
// explicit identity/generation check, not a request-id-only acceptance.
static bool completion_source_identity_matches(
    const app_state& app,
    const app_state::pending_resize_validation& pending,
    const oz::viewer::edit_completion_msg& comp) {
    if (comp.source) {
        const auto& source = *comp.source;
        if (!source.is_valid() || source.document_instance_id != pending.document_instance_id) {
            return false;
        }
        if (pending.source_identity_expected &&
            (source.source_path != pending.source_path ||
             source.file_size != pending.source_file_size ||
             source.last_write_time != pending.source_last_write_time ||
             source.dimensions != pending.source_dimensions)) {
            return false;
        }
        return true;
    }

    // No acquired source in the completion: the request must have validated the
    // exact source the UI still holds.
    if (pending.source_was_acquired) {
        return false;
    }
    if (!app.full_res_source || !app.full_res_source->is_valid()) {
        return false;
    }
    if (pending.source_identity_expected &&
        (app.full_res_source->document_instance_id != pending.document_instance_id ||
         app.full_res_source->source_path != pending.source_path ||
         app.full_res_source->file_size != pending.source_file_size ||
         app.full_res_source->last_write_time != pending.source_last_write_time ||
         app.full_res_source->dimensions != pending.source_dimensions)) {
        return false;
    }
    return true;
}

// Two-phase Resize Apply, phase 2 (UI thread): commit exactly one
// automatic-filter resize command and present it through the geometry-only
// proxy from the retained immutable source. The materialized pixels carried by
// the completion are validation-only and are discarded here.
static bool commit_validated_resize(app_state& app,
                                    const app_state::pending_resize_validation& pending,
                                    oz::viewer::edit_completion_msg& comp) {
    if (!app.image_doc) {
        return false;
    }

    // Source-only promotion is best-effort; the resize can still be presented
    // from the current retained source if the upgrade fails.
    if (comp.source && comp.source->is_valid()) {
        adopt_acquired_source(app, *comp.source);
    }
    app.draft_resize.reset();

    const oz::viewer_core::resize_command command{
        pending.target, oz::viewer_core::resize_filter::automatic};
    if (!app.image_doc->history().push_command(command)) {
        return false;
    }

    RECT rc{};
    GetClientRect(app.hwnd, &rc);
    const auto plan = compute_active_presentation_plan(
        app, static_cast<int>(rc.right), static_cast<int>(rc.bottom));
    if (!oz::viewer::redraw_retained_plan(app.renderer, plan)) {
        if (!upload_and_present(app)) {
            app.image_doc->history().undo();
            return false;
        }
    }

    // The validated materialization already corresponded to exactly this
    // command sequence, so no further worker submission is required here.
    comp.result.pixels.clear();
    return true;
}

// Two-phase Resize Apply, phase 3 (UI thread, kMsgEditComplete): validate the
// completion identity, then commit the single history transition. Any worker
// failure, superseded/stale completion, source/document/revision mismatch or
// presentation failure leaves history unchanged and reports a bounded error.
static void handle_pending_resize_completion(app_state& app,
                                             oz::viewer::edit_completion_msg& comp) {
    const app_state::pending_resize_validation pending = *app.pending_resize;
    app.pending_resize.reset();

    std::wstring error;
    bool committed = false;

    const bool document_matches =
        app.image_doc && comp.document_instance_id == pending.document_instance_id &&
        app.image_doc->instance_id() == pending.document_instance_id;
    const bool revision_matches =
        document_matches && comp.edit_revision == pending.base_revision &&
        app.image_doc->edit_revision() == pending.base_revision;
    const bool token_matches = comp.request_id == pending.request_id;

    if (!token_matches || !document_matches || !revision_matches) {
        error = L"Resize validation was superseded; no changes were applied.";
    } else if (!completion_source_identity_matches(app, pending, comp)) {
        error = L"The source image changed during resize validation; no changes were applied.";
    } else if (!comp.ok) {
        error = widen_bounded_error(comp.error_detail, L"Resize validation failed.");
    } else if (!comp.result.dimensions.is_valid() || comp.result.dimensions != pending.target) {
        error = L"Resize validation produced unexpected dimensions; no changes were applied.";
    } else {
        committed = commit_validated_resize(app, pending, comp);
        if (!committed) {
            error = L"Resize could not be presented; history was left unchanged.";
        }
    }

    // The draft never survives an Apply outcome: on success the command was
    // committed, otherwise the previous committed canvas is restored.
    app.draft_resize.reset();
    update_edit_menu_state(app);
    if (!committed) {
        redraw_current_view(app);
    }

    if (app.resize_dlg != nullptr && IsWindow(app.resize_dlg)) {
        oz::viewer::notify_resize_dialog_result(
            app.resize_dlg, committed, committed ? std::wstring{} : error);
    }
}

// Two-phase Resize Apply, phase 1 (UI thread): capture document instance, base
// revision, source identity, target dimensions and the proposed command
// sequence, then submit exactly one latest-only edit controller
// validation/materialization request.
static bool submit_resize_validation(app_state& app, HWND dialog,
                                     const oz::viewer_core::edit_dimensions& target) {
    if (!dialog || !app.edit_ctrl || !app.image_doc || !app.current ||
        app.benchmark || app.nav_benchmark) {
        return false;
    }
    if (edit_transition_blocked(app)) {
        return false;  // A pending/in-flight Save owns the document.
    }
    if (app.pending_resize.has_value()) {
        return false;  // At most one in-flight validation request/completion.
    }
    const auto dims = app.image_doc->current_dimensions();
    if (!dims.has_value() || !dims->is_valid()) {
        return false;
    }
    const oz::viewer_core::resize_command command{
        target, oz::viewer_core::resize_filter::automatic};
    if (!oz::viewer_core::is_command_valid(*dims, command) || target == *dims) {
        return false;
    }

    oz::viewer::edit_work_request req;
    req.document_instance_id = app.image_doc->instance_id();
    req.edit_revision = app.image_doc->edit_revision();

    app_state::pending_resize_validation pending;
    pending.document_instance_id = req.document_instance_id;
    pending.base_revision = req.edit_revision;
    pending.target = target;

    if (!app.full_res_source || !app.full_res_source->is_valid() ||
        app.full_res_source->document_instance_id != app.image_doc->instance_id()) {
        req.needs_source_acquisition = true;
        req.source_path = app.image_doc->metadata().source_path;
        const auto& baseline = app.image_doc->source_baseline();
        if (baseline.valid) {
            req.expected_file_size = baseline.file_size;
            req.expected_last_write_time = baseline.last_write_time;
            pending.source_identity_expected = true;
        } else {
            req.expected_file_size = app.image_doc->metadata().file_size;
            req.expected_last_write_time = app.image_doc->metadata().last_write_time;
            pending.source_identity_expected = (req.expected_file_size != 0 || req.expected_last_write_time != 0);
        }
        req.expected_dimensions = app.image_doc->metadata().source_dimensions;
        pending.source_was_acquired = true;
        pending.source_path = req.source_path;
        pending.source_file_size = req.expected_file_size;
        pending.source_last_write_time = req.expected_last_write_time;
        pending.source_dimensions = req.expected_dimensions;
    } else {
        req.needs_source_acquisition = false;
        req.existing_source = app.full_res_source;
        pending.source_path = app.full_res_source->source_path;
        pending.source_file_size = app.full_res_source->file_size;
        pending.source_last_write_time = app.full_res_source->last_write_time;
        pending.source_dimensions = app.full_res_source->dimensions;
        pending.source_identity_expected = true;
    }

    req.needs_materialization = true;
    const auto active_cmds = app.image_doc->history().active_commands();
    req.commands.assign(active_cmds.begin(), active_cmds.end());
    req.commands.push_back(command);
    req.memory_budget = oz::viewer_core::get_system_edit_admission_budget();

    pending.request_id = app.edit_ctrl->submit_request(std::move(req));
    app.resize_dlg = dialog;
    app.pending_resize = std::move(pending);
    return true;
}

static void apply_undo(app_state& app) {
    if (!app.have_image || !app.current || !app.image_doc || app.benchmark || app.nav_benchmark) {
        return;
    }
    if (edit_transition_blocked(app)) {
        return;  // A pending/in-flight Save owns the document.
    }
    if (!app.image_doc->history().can_undo()) {
        return;
    }
    if (!app.image_doc->history().undo()) {
        return;
    }

    const auto layout = current_ui_layout(app);
    const int view_w = layout.canvas_physical.width;
    const int view_h = layout.canvas_physical.height;
    const auto plan = compute_active_presentation_plan(app, view_w, view_h);

    if (!oz::viewer::redraw_retained_plan(app.renderer, plan)) {
        // A presentation failure must not leave the history cursor claiming an
        // unpresentable canvas: attempt device recovery, then roll back.
        if (!upload_and_present(app)) {
            app.image_doc->history().redo();
            return;
        }
    }

    update_edit_menu_state(app);
    schedule_edit_materialization(app);
}

static void apply_redo(app_state& app) {
    if (!app.have_image || !app.current || !app.image_doc || app.benchmark || app.nav_benchmark) {
        return;
    }
    if (edit_transition_blocked(app)) {
        return;  // A pending/in-flight Save owns the document.
    }
    if (!app.image_doc->history().can_redo()) {
        return;
    }
    if (!app.image_doc->history().redo()) {
        return;
    }

    const auto layout = current_ui_layout(app);
    const int view_w = layout.canvas_physical.width;
    const int view_h = layout.canvas_physical.height;
    const auto plan = compute_active_presentation_plan(app, view_w, view_h);

    if (!oz::viewer::redraw_retained_plan(app.renderer, plan)) {
        // Redo never adds a command; a failed presentation only rolls the
        // cursor back so the visible canvas and history stay consistent.
        if (!upload_and_present(app)) {
            app.image_doc->history().undo();
            return;
        }
    }

    update_edit_menu_state(app);
    schedule_edit_materialization(app);
}

static void begin_crop_mode(app_state& app) {
    if (!app.have_image || !app.current || !app.image_doc || app.benchmark || app.nav_benchmark ||
        edit_transition_blocked(app) ||
        (app.crop.has_value() && app.crop->active)) {
        return;
    }
    const auto dims = app.image_doc->current_dimensions();
    if (!dims.has_value() || !dims->is_valid()) {
        return;
    }
    app.is_panning = false;
    app.crop = oz::viewer_core::begin_crop_interaction(*dims);
    if (!app.crop->active) {
        app.crop.reset();
        return;
    }
    update_crop_window_title(app);
    update_edit_menu_state(app);
    redraw_current_view(app);
}

static void cancel_crop_mode(app_state& app) {
    if (!app.crop.has_value()) {
        return;
    }
    app.crop.reset();
    app.is_panning = false;
    if (GetCapture() == app.hwnd) {
        ReleaseCapture();
    }
    update_crop_window_title(app);
    update_edit_menu_state(app);
    redraw_current_view(app);
}

static void reset_crop_mode(app_state& app) {
    if (!app.crop.has_value() || !app.crop->active) {
        return;
    }
    oz::viewer_core::reset_crop_interaction(*app.crop);
    if (app.crop_strip_ui && app.crop_strip_ui->is_created()) {
        app.crop_strip_ui->sync_state(&*app.crop);
    }
    redraw_current_view(app);
}

static void set_crop_ratio(app_state& app, oz::viewer_core::crop_ratio_preset preset) {
    if (!app.crop.has_value() || !app.crop->active) {
        return;
    }
    oz::viewer_core::set_crop_interaction_ratio(*app.crop, preset);
    if (app.crop_strip_ui && app.crop_strip_ui->is_created()) {
        app.crop_strip_ui->sync_state(&*app.crop);
    }
    update_edit_menu_state(app);
    redraw_current_view(app);
}

static void apply_crop_mode(app_state& app) {
    if (!app.crop.has_value() || !app.crop->active || !app.image_doc) {
        return;
    }
    const auto selection = app.crop->selection;
    const auto dims = app.image_doc->current_dimensions();
    if (!selection.is_valid() || !dims.has_value() || !dims->is_valid() ||
        !oz::viewer_core::is_command_valid(*dims, oz::viewer_core::crop_command{selection})) {
        return;
    }

    // Cropping the full canvas is a visual no-op; leave history pristine.
    if (selection.x == 0 && selection.y == 0 &&
        selection.width == dims->width && selection.height == dims->height) {
        cancel_crop_mode(app);
        return;
    }

    const auto draft = *app.crop;
    app.crop.reset();
    update_crop_window_title(app);
    if (!apply_edit_command(app, oz::viewer_core::crop_command{selection})) {
        // Restore the exact draft only if the command did not commit.
        app.crop = draft;
        update_crop_window_title(app);
    }
    update_edit_menu_state(app);
    redraw_current_view(app);
}

static void open_resize_modal_dialog(app_state& app) {
    if (!app.have_image || !app.current || !app.image_doc || !app.edit_ctrl ||
        app.benchmark || app.nav_benchmark) {
        return;
    }
    if (edit_transition_blocked(app)) {
        return;  // A pending/in-flight Save owns the document.
    }
    if (app.crop.has_value() && app.crop->active) {
        return;  // Crop mode owns the canvas until applied or cancelled.
    }
    if (app.resize_dlg != nullptr || app.pending_resize.has_value()) {
        return;  // One modal resize transition at a time.
    }
    const auto dims = app.image_doc->current_dimensions();
    if (!dims.has_value() || !dims->is_valid()) {
        return;
    }

    // Presentation-only draft: never touches the edit log.
    const auto preview_cb = [&app](const std::optional<oz::viewer_core::edit_dimensions>& draft) {
        app.draft_resize = draft;
        redraw_current_view(app);
    };

    // Apply phase 1: submit one validation request and return immediately; the
    // dialog stays responsive until the matching completion arrives.
    const auto apply_cb = [&app](HWND dialog, const oz::viewer_core::edit_dimensions& target) {
        if (!submit_resize_validation(app, dialog, target)) {
            // Local state changed under the dialog; report through the same
            // two-phase channel instead of leaving Apply pending forever.
            oz::viewer::notify_resize_dialog_result(
                dialog, false, L"Resize request could not be submitted.");
        }
    };

    oz::viewer::show_resize_dialog(app.hwnd, *dims, preview_cb, apply_cb);

    // Any exit from the dialog invalidates a still-pending request: a late
    // completion can no longer match `pending_resize`, and the presentation
    // draft is always cleared back to the committed canvas.
    app.resize_dlg = nullptr;
    app.pending_resize.reset();
    app.draft_resize.reset();
    update_edit_menu_state(app);
    redraw_current_view(app);
}

// True while a Save is pending/in flight (or its completion has not been
// consumed): conflicting edit/history transitions must not mutate the
// document while the save owns it.
static bool edit_transition_blocked(app_state& app) {
    return app.pending_save.has_value() || (app.save_ctrl && app.save_ctrl->is_active());
}

// Bounded user-facing message for a save failure taxonomy value. `detail` is a
// worker ASCII diagnostic and is truncated before display.
static std::wstring save_error_ui_message(oz::viewer_core::save_error error,
                                          const std::string& detail) {
    const wchar_t* head =
        L"The image could not be saved. The destination was left unchanged.";
    switch (error) {
        case oz::viewer_core::save_error::none:
            return {};
        case oz::viewer_core::save_error::cancelled:
            head = L"The save was cancelled. No file was written.";
            break;
        case oz::viewer_core::save_error::invalid_request:
            head = L"The save request was not valid. No file was written.";
            break;
        case oz::viewer_core::save_error::stale_source:
            head = L"The source image changed while saving. No file was written.";
            break;
        case oz::viewer_core::save_error::destination_conflict:
            head = L"The destination file changed or appeared after the save began. "
                   L"No file was written.";
            break;
        case oz::viewer_core::save_error::materialization_failed:
            head = L"The edited image could not be prepared for saving. No file was written.";
            break;
        case oz::viewer_core::save_error::alpha_not_supported:
            head = L"The selected format does not support transparency.\n\n"
                   L"Use File -> Save As and choose PNG, WebP, or TIFF to keep the alpha channel.";
            break;
        case oz::viewer_core::save_error::encoder_unavailable:
            head = L"The requested image encoder is not available on this system. "
                   L"No file was written.";
            break;
        case oz::viewer_core::save_error::encoder_configuration_failed:
            head = L"The image encoder rejected the requested format or quality. "
                   L"No file was written.";
            break;
        case oz::viewer_core::save_error::encode_failed:
            head = L"Encoding the image failed. No file was written.";
            break;
        case oz::viewer_core::save_error::temp_create_failed:
            head = L"A temporary file could not be created next to the destination. "
                   L"No file was written.";
            break;
        case oz::viewer_core::save_error::flush_failed:
            head = L"The encoded file could not be flushed to disk. No file was written.";
            break;
        case oz::viewer_core::save_error::replace_failed:
            head = L"The encoded file could not be put in place. The destination was left "
                   L"unchanged.";
            break;
        case oz::viewer_core::save_error::commit_state_uncertain:
            head = L"The save could not be confirmed.\n\n"
                   L"The destination may have been partially replaced. The document remains "
                   L"modified; verify the file before continuing.";
            break;
        case oz::viewer_core::save_error::cleanup_failed:
            head = L"The save failed and its temporary file could not be removed.";
            break;
    }
    std::wstring message(head);
    if (!detail.empty()) {
        message += L"\n\n";
        message += widen_bounded_error(detail, L"");
    }
    return message;
}

static bool destination_is_current_source(const app_state& app,
                                          const std::filesystem::path& destination) {
    if (!app.image_doc) {
        return false;
    }
    const auto& source_path = app.image_doc->metadata().source_path;
    return !source_path.empty() && oz::viewer_core::same_file_identity(source_path, destination);
}

// File -> Save (in place) / Save As. Captures the request identity on the UI
// thread and submits exactly one single-flight worker request. Save As is an
// export copy: the document keeps its source path/codec/renderer source and
// remains dirty unless the selected destination resolves to the current
// source file (then it follows Save semantics).
static void start_save(app_state& app, bool save_as) {
    if (!app.have_image || !app.current || !app.image_doc || !app.save_ctrl ||
        app.benchmark || app.nav_benchmark) {
        return;
    }
    if (edit_transition_blocked(app) || app.pending_save.has_value()) {
        return;  // One pending/in-flight save at a time.
    }
    if (app.crop.has_value() && app.crop->active) {
        return;  // Crop mode owns the canvas until applied or cancelled.
    }
    if (app.resize_dlg != nullptr || app.pending_resize.has_value()) {
        return;  // One modal edit transition at a time.
    }

    const auto& metadata = app.image_doc->metadata();
    const oz::viewer_core::save_routing routing = oz::viewer_core::route_save_request(
        metadata.codec, !metadata.source_path.empty(), save_as);
    if (routing.route == oz::viewer_core::save_route::none) {
        return;
    }
    if (routing.route == oz::viewer_core::save_route::save_in_place &&
        !app.image_doc->is_dirty()) {
        return;  // Nothing to write in place.
    }

    std::filesystem::path destination;
    oz::viewer_core::save_format format = routing.format;
    bool same_source = false;

    if (routing.route == oz::viewer_core::save_route::save_in_place) {
        destination = metadata.source_path;
        same_source = true;
    } else {
        const auto save_as_res = oz::viewer::show_save_as_dialog(
            app.hwnd,
            metadata.source_path,
            metadata.codec,
            oz::viewer_core::webp_encoder_available());

        if (!save_as_res.accepted) {
            return;  // User cancelled.
        }

        destination = save_as_res.destination;
        const std::wstring extension = destination.extension().wstring();
        if (!extension.empty()) {
            const auto selected = oz::viewer_core::save_format_for_extension(extension);
            if (!selected.has_value()) {
                static_cast<void>(oz::viewer::show_prompt(
                    app.hwnd,
                    L"OZ Image Viewer",
                    L"Unsupported output extension.",
                    L"Supported formats are PNG (.png), JPEG (.jpg/.jpeg), "
                    L"WebP (.webp), TIFF (.tif/.tiff), and BMP (.bmp).",
                    MB_OK,
                    MB_ICONWARNING));
                return;  // Never write bytes that do not match the typed extension.
            }
            if (*selected == oz::viewer_core::save_format::webp &&
                !oz::viewer_core::webp_encoder_available()) {
                const std::wstring msg = save_error_ui_message(
                    oz::viewer_core::save_error::encoder_unavailable,
                    "WebP encoder is not available in this build");
                static_cast<void>(oz::viewer::show_prompt(app.hwnd, L"OZ Image Viewer", msg.c_str(), nullptr, MB_OK, MB_ICONWARNING));
                return;
            }
            format = *selected;
        } else {
            format = save_as_res.format;
            destination += oz::viewer_core::preferred_extension(format);
        }

        same_source = destination_is_current_source(app, destination);
        if (same_source) {
            if (metadata.codec == oz::viewer_core::codec_id::tiff_wic) {
                static_cast<void>(oz::viewer::show_prompt(
                    app.hwnd,
                    L"OZ Image Viewer",
                    L"The selected destination is the current TIFF source file.",
                    L"TIFF is supported for Save As export copies only. Choose a different file name.",
                    MB_OK,
                    MB_ICONWARNING));
                return;
            }
            const auto source_routing =
                oz::viewer_core::route_save_request(metadata.codec, true, false);
            if (!source_routing.source_supports_in_place || format != source_routing.format) {
                static_cast<void>(oz::viewer::show_prompt(
                    app.hwnd,
                    L"OZ Image Viewer",
                    L"The selected destination is the current source file.",
                    L"Choose a different file name or keep the source format.",
                    MB_OK,
                    MB_ICONWARNING));
                return;
            }
        }
    }

    const oz::viewer_core::file_fingerprint destination_fingerprint =
        oz::viewer_core::query_file_fingerprint(destination);
    if (same_source) {
        if (!destination_fingerprint.exists) {
            static_cast<void>(oz::viewer::show_prompt(
                app.hwnd,
                L"OZ Image Viewer",
                L"The source file no longer exists at its recorded location.",
                L"Use File -> Save As to write a copy.",
                MB_OK,
                MB_ICONERROR));
            return;
        }
        // In-place Save trusted source baseline check: reject if external modification
        // occurred after open/last-save before materializing or overwriting.
        const auto& baseline = app.image_doc->source_baseline();
        if (!oz::viewer_core::save_pre_submit_eligible(baseline, destination_fingerprint)) {
            const std::wstring msg = save_error_ui_message(
                oz::viewer_core::save_error::stale_source,
                "source file modified or inaccessible on disk before saving");
            static_cast<void>(oz::viewer::show_prompt(app.hwnd, L"OZ Image Viewer", msg.c_str(), nullptr, MB_OK, MB_ICONWARNING));
            return;
        }
    }

    oz::viewer::save_work_request request;
    request.document_instance_id = app.image_doc->instance_id();
    request.diagnostic_revision = app.image_doc->edit_revision();
    const auto active_commands = app.image_doc->history().active_commands();
    request.commands.assign(active_commands.begin(), active_commands.end());

    // Immutable canonical source: the retained full-resolution source when it
    // belongs to this document instance; otherwise the worker acquires a fresh
    // full-resolution decode of the original file (source-only upgrade, never a
    // materialized edit result).
    const bool retained_source_matches =
        app.full_res_source && app.full_res_source->is_valid() &&
        app.full_res_source->document_instance_id == app.image_doc->instance_id();
    if (retained_source_matches) {
        request.existing_source = app.full_res_source;
    } else {
        if (metadata.source_path.empty()) {
            static_cast<void>(oz::viewer::show_prompt(
                app.hwnd,
                L"OZ Image Viewer",
                L"No source file is available for this image.",
                L"The image must be opened from a file before it can be saved.",
                MB_OK,
                MB_ICONERROR));
            return;
        }
        const auto acquisition_fingerprint =
            oz::viewer_core::query_file_fingerprint(metadata.source_path);
        if (!acquisition_fingerprint.exists) {
            static_cast<void>(oz::viewer::show_prompt(
                app.hwnd,
                L"OZ Image Viewer",
                L"The source file no longer exists at its recorded location.",
                nullptr,
                MB_OK,
                MB_ICONERROR));
            return;
        }
        const auto& baseline = app.image_doc->source_baseline();
        if (baseline.valid &&
            (acquisition_fingerprint.file_size != baseline.file_size ||
             acquisition_fingerprint.last_write_time != baseline.last_write_time)) {
            const std::wstring msg = save_error_ui_message(
                oz::viewer_core::save_error::stale_source,
                "source file modified or inaccessible on disk before acquisition");
            static_cast<void>(oz::viewer::show_prompt(app.hwnd, L"OZ Image Viewer", msg.c_str(), nullptr, MB_OK, MB_ICONWARNING));
            return;
        }
        request.needs_source_acquisition = true;
        request.source_path = metadata.source_path;
        request.expected_file_size = baseline.valid ? baseline.file_size : acquisition_fingerprint.file_size;
        request.expected_last_write_time = baseline.valid ? baseline.last_write_time : acquisition_fingerprint.last_write_time;
        request.expected_dimensions = metadata.source_dimensions;
    }

    request.request.destination = destination;
    request.request.format = format;
    request.request.jpeg_quality = oz::viewer_core::kDefaultJpegQuality;
    request.request.memory_budget = oz::viewer_core::get_system_edit_admission_budget();
    request.request.destination_baseline.existed = destination_fingerprint.exists;
    request.request.destination_baseline.file_size = destination_fingerprint.file_size;
    request.request.destination_baseline.last_write_time = destination_fingerprint.last_write_time;

    // Captured source identity for worker-side staleness revalidation:
    // uses authoritative document baseline if valid, otherwise existing source identity or current disk.
    const std::filesystem::path fingerprint_path =
        !metadata.source_path.empty()
            ? metadata.source_path
            : (request.existing_source ? request.existing_source->source_path
                                       : std::filesystem::path{});
    if (!fingerprint_path.empty()) {
        const auto& baseline = app.image_doc->source_baseline();
        if (baseline.valid && baseline.path == fingerprint_path) {
            request.request.source_fingerprint.path = fingerprint_path;
            request.request.source_fingerprint.file_size = baseline.file_size;
            request.request.source_fingerprint.last_write_time = baseline.last_write_time;
            request.request.source_fingerprint.dimensions = metadata.source_dimensions;
            request.request.source_fingerprint.expected = true;
        } else if (request.existing_source) {
            request.request.source_fingerprint.path = fingerprint_path;
            request.request.source_fingerprint.file_size = request.existing_source->file_size;
            request.request.source_fingerprint.last_write_time = request.existing_source->last_write_time;
            request.request.source_fingerprint.dimensions = metadata.source_dimensions;
            request.request.source_fingerprint.expected = (request.existing_source->file_size != 0 ||
                                                           request.existing_source->last_write_time != 0);
        } else {
            // Source has no authoritative baseline or retained source buffer.
            // Do not query a fresh unverified filesystem snapshot as an authoritative baseline.
            request.request.source_fingerprint.path = fingerprint_path;
            request.request.source_fingerprint.file_size = metadata.file_size;
            request.request.source_fingerprint.last_write_time = metadata.last_write_time;
            request.request.source_fingerprint.dimensions = metadata.source_dimensions;
            request.request.source_fingerprint.expected = (metadata.file_size != 0 || metadata.last_write_time != 0);
        }
    }

    const std::uint64_t request_id = app.save_ctrl->submit_request(std::move(request));
    if (request_id == 0) {
        return;  // Single-flight guard: another save owns the slot.
    }

    app_state::pending_save_state pending;
    pending.request_id = request_id;
    pending.document_instance_id = app.image_doc->instance_id();
    pending.captured_commands.assign(active_commands.begin(), active_commands.end());
    pending.destination = destination;
    pending.same_source = same_source;
    pending.format = format;
    app.pending_save = std::move(pending);
    update_edit_menu_state(app);
}

// Document-switching actions (open/drop/navigation/close) must not abandon a
// save that could still replace a file. Offer Wait (default) or Cancel of the
// save, then block until the worker completion has been consumed on the UI
// thread. Once the replacement step has begun, cancellation cannot revoke it.
static bool resolve_active_save_for_action(app_state& app) {
    if (app.benchmark || app.nav_benchmark) {
        return true;
    }
    if (!app.save_ctrl || !app.save_ctrl->is_active()) {
        return true;
    }

    const auto prompt_rc = oz::viewer::show_prompt(
        app.hwnd,
        L"OZ Image Viewer",
        L"A save is still in progress.",
        L"OK waits for the save to finish before continuing.\n"
        L"Cancel stops the save. If the replacement step has already begun it cannot be revoked.",
        MB_OKCANCEL,
        MB_ICONWARNING);
    if (prompt_rc == oz::viewer::prompt_result::cancel) {
        app.save_ctrl->request_cancel();
    }

    EnableWindow(app.hwnd, FALSE);
    const ULONGLONG deadline = GetTickCount64() + 60000;
    bool timed_out = false;
    while (app.save_ctrl->is_active()) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(msg.wParam));
                EnableWindow(app.hwnd, TRUE);
                return false;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!app.save_ctrl->is_active()) {
            break;
        }
        if (GetTickCount64() >= deadline) {
            timed_out = true;
            break;
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    EnableWindow(app.hwnd, TRUE);
    SetForegroundWindow(app.hwnd);
    if (timed_out) {
        static_cast<void>(oz::viewer::show_prompt(
            app.hwnd,
            L"OZ Image Viewer",
            L"The save did not finish in time.",
            L"The requested action was not started.",
            MB_OK,
            MB_ICONWARNING));
        return false;
    }
    return true;
}

// kMsgSaveComplete (UI thread): consume the matching completion, promote any
// source-only acquisition, and record the captured active-command snapshot as
// the saved checkpoint only for a matching successful same-source save. A
// Save As copy never rebinds the document and never moves the checkpoint.
static void handle_save_completion(app_state& app, oz::viewer::save_completion_msg& comp) {
    const std::uint64_t pending_request_id =
        app.pending_save.has_value() ? app.pending_save->request_id : 0;
    if (!oz::viewer_core::save_completion_is_for_pending(app.pending_save.has_value(),
                                                         pending_request_id,
                                                         comp.request_id)) {
        return;  // Unknown or already-consumed completion.
    }
    app_state::pending_save_state pending = std::move(*app.pending_save);
    app.pending_save.reset();

    const bool document_matches =
        app.image_doc && comp.document_instance_id == pending.document_instance_id &&
        app.image_doc->instance_id() == pending.document_instance_id;

    // Source-only full-resolution upgrade is independent of the file outcome:
    // the acquired buffer is an immutable decode of the original source, never
    // a materialized edit result.
    if (document_matches && comp.acquired_source && comp.acquired_source->is_valid()) {
        app.full_res_source = comp.acquired_source;
        adopt_acquired_source(app, *comp.acquired_source);
    }

    const std::span<const oz::viewer_core::edit_command> active =
        app.image_doc ? app.image_doc->history().active_commands()
                      : std::span<const oz::viewer_core::edit_command>{};
    const oz::viewer_core::save_completion_policy_input policy_input{
        comp.committed, comp.error, document_matches, pending.same_source,
        active, pending.captured_commands, comp.committed_fingerprint};
    switch (oz::viewer_core::evaluate_save_completion_policy(policy_input)) {
    case oz::viewer_core::save_completion_action::checkpoint:
        app.image_doc->mark_saved_with_baseline(comp.committed_fingerprint.file_size,
                                                comp.committed_fingerprint.last_write_time);
        return;
    case oz::viewer_core::save_completion_action::saved_unverified_baseline:
        static_cast<void>(oz::viewer::show_prompt(
            app.hwnd,
            L"OZ Image Viewer",
            L"The image was saved, but the committed file baseline could not be verified.",
            L"The document remains modified.",
            MB_OK,
            MB_ICONWARNING));
        return;
    case oz::viewer_core::save_completion_action::saved_commands_changed:
        // Defensive: conflicting edits are disabled while saving, so
        // this cannot normally happen. The file was written from the
        // captured snapshot; the document must stay modified.
        static_cast<void>(oz::viewer::show_prompt(
            app.hwnd,
            L"OZ Image Viewer",
            L"The image was saved, but the document changed during the save.",
            L"The document remains modified.",
            MB_OK,
            MB_ICONWARNING));
        return;
    case oz::viewer_core::save_completion_action::saved_silent:
        return;  // Save As copy is silent and remains dirty.
    case oz::viewer_core::save_completion_action::none:
        break;
    }

    if (comp.error == oz::viewer_core::save_error::cancelled) {
        return;  // User-initiated cancellation; no file was written.
    }

    const std::wstring message = save_error_ui_message(comp.error, comp.detail);
    if (!message.empty()) {
        static_cast<void>(oz::viewer::show_prompt(app.hwnd, L"OZ Image Viewer", message.c_str(), nullptr, MB_OK, MB_ICONWARNING));
    }
}
#endif
static void update_open_button_layout(app_state& app) {
    if (!app.btn_open) return;
    if (app.have_image) {
        ShowWindow(app.btn_open, SW_HIDE);
    } else {
        // Ensure the Open button uses a scalable Segoe UI font matching current DPI
        if (app.font_open_btn) {
            DeleteObject(app.font_open_btn);
            app.font_open_btn = nullptr;
        }
        const int font_height = oz::viewer::dip_to_physical_round(13.0f, app.renderer.dpi);
        app.font_open_btn = CreateFontW(
            -font_height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");
        if (app.font_open_btn) {
            SendMessageW(app.btn_open, WM_SETFONT, reinterpret_cast<WPARAM>(app.font_open_btn), TRUE);
        }

        const auto layout = current_ui_layout(app);
        const auto& btn = layout.open_btn_physical;
        if (!btn.is_empty()) {
            SetWindowPos(app.btn_open, nullptr, btn.x, btn.y, btn.width, btn.height, SWP_NOZORDER | SWP_SHOWWINDOW);
        } else {
            ShowWindow(app.btn_open, SW_HIDE);
        }
    }
}

// Toggle borderless fullscreen
static void toggle_fullscreen(app_state& app) {
    if (app.benchmark || app.nav_benchmark) return;
    HWND hwnd = app.hwnd;
    if (!app.is_fullscreen) {
        // Transition to borderless fullscreen on current monitor
        app.saved_style = GetWindowLongW(hwnd, GWL_STYLE);
        app.saved_exstyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
        GetWindowPlacement(hwnd, &app.saved_placement);

        HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(hmon, &mi)) {
            SetWindowLongW(hwnd, GWL_STYLE, app.saved_style & ~(WS_CAPTION | WS_THICKFRAME));
            SetWindowLongW(hwnd, GWL_EXSTYLE, app.saved_exstyle & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
            SetWindowPos(hwnd, HWND_TOP,
                         mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            app.is_fullscreen = true;
        }
    } else {
        // Restore windowed style and placement
        SetWindowLongW(hwnd, GWL_STYLE, app.saved_style);
        SetWindowLongW(hwnd, GWL_EXSTYLE, app.saved_exstyle);
        SetWindowPlacement(hwnd, &app.saved_placement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        app.is_fullscreen = false;
    }

    // Re-layout chrome and canvas for the new fullscreen state
    const auto layout = current_ui_layout(app);
    if (app.chrome) {
        app.chrome->update_layout(layout, app.is_fullscreen);
    }
    if (app.have_image && app.current) {
        const auto dims = active_view_dimensions(app);
        oz::viewer_core::on_viewport_resized(app.view,
                                             dims.width,
                                             dims.height,
                                             0, 0, layout.canvas_physical.width,
                                             layout.canvas_physical.height);
        redraw_current_view(app);
    } else {
        update_open_button_layout(app);
    }
}

// Open File Dialog (Ctrl+O, File->Open, or Open button)
static void start_open_file_dialog(app_state& app) {
    if (app.benchmark || app.nav_benchmark) return;

    if (!check_dirty_discard_allowed(app)) {
        return;
    }

    const auto selected = oz::viewer::show_open_image_dialog(app.hwnd);
    if (selected.has_value()) {
        open_new_document(app, *selected);
    }
}

// Launch start_copy_image
static void start_copy_image(app_state& app) {
    if (app.benchmark || app.nav_benchmark) return;
    if (!app.have_image || !app.current || !app.doc.has_document) {
        return;
    }
#if OZVIEWER_ENABLE_EDITING
    if (app.crop.has_value() && app.crop->active) {
        return;
    }
    if (app.resize_dlg != nullptr || app.pending_resize.has_value()) {
        return;
    }
    if (app.pending_save.has_value() || (app.save_ctrl && app.save_ctrl->is_active())) {
        return;
    }
#endif
    if (app.pending_copy.has_value() || (app.copy_ctrl && app.copy_ctrl->is_active())) {
        return;
    }

    // Lazy initialization of copy_controller on the first valid interactive request
    if (!app.copy_ctrl) {
        app.copy_ctrl = std::make_unique<oz::viewer::copy_controller>();
        app.copy_ctrl->start(app.hwnd, kMsgCopyComplete);
        app.copy_ctrl_started = true;
    }

    oz::viewer::copy_work_request req;
    req.document_generation = app.doc.generation;

#if OZVIEWER_ENABLE_EDITING
    if (app.image_doc) {
        req.document_instance_id = app.image_doc->instance_id();
        req.diagnostic_revision = app.image_doc->edit_revision();

        const bool has_retained_source =
            app.full_res_source && app.full_res_source->is_valid() &&
            app.full_res_source->document_instance_id == app.image_doc->instance_id();

        if (has_retained_source) {
            req.existing_source = app.full_res_source;
        } else {
            // Must acquire stable full-resolution source
            req.needs_source_acquisition = true;
            req.source_path = app.image_doc->metadata().source_path;
            const auto& baseline = app.image_doc->source_baseline();
            if (baseline.valid) {
                req.expected_file_size = baseline.file_size;
                req.expected_last_write_time = baseline.last_write_time;
            } else {
                req.expected_file_size = app.image_doc->metadata().file_size;
                req.expected_last_write_time = app.image_doc->metadata().last_write_time;
            }
            req.expected_dimensions = app.image_doc->metadata().source_dimensions;
        }

        const auto active_cmds = app.image_doc->history().active_commands();
        req.commands.assign(active_cmds.begin(), active_cmds.end());
    } else
#endif
    {
        // Editing OFF: full-resolution source with orientation applied.
        // The document generation is the OFF-mode document identity; it is
        // nonzero and stable for the current document, so the immutable source
        // passes edit_source_buffer::is_valid() without weakening the
        // generation-based stale-completion check.
        req.document_instance_id = app.doc.generation;
        req.diagnostic_revision = 0;

        if (app.current->scale_den == 1) {
            auto buf = std::make_shared<oz::viewer::edit_source_buffer>();
            buf->document_instance_id = req.document_instance_id;
            buf->source_path = app.doc.active_path;
            buf->dimensions = {app.current->width, app.current->height};
            buf->stride = app.current->stride;
            buf->pixels = app.current->pixels;
            req.existing_source = std::move(buf);
        } else {
            req.needs_source_acquisition = true;
            req.source_path = app.doc.active_path;
        }
    }

    // The Editing-OFF copy path never submits edit commands, so the worker
    // never consumes the budget there; keep the value zero to avoid linking
    // the edit-pipeline budget helper into Editing-OFF builds.
#if OZVIEWER_ENABLE_EDITING
    req.memory_budget = oz::viewer_core::get_system_edit_admission_budget();
#else
    req.memory_budget = 0;
#endif

    app_state::pending_copy_state pending;
    pending.document_instance_id = req.document_instance_id;
    pending.diagnostic_revision = req.diagnostic_revision;
    pending.document_generation = app.doc.generation;
    pending.request_id = app.copy_ctrl->submit_request(std::move(req));
    app.pending_copy = pending;

    update_edit_menu_state(app);
}

// Stage 9.5: Paste Image from the Windows clipboard (interactive Ctrl+V).
// Reads a valid clipboard DIB-family image and opens it as an in-memory
// temporary document. Any clipboard or bitmap failure is quiet: no prompt,
// no partial document, current state untouched. Benchmark and
// navigation-benchmark modes never reach here (guarded at every caller).
static void start_paste_from_clipboard(app_state& app) {
    if (app.benchmark || app.nav_benchmark) return;
#if OZVIEWER_ENABLE_EDITING
    if (app.crop.has_value() && app.crop->active) {
        return;
    }
    if (app.resize_dlg != nullptr || app.pending_resize.has_value()) {
        return;
    }
    if (app.pending_save.has_value() || (app.save_ctrl && app.save_ctrl->is_active())) {
        return;
    }
#endif
    // A pending/in-flight Copy owns the clipboard handoff; pasting now would
    // orphan its generation without user-visible benefit, so wait quietly.
    if (app.pending_copy.has_value() || (app.copy_ctrl && app.copy_ctrl->is_active())) {
        return;
    }

    oz::viewer::clipboard_bitmap bitmap;
    const auto res = oz::viewer::read_clipboard_image(app.hwnd, bitmap);
    if (!res.ok() || !bitmap.is_valid()) {
        return;  // Locked/absent/invalid/unsupported clipboard: fail quietly.
    }

    if (!check_dirty_discard_allowed(app)) {
        return;  // User kept the current dirty document; nothing changes.
    }

    open_pasted_document(app, std::move(bitmap));
}

// Commit an already-validated clipboard bitmap as the active temporary
// document. The complete install - display image, pathless domain document,
// and the payload-sized retained source used by Copy Image - is staged by
// prepare_paste_install BEFORE any state mutation, so a clipboard-payload
// allocation fault returns quietly with the prior display/document/view/flags
// untouched. The commit then bumps the document generation first so in-flight
// file workers (initial decode, enumeration, navigation, copy) can never
// commit stale file state over the pasted pixels. The temporary document
// carries no source file path: file-only commands stay disabled via
// has_file_path, while Copy Image remains functional through the retained
// source.
static bool open_pasted_document(app_state& app, oz::viewer::clipboard_bitmap&& bitmap) {
    // Phase 1 (allocation-safe staging): every allocation-capable artifact of
    // the temporary document install is built here, including the pixel copy
    // into the retained source. Failure is quiet: no UI/document/presentation
    // state has been touched yet.
    oz::viewer::paste_install install;
    if (!oz::viewer::prepare_paste_install(std::move(bitmap), install)) {
        return false;
    }
    const auto& img = install.image;

    // Phase 2 (commit): no-throw moves/swaps and bool-signaled presentation
    // only; the remaining allocation-capable work already happened above.
    // Invalidate the old generation so any in-flight workers won't commit.
    const std::uint64_t new_gen = app.doc.next_generation();
    (void)new_gen;

    // Cooperative cancel of any pending background file work.
    app.cancel_initial_decode.store(true, std::memory_order_relaxed);
    app.cancel_enum.store(true, std::memory_order_relaxed);
    app.work_queue.replace_plan(++app.current_epoch, {});

    // Clear initial decode cancellation for future file opens.
    app.cancel_initial_decode.store(false, std::memory_order_relaxed);

    const auto layout = current_ui_layout(app);
    const auto old_view = app.view;
    oz::viewer_core::set_fit(app.view, img->src_width, img->src_height,
                             layout.canvas_physical.width, layout.canvas_physical.height);

    auto prev_image = app.current;
    const bool prev_have = app.have_image;
#if OZVIEWER_ENABLE_EDITING
    const bool old_suppress_edit_presentation = app.suppress_edit_presentation;
    app.suppress_edit_presentation = true;
#endif
    app.current = img;
    app.have_image = true;

    if (!upload_and_present(app)) {
        if (!recover_and_present(app)) {
            // Restore the prior image and its view; never leave a half-presented document.
            app.current = prev_image;
            app.have_image = prev_have;
            app.view = old_view;
#if OZVIEWER_ENABLE_EDITING
            app.suppress_edit_presentation = old_suppress_edit_presentation;
#endif
            if (app.have_image) {
                redraw_current_view(app);
            } else {
                oz::viewer::render_blank(app.renderer);
                update_open_button_layout(app);
            }
            return false;
        }
    }

#if OZVIEWER_ENABLE_EDITING
    app.suppress_edit_presentation = old_suppress_edit_presentation;
#endif

    // Successful commit: temporary document with no source file path and no
    // file navigation siblings (enumeration never runs for clipboard docs).
    app.doc.has_document = true;
    app.doc.active_path.clear();
    app.nav = oz::viewer_core::navigation_list{};
    app.desired_index = 0;
    app.committed_index = 0;

#if OZVIEWER_ENABLE_EDITING
    // The staged document and retained source become current together. Both
    // are no-throw moves, so the successful presentation above can never be
    // followed by a clipboard-driven allocation fault: the source pixels were
    // already copied into install.source before the first mutation.
    app.image_doc = std::move(install.document);
    app.full_res_source = std::move(install.source);
#endif

    update_edit_menu_state(app);
    update_open_button_layout(app);
    return true;
}

// Open a new document replacing the current image or no-image state.
static void open_new_document(app_state& app, const std::filesystem::path& path) {
    // Invalidate old generation so any in-flight workers won't commit
    const std::uint64_t new_gen = app.doc.next_generation();

    // Cooperative cancel of any pending background work
    app.cancel_initial_decode.store(true, std::memory_order_relaxed);
    app.cancel_enum.store(true, std::memory_order_relaxed);
    app.work_queue.replace_plan(++app.current_epoch, {});

    // Clear initial decode cancellation for the new task
    app.cancel_initial_decode.store(false, std::memory_order_relaxed);

    const auto layout = current_ui_layout(app);
    const int view_w = layout.canvas_physical.width;
    const int view_h = layout.canvas_physical.height;

    // Launch initial decode worker for the new document
    // We retain the existing image until this worker successfully decodes and presents!
    app.initial_decode_thread = std::jthread(initial_decode_worker_func, std::ref(app), path,
                                             view_w, view_h, new_gen);
}

// --- window procedure -------------------------------------------------------

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    app_state* app = reinterpret_cast<app_state*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return 0;
        }
        case WM_COMMAND: {
            if (!app) return 0;
            const int id = LOWORD(wparam);
            if (id == kIdFileOpen || id == kIdBtnOpen) {
                start_open_file_dialog(*app);
                return 0;
            }
            if (id == kIdFileCopyImage) {
                if (!noninteractive_mode(*app)) {
                    const auto ctx = current_ui_command_context(*app);
                    const auto st = oz::viewer::derive_command_state(oz::viewer::ui_command_id::file_copy_image, ctx);
                    if (st.enabled) {
                        start_copy_image(*app);
                    }
                }
                return 0;
            }
            if (id == kIdFileOpenLocation) {
                if (!noninteractive_mode(*app)) {
                    const auto ctx = current_ui_command_context(*app);
                    const auto st = oz::viewer::derive_command_state(oz::viewer::ui_command_id::file_open_location, ctx);
                    if (st.enabled && !app->doc.active_path.empty()) {
                        const auto res = oz::viewer::reveal_file_in_explorer(app->doc.active_path);
                        if (!res.ok() && !noninteractive_mode(*app)) {
                            static_cast<void>(oz::viewer::show_prompt(
                                app->hwnd,
                                L"Open File Location",
                                L"Unable to reveal file in Explorer.",
                                L"The file may have been moved, renamed, or deleted.",
                                MB_OK,
                                MB_ICONWARNING));
                        }
                    }
                }
                return 0;
            }
            if (id == kIdFileCopy) {
                if (!noninteractive_mode(*app)) {
                    const auto ctx = current_ui_command_context(*app);
                    const auto st = oz::viewer::derive_command_state(oz::viewer::ui_command_id::file_copy, ctx);
                    if (st.enabled && !app->doc.active_path.empty()) {
                        const auto res = oz::viewer::copy_file_to_clipboard(app->hwnd, app->doc.active_path);
                        if (!res.ok() && !noninteractive_mode(*app)) {
                            static_cast<void>(oz::viewer::show_prompt(
                                app->hwnd,
                                L"Copy File",
                                L"Unable to copy file to the clipboard.",
                                L"The file may have been moved, renamed, deleted, or the clipboard could not be opened.",
                                MB_OK,
                                MB_ICONWARNING));
                        }
                    }
                }
                return 0;
            }
            if (id == kIdFileCopyPath) {
                if (!noninteractive_mode(*app)) {
                    const auto ctx = current_ui_command_context(*app);
                    const auto st = oz::viewer::derive_command_state(oz::viewer::ui_command_id::file_copy_path, ctx);
                    if (st.enabled && !app->doc.active_path.empty()) {
                        const auto res = oz::viewer::copy_file_path_to_clipboard(app->hwnd, app->doc.active_path);
                        if (!res.ok() && !noninteractive_mode(*app)) {
                            static_cast<void>(oz::viewer::show_prompt(
                                app->hwnd,
                                L"Copy File Path",
                                L"Unable to copy file path to the clipboard.",
                                L"The file may have been moved, renamed, deleted, or the clipboard could not be opened.",
                                MB_OK,
                                MB_ICONWARNING));
                        }
                    }
                }
                return 0;
            }
            if (id == 302) { // View Fit
                if (app->have_image && app->current) {
                    const auto layout = current_ui_layout(*app);
                    const auto dims = active_view_dimensions(*app);
                    oz::viewer_core::set_fit(app->view, dims.width, dims.height, layout.canvas_physical.width, layout.canvas_physical.height);
                    redraw_current_view(*app);
                    update_edit_menu_state(*app);
                }
                return 0;
            }
            if (id == 301) { // View 1:1
                if (app->have_image && app->current) {
                    const auto layout = current_ui_layout(*app);
                    const auto dims = active_view_dimensions(*app);
                    oz::viewer_core::set_one_to_one(app->view, dims.width, dims.height, layout.canvas_physical.width, layout.canvas_physical.height);
                    redraw_current_view(*app);
                    update_edit_menu_state(*app);
                }
                return 0;
            }
            if (id == 309) { // Fullscreen
                toggle_fullscreen(*app);
                return 0;
            }
#if OZVIEWER_ENABLE_EDITING
            if (id == kIdFileSave) {
                start_save(*app, false);
                return 0;
            }
            if (id == kIdFileSaveAs) {
                start_save(*app, true);
                return 0;
            }
            if (id == kIdEditRotateLeft) {
                apply_edit_command(*app, oz::viewer_core::rotate_command{oz::viewer_core::rotate_direction::left_90});
                return 0;
            }
            if (id == kIdEditRotateRight) {
                apply_edit_command(*app, oz::viewer_core::rotate_command{oz::viewer_core::rotate_direction::right_90});
                return 0;
            }
            if (id == kIdEditFlipH) {
                apply_edit_command(*app, oz::viewer_core::flip_command{oz::viewer_core::flip_direction::horizontal});
                return 0;
            }
            if (id == kIdEditFlipV) {
                apply_edit_command(*app, oz::viewer_core::flip_command{oz::viewer_core::flip_direction::vertical});
                return 0;
            }
            if (id == kIdEditUndo) {
                apply_undo(*app);
                return 0;
            }
            if (id == kIdEditRedo) {
                apply_redo(*app);
                return 0;
            }
            if (id == kIdEditCrop) {
                begin_crop_mode(*app);
                return 0;
            }
            if (id == kIdEditResize) {
                open_resize_modal_dialog(*app);
                return 0;
            }
            if (id == kIdEditCropApply) {
                apply_crop_mode(*app);
                return 0;
            }
            if (id == kIdEditCropCancel) {
                cancel_crop_mode(*app);
                return 0;
            }
            if (id == oz::viewer::kIdCropStripBtnReset) {
                reset_crop_mode(*app);
                return 0;
            }
            if (id == oz::viewer::kIdCropStripBtnApply) {
                apply_crop_mode(*app);
                return 0;
            }
            if (id == oz::viewer::kIdCropStripBtnCancel) {
                cancel_crop_mode(*app);
                return 0;
            }
            if (id == oz::viewer::kIdCropStripComboRatio && HIWORD(wparam) == CBN_SELCHANGE) {
                if (app->crop_strip_ui && app->crop.has_value() && app->crop->active) {
                    const LRESULT cur_sel = SendMessageW(reinterpret_cast<HWND>(lparam), CB_GETCURSEL, 0, 0);
                    if (cur_sel >= 0 && cur_sel < 9) {
                        static constexpr oz::viewer_core::crop_ratio_preset kPresets[] = {
                            oz::viewer_core::crop_ratio_preset::free,
                            oz::viewer_core::crop_ratio_preset::original,
                            oz::viewer_core::crop_ratio_preset::square_1x1,
                            oz::viewer_core::crop_ratio_preset::ratio_3x2,
                            oz::viewer_core::crop_ratio_preset::ratio_4x3,
                            oz::viewer_core::crop_ratio_preset::ratio_5x4,
                            oz::viewer_core::crop_ratio_preset::ratio_16x9,
                            oz::viewer_core::crop_ratio_preset::ratio_9x16,
                            oz::viewer_core::crop_ratio_preset::a_series
                        };
                        set_crop_ratio(*app, kPresets[cur_sel]);
                    }
                }
                return 0;
            }
            switch (id) {
                case kIdCropRatioFree:
                    set_crop_ratio(*app, oz::viewer_core::crop_ratio_preset::free); break;
                case kIdCropRatioOriginal:
                    set_crop_ratio(*app, oz::viewer_core::crop_ratio_preset::original); break;
                case kIdCropRatio1x1:
                    set_crop_ratio(*app, oz::viewer_core::crop_ratio_preset::square_1x1); break;
                case kIdCropRatio3x2:
                    set_crop_ratio(*app, oz::viewer_core::crop_ratio_preset::ratio_3x2); break;
                case kIdCropRatio4x3:
                    set_crop_ratio(*app, oz::viewer_core::crop_ratio_preset::ratio_4x3); break;
                case kIdCropRatio5x4:
                    set_crop_ratio(*app, oz::viewer_core::crop_ratio_preset::ratio_5x4); break;
                case kIdCropRatio16x9:
                    set_crop_ratio(*app, oz::viewer_core::crop_ratio_preset::ratio_16x9); break;
                case kIdCropRatio9x16:
                    set_crop_ratio(*app, oz::viewer_core::crop_ratio_preset::ratio_9x16); break;
                case kIdCropRatioASeries:
                    set_crop_ratio(*app, oz::viewer_core::crop_ratio_preset::a_series); break;
                default:
                    break;
            }
            if (id >= kIdCropRatioFree && id <= kIdCropRatioASeries) {
                return 0;
            }
#endif
            break;
        }
        case WM_DROPFILES: {
            if (!app || app->benchmark || app->nav_benchmark) {
                return 0;
            }
            if (!check_dirty_discard_allowed(*app)) {
                DragFinish(reinterpret_cast<HDROP>(wparam));
                return 0;
            }
            HDROP hDrop = reinterpret_cast<HDROP>(wparam);
            const UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
            if (count == 1) {
                wchar_t path_buf[32768]{};
                if (DragQueryFileW(hDrop, 0, path_buf, static_cast<UINT>(std::size(path_buf))) > 0) {
                    open_new_document(*app, std::filesystem::path(path_buf));
                }
            }
            // Safely finish drop and ignore multiple or 0 items
            DragFinish(hDrop);
            return 0;
        }
        case WM_SIZE: {
            if (app && app->renderer.swap_chain) {
                const UINT w = LOWORD(lparam);
                const UINT h = HIWORD(lparam);
                oz::viewer::resize_renderer(app->renderer, w, h);
                const auto layout = current_ui_layout(*app);
                if (app->chrome) {
                    app->chrome->update_layout(layout, app->is_fullscreen);
                }
#if OZVIEWER_ENABLE_EDITING
                if (app->crop_strip_ui) {
                    const bool crop_active = app->crop.has_value() && app->crop->active;
                    app->crop_strip_ui->update_layout(layout, crop_active);
                }
#endif
                if (app->have_image && app->current) {
                    const auto dims = active_view_dimensions(*app);
                    oz::viewer_core::on_viewport_resized(app->view,
                                                         dims.width,
                                                         dims.height,
                                                         0, 0, layout.canvas_physical.width,
                                                         layout.canvas_physical.height);
                    redraw_current_view(*app);
                } else {
                    oz::viewer::render_blank(app->renderer);
                    update_open_button_layout(*app);
                }
            }
            return 0;
        }
        case WM_DPICHANGED: {
            if (app && app->renderer.swap_chain) {
                const float new_dpi = static_cast<float>(LOWORD(wparam));
                oz::viewer::set_renderer_dpi(app->renderer, new_dpi);
                if (app->chrome) {
                    app->chrome->set_dpi(new_dpi);
                    app->chrome->update_icons(app->renderer.d2d_context.Get());
                }
#if OZVIEWER_ENABLE_EDITING
                if (app->crop_strip_ui) {
                    app->crop_strip_ui->set_dpi(new_dpi);
                }
#endif
                const RECT* rc = reinterpret_cast<const RECT*>(lparam);
                SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left,
                             rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED: {
            if (app && app->chrome) {
                const auto mode = oz::viewer::query_system_theme_mode();
                app->chrome->set_theme(mode);
                app->chrome->update_icons(app->renderer.d2d_context.Get());
#if OZVIEWER_ENABLE_EDITING
                if (app->crop_strip_ui) {
                    app->crop_strip_ui->set_theme(mode);
                }
#endif
                redraw_current_view(*app);
            }
            return 0;
        }
        case WM_NOTIFY: {
            if (app && app->chrome) {
                return app->chrome->handle_notify(lparam);
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            if (app) {
                HDC hdc = reinterpret_cast<HDC>(wparam);
                HWND ctrl = reinterpret_cast<HWND>(lparam);
#if OZVIEWER_ENABLE_EDITING
                if (app->crop_strip_ui) {
                    HBRUSH br = app->crop_strip_ui->handle_ctl_color(hdc, ctrl);
                    if (br) return reinterpret_cast<INT_PTR>(br);
                }
#endif
                if (app->chrome) {
                    HBRUSH br = app->chrome->handle_ctl_color(hdc, ctrl);
                    if (br) return reinterpret_cast<INT_PTR>(br);
                }
            }
            break;
        }
        case WM_KEYDOWN: {
            if (!app) return 0;

            const bool ctrl_down = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift_down = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool alt_down = (GetKeyState(VK_MENU) & 0x8000) != 0;
            const HWND focused = GetFocus();
#if OZVIEWER_ENABLE_EDITING
            const bool crop_active = app->crop.has_value() && app->crop->active;

            // Crop mode has its own small modal command set. It takes Esc
            // precedence over fullscreen so Escape never abandons a draft.
            if (crop_active) {
                if (wparam == VK_ESCAPE) {
                    cancel_crop_mode(*app);
                    return 0;
                }
                if (wparam == VK_RETURN) {
                    apply_crop_mode(*app);
                    return 0;
                }
                if (wparam == 'R' && !ctrl_down && !shift_down) {
                    reset_crop_mode(*app);
                    return 0;
                }
                // Do not let navigation/edit transforms alter the document
                // behind an in-progress crop draft.
                if (wparam == 'C' || (ctrl_down && (wparam == 'L' || wparam == 'R' ||
                    wparam == 'Z' || wparam == 'Y' || wparam == 'H' || wparam == 'V' ||
                    wparam == 'S'))) {
                    return 0;
                }
            }
#else
            const bool crop_active = false; // No crop mode when editing is compiled out
#endif

            // File -> Save (Ctrl+S) / Save As (Ctrl+Shift+S). A pending/in-flight
            // save is single-flight; edit transitions are blocked while active.
#if OZVIEWER_ENABLE_EDITING
            {
                const bool ctrl_down_for_save = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                const bool shift_down_for_save = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                const bool alt_down_for_save = (GetKeyState(VK_MENU) & 0x8000) != 0;
                if (wparam == 'S' && ctrl_down_for_save && !alt_down_for_save) {
                    start_save(*app, shift_down_for_save);
                    return 0;
                }
            }
#endif

            // F11: Fullscreen toggle (in interactive mode)
            if (wparam == VK_F11) {
                toggle_fullscreen(*app);
                return 0;
            }

            // Esc: exits fullscreen only
            if (wparam == VK_ESCAPE) {
                if (app->is_fullscreen) {
                    toggle_fullscreen(*app);
                }
                return 0;
            }

            // Copy Image: Ctrl+C (evaluated via pure shortcut routing helper)
            if (wparam == 'C' || wparam == 'c') {
                const auto ctx = current_ui_command_context(*app);
                const auto copy_img_st = oz::viewer::derive_command_state(oz::viewer::ui_command_id::file_copy_image, ctx);
                HWND resize_hwnd = nullptr;
#if OZVIEWER_ENABLE_EDITING
                resize_hwnd = app->resize_dlg;
#endif
                const auto action = oz::viewer::route_shortcut_key(
                    wparam, ctrl_down, shift_down, alt_down,
                    focused, resize_hwnd, crop_active, copy_img_st);

                if (action == oz::viewer::shortcut_action::file_copy_image) {
                    start_copy_image(*app);
                    return 0;
                } else if (action == oz::viewer::shortcut_action::crop_consume ||
                           action == oz::viewer::shortcut_action::command_disabled) {
                    return 0; // Consumed without action
                }
            }

            // Paste Image from clipboard: Ctrl+V (evaluated via pure shortcut routing helper)
            if (wparam == 'V' || wparam == 'v') {
                if (!noninteractive_mode(*app)) {
                    const auto ctx = current_ui_command_context(*app);
                    const auto copy_img_st = oz::viewer::derive_command_state(oz::viewer::ui_command_id::file_copy_image, ctx);
                    HWND resize_hwnd = nullptr;
#if OZVIEWER_ENABLE_EDITING
                    resize_hwnd = app->resize_dlg;
#endif
                    const auto action = oz::viewer::route_shortcut_key(
                        wparam, ctrl_down, shift_down, alt_down,
                        focused, resize_hwnd, crop_active, copy_img_st);

                    if (action == oz::viewer::shortcut_action::file_paste_image) {
                        start_paste_from_clipboard(*app);
                        return 0;
                    } else if (action == oz::viewer::shortcut_action::crop_consume ||
                               action == oz::viewer::shortcut_action::command_disabled) {
                        return 0; // Consumed without action
                    }
                    // native_text_paste / dialog_default fall through to normal dispatch.
                }
            }

            // Ctrl+O: Open file dialog
            if (wparam == 'O' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                start_open_file_dialog(*app);
                return 0;
            }

#if OZVIEWER_ENABLE_EDITING
            // Resize dialog: Ctrl+Alt+R. Checked before Ctrl+R so Rotate Right
            // never shadows the dialog shortcut.
            if (wparam == 'R' && ctrl_down && alt_down) {
                open_resize_modal_dialog(*app);
                return 0;
            }
            // Rotate Left: Ctrl+L
            if (wparam == 'L' && ctrl_down && !shift_down && !alt_down) {
                apply_edit_command(*app, oz::viewer_core::rotate_command{oz::viewer_core::rotate_direction::left_90});
                return 0;
            }
            // Rotate Right: Ctrl+R
            if (wparam == 'R' && ctrl_down && !shift_down && !alt_down) {
                apply_edit_command(*app, oz::viewer_core::rotate_command{oz::viewer_core::rotate_direction::right_90});
                return 0;
            }
            // Flip Horizontal: Ctrl+Shift+H
            if (wparam == 'H' && ctrl_down && shift_down) {
                apply_edit_command(*app, oz::viewer_core::flip_command{oz::viewer_core::flip_direction::horizontal});
                return 0;
            }
            // Flip Vertical: Ctrl+Shift+V
            if (wparam == 'V' && ctrl_down && shift_down) {
                apply_edit_command(*app, oz::viewer_core::flip_command{oz::viewer_core::flip_direction::vertical});
                return 0;
            }
            // Undo: Ctrl+Z
            if (wparam == 'Z' && ctrl_down && !shift_down) {
                apply_undo(*app);
                return 0;
            }
            // Redo: Ctrl+Y
            if (wparam == 'Y' && ctrl_down && !shift_down) {
                apply_redo(*app);
                return 0;
            }
            // Enter crop mode: C (without a modifier). Crop mode itself owns
            // Enter/Esc/R and disables transform/history commands.
            if (wparam == 'C' && !ctrl_down && !shift_down) {
                begin_crop_mode(*app);
                return 0;
            }
#endif

            // If benchmark or no image, navigation/zoom keys do not apply
            if (app->benchmark || !app->have_image || !app->current) {
                return 0;
            }

            const auto layout = current_ui_layout(*app);
            const int view_w = layout.canvas_physical.width;
            const int view_h = layout.canvas_physical.height;
            const auto dims = active_view_dimensions(*app);

            // 'F' or 'f': Fit
            if (wparam == 'F') {
                oz::viewer_core::set_fit(app->view, dims.width, dims.height, view_w, view_h);
                redraw_current_view(*app);
                return 0;
            }

            // '1': 100%
            if (wparam == '1') {
                oz::viewer_core::set_one_to_one(app->view, dims.width, dims.height, view_w, view_h);
                redraw_current_view(*app);
                return 0;
            }

            // '+' or '=' or VK_OEM_PLUS or VK_ADD: zoom in
            if (wparam == VK_OEM_PLUS || wparam == VK_ADD || wparam == VK_RETURN) {
                const double cx = view_w * 0.5;
                const double cy = view_h * 0.5;
                oz::viewer_core::zoom_at_point(app->view, dims.width, dims.height, view_w, view_h, cx, cy, 1.0);
                redraw_current_view(*app);
                return 0;
            }

            // '-' or VK_OEM_MINUS or VK_SUBTRACT: zoom out
            if (wparam == VK_OEM_MINUS || wparam == VK_SUBTRACT) {
                const double cx = view_w * 0.5;
                const double cy = view_h * 0.5;
                oz::viewer_core::zoom_at_point(app->view, dims.width, dims.height, view_w, view_h, cx, cy, -1.0);
                redraw_current_view(*app);
                return 0;
            }

            // Folder navigation keys (arrow keys, Backspace, Space, Home, End)
            if (!app->nav.empty() && app->first_present_done) {
                execute_navigation_request(*app, wparam, "interactive");
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (!app || !app->have_image || !app->current) return 0;
            const short zDelta = GET_WHEEL_DELTA_WPARAM(wparam);
            if (zDelta == 0) return 0;

            POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd, &pt);

            const auto layout = current_ui_layout(*app);
            const int view_w = layout.canvas_physical.width;
            const int view_h = layout.canvas_physical.height;
            const auto dims = active_view_dimensions(*app);

            const double anchor_x = static_cast<double>(pt.x - layout.canvas_physical.x);
            const double anchor_y = static_cast<double>(pt.y - layout.canvas_physical.y);

            // Steps can be fractional for high-res wheels
            const double delta_steps = static_cast<double>(zDelta) / static_cast<double>(WHEEL_DELTA);
            oz::viewer_core::zoom_at_point(app->view, dims.width, dims.height,
                                          view_w, view_h, anchor_x, anchor_y,
                                          delta_steps);
            redraw_current_view(*app);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (!app || !app->have_image || !app->current) return 0;
#if OZVIEWER_ENABLE_EDITING
            if (app->crop.has_value() && app->crop->active) {
                double crop_x = 0.0;
                double crop_y = 0.0;
                double handle_radius = 0.0;
                if (crop_canvas_point_for_client(*app, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam),
                                                 crop_x, crop_y, handle_radius) &&
                    oz::viewer_core::begin_crop_drag(*app->crop, crop_x, crop_y, handle_radius)) {
                    SetCapture(hwnd);
                    redraw_current_view(*app);
                }
                return 0;
            }
#endif
            SetCapture(hwnd);
            app->is_panning = true;
            app->last_mouse_pos.x = GET_X_LPARAM(lparam);
            app->last_mouse_pos.y = GET_Y_LPARAM(lparam);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!app || !app->have_image || !app->current) return 0;
#if OZVIEWER_ENABLE_EDITING
            if (app->crop.has_value() && app->crop->active &&
                app->crop->drag != oz::viewer_core::crop_drag_target::none) {
                double crop_x = 0.0;
                double crop_y = 0.0;
                double ignored_handle_radius = 0.0;
                if (crop_canvas_point_for_client(*app, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam),
                                                 crop_x, crop_y, ignored_handle_radius) &&
                    oz::viewer_core::update_crop_drag(*app->crop, crop_x, crop_y)) {
                    if (app->crop_strip_ui && app->crop_strip_ui->is_created()) {
                        app->crop_strip_ui->sync_dimension_summary(&*app->crop);
                    }
                    redraw_current_view(*app);
                }
                return 0;
            }
#endif
            if (!app->is_panning) return 0;
            const int cur_x = GET_X_LPARAM(lparam);
            const int cur_y = GET_Y_LPARAM(lparam);
            const double dx = static_cast<double>(cur_x - app->last_mouse_pos.x);
            const double dy = static_cast<double>(cur_y - app->last_mouse_pos.y);
            app->last_mouse_pos.x = cur_x;
            app->last_mouse_pos.y = cur_y;

            const auto layout = current_ui_layout(*app);
            const int view_w = layout.canvas_physical.width;
            const int view_h = layout.canvas_physical.height;
            const auto dims = active_view_dimensions(*app);

            oz::viewer_core::apply_pan_delta(app->view, dims.width, dims.height,
                                             view_w, view_h, dx, dy);
            redraw_current_view(*app);
            return 0;
        }
        case WM_LBUTTONUP: {
#if OZVIEWER_ENABLE_EDITING
            if (app && app->crop.has_value() && app->crop->active) {
                oz::viewer_core::end_crop_drag(*app->crop);
                // Sync the crop strip back to the authoritative selection after
                // the accepted drag end (WM_MOUSEMOVE only refreshes during the
                // drag; a button-up or capture-loss can leave the summary stale).
                if (app->crop_strip_ui && app->crop_strip_ui->is_created()) {
                    app->crop_strip_ui->sync_dimension_summary(&*app->crop);
                }
                if (GetCapture() == hwnd) {
                    ReleaseCapture();
                }
                return 0;
            }
#endif
            if (app && app->is_panning) {
                app->is_panning = false;
                ReleaseCapture();
            }
            return 0;
        }
        case WM_CAPTURECHANGED: {
            if (app) {
                app->is_panning = false;
#if OZVIEWER_ENABLE_EDITING
                if (app->crop.has_value()) {
                    oz::viewer_core::end_crop_drag(*app->crop);
                    // Capture can also be lost without a preceding drag (e.g. an
                    // alt-tab or focus steal); re-sync the crop strip summary so
                    // it never displays a stale authoritative dimension.
                    if (app->crop_strip_ui && app->crop_strip_ui->is_created()) {
                        app->crop_strip_ui->sync_dimension_summary(&*app->crop);
                    }
                }
#endif
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            if (!app || !app->have_image || !app->current) return 0;
#if OZVIEWER_ENABLE_EDITING
            if (app->crop.has_value() && app->crop->active) {
                return 0;
            }
#endif
            const auto layout = current_ui_layout(*app);
            const int view_w = layout.canvas_physical.width;
            const int view_h = layout.canvas_physical.height;
            const auto dims = active_view_dimensions(*app);

            const double anchor_x = static_cast<double>(GET_X_LPARAM(lparam) - layout.canvas_physical.x);
            const double anchor_y = static_cast<double>(GET_Y_LPARAM(lparam) - layout.canvas_physical.y);

            oz::viewer_core::toggle_fit_or_one(app->view, dims.width, dims.height,
                                               view_w, view_h, anchor_x, anchor_y);
            redraw_current_view(*app);
            return 0;
        }
        case kMsgEnumComplete: {
            if (!app) {
                return 0;
            }
            std::unique_ptr<oz::viewer_core::navigation_list> list;
            std::uint64_t enum_gen = 0;
            {
                std::lock_guard<std::mutex> lock(app->enum_mutex);
                list = std::move(app->enum_result);
                enum_gen = app->enum_generation;
            }
            // Reject stale enumeration from an older document generation
            if (enum_gen != app->doc.generation) {
                return 0;
            }
            if (list) {
                app->nav = std::move(*list);
                if (app->nav.active_index().has_value()) {
                    app->desired_index = *app->nav.active_index();
                    app->committed_index = app->desired_index;
                }
                if (!app->persistent_worker_started && !app->benchmark) {
                    app->persistent_worker_started = true;
                    app->persistent_worker_thread = std::jthread(persistent_worker_func, std::ref(*app));
                    schedule_navigation_plan(*app);

                    if (app->nav_benchmark) {
                        SetTimer(hwnd, kNavBenchTimerId, 16, nullptr);
                    }
                } else if (app->persistent_worker_started && !app->benchmark) {
                    schedule_navigation_plan(*app);
                }
            }
            return 0;
        }
        case kMsgDecodeComplete: {
            if (!app) {
                return 0;
            }

            // 1. Initial / new document decode completion
            std::shared_ptr<initial_decode_outcome> outcome;
            {
                std::lock_guard<std::mutex> lock(app->initial_decode_mutex);
                outcome = app->initial_result;
            }
            if (outcome) {
                // Reject stale completion from an older document generation
                if (outcome->generation != app->doc.generation) {
                    return 0;
                }
                // Clear out initial_result slot
                {
                    std::lock_guard<std::mutex> lock(app->initial_decode_mutex);
                    app->initial_result.reset();
                }

                if (!outcome->ok) {
                    if (noninteractive_mode(*app)) {
                        std::fprintf(stderr, "ozviewer: decode failed: %s\n", outcome->detail.c_str());
                        PostQuitMessage(1);
                        return 0;
                    } else {
                        static_cast<void>(oz::viewer::show_prompt(
                            hwnd,
                            L"OZ Image Viewer",
                            L"Unable to open image.",
                            L"Unsupported or corrupted image format.",
                            MB_OK,
                            MB_ICONERROR));
                        // If we already had an image, keep it! Do not quit or destroy existing state.
                        if (!app->have_image) {
                            // Show blank with open button
                            oz::viewer::render_blank(app->renderer);
                            update_open_button_layout(*app);
                        }
                        return 0;
                    }
                }

                // Stage 4 ViewState initialization
                const auto layout = current_ui_layout(*app);
                oz::viewer_core::set_fit(app->view, outcome->src_width, outcome->src_height,
                                         layout.canvas_physical.width, layout.canvas_physical.height);

#if OZVIEWER_ENABLE_EDITING
                // A new document replaces the canvas: abandon any modal Resize
                // draft (Cancel semantics; history and pending validation are
                // discarded) before the swap.
                if (app->resize_dlg != nullptr && IsWindow(app->resize_dlg)) {
                    SendMessageW(app->resize_dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
                }
#endif
                auto prev_image = app->current;
                bool prev_have = app->have_image;
#if OZVIEWER_ENABLE_EDITING
                const bool old_suppress_edit_presentation = app->suppress_edit_presentation;
                app->suppress_edit_presentation = true;
#endif
                app->current = outcome->image;
                app->have_image = true;

                if (!upload_and_present(*app)) {
                    if (!recover_and_present(*app)) {
                        if (noninteractive_mode(*app)) {
                            std::fprintf(stderr, "ozviewer: present failed\n");
                            PostQuitMessage(1);
                            return 0;
                        } else {
                            static_cast<void>(oz::viewer::show_prompt(
                                hwnd,
                                L"OZ Image Viewer",
                                L"Unable to display image.",
                                nullptr,
                                MB_OK,
                                MB_ICONERROR));
                            // Restore prior image
                            app->current = prev_image;
                            app->have_image = prev_have;
#if OZVIEWER_ENABLE_EDITING
                            app->suppress_edit_presentation = old_suppress_edit_presentation;
#endif
                            if (app->have_image) {
                                redraw_current_view(*app);
                            } else {
                                oz::viewer::render_blank(app->renderer);
                                update_open_button_layout(*app);
                            }
                            return 0;
                        }
                    }
                }

#if OZVIEWER_ENABLE_EDITING
                app->suppress_edit_presentation = old_suppress_edit_presentation;
#endif

                if (app->benchmark && app->first_present_done) {
                    PostQuitMessage(0);
                    return 0;
                }

                // Successful commit of document
                app->doc.commit_document(outcome->path);
#if OZVIEWER_ENABLE_EDITING
                if (!app->benchmark && !app->nav_benchmark) {
                    oz::viewer_core::document_metadata meta;
                    meta.source_path = outcome->path;
                    meta.source_dimensions = {outcome->src_width, outcome->src_height};
                    meta.codec = outcome->image ? outcome->image->codec : oz::viewer_core::codec_id::jpeg;
                    meta.codec_name = outcome->image ? get_codec_name_str(outcome->image->codec) : "";
                    meta.file_size = outcome->source_file_size;
                    meta.last_write_time = outcome->source_last_write_time;
                    if (!app->image_doc) {
                        app->image_doc = std::make_unique<oz::viewer_core::image_document>(meta);
                    } else {
                        app->image_doc->reset(meta);
                    }

                    if (outcome->scale_den == 1) {
                        auto buf = std::make_shared<oz::viewer::edit_source_buffer>();
                        buf->document_instance_id = app->image_doc->instance_id();
                        buf->source_path = outcome->path;
                        buf->file_size = meta.file_size;
                        buf->last_write_time = meta.last_write_time;
                        buf->dimensions = {outcome->image->width, outcome->image->height};
                        buf->stride = outcome->image->stride;
                        buf->pixels = outcome->image->pixels;
                        app->full_res_source = std::move(buf);
                    } else {
                        app->full_res_source.reset();
                    }
                    update_edit_menu_state(*app);
                }
#else
                update_menu_command_state(*app);
#endif
                update_open_button_layout(*app);

                refresh_cache_budget(*app);
                const oz::viewer_core::cache_key init_key{
                    oz::viewer_core::normalize_path_key(outcome->path),
                    outcome->scale_den};
                app->cache.insert(init_key, outcome->image, true);

                // Launch async enumeration worker strictly after successful Present
                if (!app->benchmark) {
                    app->cancel_enum.store(true, std::memory_order_relaxed);
                    app->cancel_enum.store(false, std::memory_order_relaxed);
                    app->enum_thread = std::jthread(enum_worker_func, std::ref(*app), outcome->path, app->doc.generation);
                }
                return 0;
            }

            // 2. Stage 2.2 persistent worker completions
            auto messages = app->comp_queue.pop_all();
            if (messages.empty()) {
                return 0;
            }

            const auto layout = current_ui_layout(*app);
            const int view_w = layout.canvas_physical.width;
            const int view_h = layout.canvas_physical.height;
            const auto desired_window = build_desired_window_keys(*app, view_w, view_h);

            const std::filesystem::path* desired_path = app->nav.at(app->desired_index);
            const std::wstring desired_norm = desired_path ? oz::viewer_core::normalize_path_key(*desired_path) : L"";
            bool desired_mismatch_needs_requeue = false;

            for (auto& item_msg : messages) {
                if (!item_msg.ok || !item_msg.image) {
                    if (app->nav_benchmark && app->current_nav_bench_req_id != 0 &&
                        item_msg.priority == oz::viewer_core::work_priority::p0_selected &&
                        !desired_norm.empty() &&
                        item_msg.key.normalized_path == desired_norm) {
                        app->nav_bench_session.record_request_failed(
                            app->current_nav_bench_req_id, app->current_epoch,
                            "p0_decode_failed");
                        app->current_nav_bench_req_id = 0;
                    }
                    continue;
                }

                if (oz::viewer_core::should_commit_completion(item_msg, desired_norm, view_w, view_h)) {
                    try_commit_candidate(*app, item_msg.image, item_msg.key,
                                         app->desired_index, desired_window);
                } else {
                    if (item_msg.key.normalized_path == desired_norm && !desired_norm.empty()) {
                        desired_mismatch_needs_requeue = true;
                    }
                    if (!desired_norm.empty() && path_in_window(desired_window, item_msg.key.normalized_path) &&
                        oz::viewer_core::image_profile_matches_viewport(*item_msg.image, view_w, view_h) &&
                        app->cache.can_admit_preload(item_msg.image->byte_size())) {
                        app->cache.insert(item_msg.key, item_msg.image, false, desired_window);
                    }
                }
            }

            if (desired_mismatch_needs_requeue && !desired_norm.empty() &&
                app->cache.find_for_viewport(desired_norm, view_w, view_h) == nullptr &&
                !app->comp_queue.has_valid_for_path(desired_norm, view_w, view_h)) {
                schedule_navigation_plan(*app);
            }

            return 0;
        }
#if OZVIEWER_ENABLE_EDITING
        case kMsgEditComplete: {
            if (!app || app->benchmark || app->nav_benchmark || !app->image_doc || !app->edit_ctrl) {
                return 0;
            }

            auto comps = app->edit_ctrl->pop_completions();
            for (auto& c : comps) {
                // Must match exact document_instance_id
                if (c.document_instance_id != app->image_doc->instance_id()) {
                    continue; // Stale document, ignore
                }

                if (app->pending_resize.has_value() &&
                    c.request_id == app->pending_resize->request_id) {
                    // The single in-flight Resize validation owns its matching
                    // completion; every identity check happens there.
                    handle_pending_resize_completion(*app, c);
                    continue;
                }

                // The renderer's retained bitmap always represents the
                // immutable oriented source. Promote an acquired full-res
                // *source* (when fast-first originally used a scaled decode).
                // Never promote an already-materialized edit result here:
                // later redraws/undo would otherwise apply the same command
                // prefix twice.
                if (c.source && c.source->is_valid()) {
                    app->full_res_source = c.source;
                    adopt_acquired_source(*app, *c.source);
                    if (app->crop.has_value() && app->crop->active) {
                        // Candidate promotion is deliberately overlay-free;
                        // redraw once so a pending crop draft stays visible.
                        redraw_current_view(*app);
                    }
                }
                // Materializer output remains a worker-local/result-owned
                // bounded artifact until a future Save/Save As slice consumes
                // it. Discarding it here preserves one immutable GPU source
                // bitmap and exact non-destructive undo/redo rendering.
                c.result.pixels.clear();
            }
            return 0;
        }
        case kMsgSaveComplete: {
            if (!app || app->benchmark || app->nav_benchmark || !app->save_ctrl) {
                return 0;
            }
            auto comps = app->save_ctrl->pop_completions();
            for (auto& comp : comps) {
                handle_save_completion(*app, comp);
            }
            update_edit_menu_state(*app);
            return 0;
        }
#endif
        case kMsgCopyComplete: {
            if (!app || app->benchmark || app->nav_benchmark || !app->copy_ctrl) {
                return 0;
            }
            auto comps = app->copy_ctrl->pop_completions();
            for (auto& comp : comps) {
                if (!app->pending_copy.has_value() || comp.request_id != app->pending_copy->request_id) {
                    continue; // Stale or replaced request
                }
                const auto pending = *app->pending_copy;
                app->pending_copy.reset();

                // Validate that the document generation hasn't changed
                if (comp.document_generation != app->doc.generation) {
                    continue; // Stale generation, discard without clipboard change
                }

#if OZVIEWER_ENABLE_EDITING
                // If editing is enabled, validate that document_instance_id and edit_revision are still current
                if (app->image_doc) {
                    if (comp.document_instance_id != app->image_doc->instance_id() ||
                        comp.diagnostic_revision != app->image_doc->edit_revision()) {
                        continue; // Stale document or revision, discard without clipboard change
                    }
                }
#endif

                if (!comp.ok) {
                    if (!noninteractive_mode(*app)) {
                        static_cast<void>(oz::viewer::show_prompt(
                            app->hwnd,
                            L"Copy Image",
                            L"Unable to copy image to the clipboard.",
                            L"The source image could not be decoded or materialized.",
                            MB_OK,
                            MB_ICONWARNING));
                    }
                } else {
                    // Publish straight BGRA CF_DIBV5 to clipboard
                    const auto res = oz::viewer::copy_image_to_clipboard(
                        app->hwnd,
                        comp.pixels.data(),
                        comp.dimensions.width,
                        comp.dimensions.height,
                        comp.stride);
                    if (!res.ok() && !noninteractive_mode(*app)) {
                        static_cast<void>(oz::viewer::show_prompt(
                            app->hwnd,
                            L"Copy Image",
                            L"Unable to copy image to the clipboard.",
                            L"The clipboard could not be opened or memory allocation failed.",
                            MB_OK,
                            MB_ICONWARNING));
                    }
                }
            }
            update_edit_menu_state(*app);
            return 0;
        }
        case WM_TIMER: {
            if (wparam == kNavBenchTimerId && app && app->nav_benchmark) {
                auto& session = app->nav_bench_session;
                session.update_cache_stats(app->cache.used_bytes(), app->cache.budget(),
                                           app->cache.size(), app->cache.resident_image_count());

                if (session.enforce_watchdog()) {
                    KillTimer(hwnd, kNavBenchTimerId);
                    session.finish("timed_out", session.timeout_reason());
                    PostQuitMessage(0);
                    return 0;
                }

                if (session.step_pending()) {
                    return 0;
                }

                const auto layout = current_ui_layout(*app);
                const int view_w = layout.canvas_physical.width;
                const int view_h = layout.canvas_physical.height;

                oz::viewer_core::nav_bench_viewer_state viewer_state;
                viewer_state.list_size = app->nav.size();
                viewer_state.committed_index = app->committed_index;
                viewer_state.work_drained = app->work_queue.pending_count() == 0 &&
                                            app->comp_queue.size() == 0;
                viewer_state.target_present = false;
                if (app->nav_bench_await_valid) {
                    const std::filesystem::path* awaited =
                        app->nav.at(app->nav_bench_await_index);
                    if (awaited != nullptr) {
                        const std::wstring norm =
                            oz::viewer_core::normalize_path_key(*awaited);
                        viewer_state.target_present =
                            app->cache.find_for_viewport(norm, view_w, view_h) != nullptr;
                    }
                }

                const oz::viewer_core::nav_bench_step step =
                    session.pump_benchmark_step(viewer_state);
                app->nav_bench_await_valid =
                    (step.kind == oz::viewer_core::nav_bench_step_kind::await_cache);
                app->nav_bench_await_index = step.target_index;
                if (step.kind != oz::viewer_core::nav_bench_step_kind::rearm) {
                    app->nav_bench_rearm_started = false;
                }

                switch (step.kind) {
                    case oz::viewer_core::nav_bench_step_kind::idle:
                    case oz::viewer_core::nav_bench_step_kind::await_drain:
                        return 0;
                    case oz::viewer_core::nav_bench_step_kind::rearm:
                        if (!app->nav_bench_rearm_started ||
                            app->nav_bench_rearm_index != step.target_index) {
                            app->nav_bench_rearm_index = step.target_index;
                            app->nav_bench_rearm_started = true;
                            execute_benchmark_rearm(*app, step.target_index);
                        }
                        return 0;
                    case oz::viewer_core::nav_bench_step_kind::await_cache:
                        if (step.purge_app_cache) {
                            app->cache.clear_except_pinned();
                        }
                        return 0;
                    case oz::viewer_core::nav_bench_step_kind::navigation: {
                        if (step.purge_app_cache) {
                            app->cache.clear_except_pinned();
                        }
                        const std::string sc_label = step.scenario.empty()
                                                         ? session.current_scenario_label()
                                                         : step.scenario;
                        const bool issued = execute_navigation_request(
                            *app, static_cast<WPARAM>(step.vk_key), sc_label);
                        if (!issued) {
                            session.note_step_no_op(sc_label, "no_op",
                                                    "navigation_no_op_at_boundary");
                        }
                        return 0;
                    }
                    case oz::viewer_core::nav_bench_step_kind::finished:
                        KillTimer(hwnd, kNavBenchTimerId);
                        session.finish("completed", "");
                        PostQuitMessage(0);
                        return 0;
                }
            }
            return 0;
        }
        case WM_CONTEXTMENU: {
            if (!app || noninteractive_mode(*app) || !app->have_image || !app->current) {
                break;
            }

            // Reject if the context menu message was not originated for the main window
            if (reinterpret_cast<HWND>(wparam) != hwnd) {
                break;
            }

            const auto layout = current_ui_layout(*app);
            const auto& canvas = layout.canvas_physical;
            if (canvas.is_empty()) {
                break;
            }

            int screen_x = GET_X_LPARAM(lparam);
            int screen_y = GET_Y_LPARAM(lparam);

            if (screen_x == -1 && screen_y == -1) {
                // Keyboard invocation (Shift+F10 / VK_APPS):
                // Anchor at center of canvas physical rect only if main window currently has keyboard focus
                if (GetFocus() != hwnd) {
                    break;
                }
                POINT center_pt{
                    canvas.x + canvas.width / 2,
                    canvas.y + canvas.height / 2
                };
                ClientToScreen(hwnd, &center_pt);
                screen_x = center_pt.x;
                screen_y = center_pt.y;
            } else {
                // Pointer invocation:
                // Must be inside canvas_physical (letterbox included).
                // Rejects toolbar, status, crop strip, open button, menu/non-client.
                POINT pt{screen_x, screen_y};
                ScreenToClient(hwnd, &pt);
                if (!canvas.contains(pt.x, pt.y)) {
                    break;
                }
            }

            const auto ctx = current_ui_command_context(*app);
            const auto plan = oz::viewer::build_canvas_context_menu_plan(ctx);
            if (!plan.visible || plan.items.empty()) {
                break;
            }

            HMENU hmenu = CreatePopupMenu();
            if (!hmenu) {
                break;
            }

            for (std::size_t i = 0; i < plan.items.size(); ++i) {
                const auto& item = plan.items[i];
                if (item.type == oz::viewer::context_menu_item_type::separator) {
                    AppendMenuW(hmenu, MF_SEPARATOR, 0, nullptr);
                } else {
                    UINT flags = MF_STRING;
                    flags |= (item.enabled ? MF_ENABLED : MF_GRAYED);
                    flags |= (item.checked ? MF_CHECKED : MF_UNCHECKED);
                    AppendMenuW(hmenu, flags, static_cast<UINT_PTR>(item.command_id), item.text.c_str());
                }
            }

            // Display popup menu and route selected command solely via WM_COMMAND
            const UINT selected_id = TrackPopupMenuEx(
                hmenu,
                TPM_RETURNCMD | TPM_RIGHTBUTTON,
                screen_x,
                screen_y,
                hwnd,
                nullptr
            );

            DestroyMenu(hmenu);

            if (selected_id != 0) {
                SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(selected_id, 0), 0);
            }
            return 0;
        }
        case WM_CLOSE: {
#if OZVIEWER_ENABLE_EDITING
            // The modal Resize dialog owns the interaction: its own Cancel
            // path must run before the application can close.
            if (app && app->resize_dlg != nullptr && IsWindow(app->resize_dlg)) {
                return 0;
            }
#endif
            if (app && !check_dirty_discard_allowed(*app)) {
                return 0; // Prevent window close
            }
            break;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

int wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, PWSTR /*lpCmdLine*/,
             int /*nCmdShow*/) {
    // --- argument parsing ---------------------------------------------------
    bool benchmark = false;
    bool nav_benchmark = false;
    oz::viewer_core::nav_bench_options nav_opts;
    oz::viewer_core::corpus_validation_result nav_corpus_validation;
    std::string nav_arg_error;
    std::wstring path_str;
    bool zero_arg_launch = false;

    int argc = 0;
    PWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return 2;
    }

    if (argc >= 2 && std::wstring(argv[1]) == L"--navigation-benchmark") {
        if (!oz::viewer_core::parse_navigation_benchmark_args(argc, argv, nav_opts, nav_arg_error)) {
            std::fwprintf(stderr, L"ozviewer: --navigation-benchmark argument error: %hs\n", nav_arg_error.c_str());
            LocalFree(argv);
            return 1;
        }
        nav_benchmark = true;
    } else if (argc == 1) {
        zero_arg_launch = true;
    } else if (argc == 2) {
        path_str = argv[1];
    } else if (argc == 3 && std::wstring(argv[1]) == L"--benchmark") {
        benchmark = true;
        path_str = argv[2];
    } else {
        MessageBoxW(nullptr, L"usage: ozviewer.exe [image]\n"
                             L"       ozviewer.exe --benchmark <image>\n"
                             L"       ozviewer.exe --navigation-benchmark --corpus <dir> --out <file.jsonl> [options]\n"
                             L"Supported: JPEG (.jpg/.jpeg), PNG (.png), static WebP (.webp) when enabled,\n"
                             L"static AVIF (.avif) when enabled, static HEIC/HEIF (.heic/.heif) when enabled/loaded,\n"
                             L"Phase A: BMP (.bmp), static GIF (.gif), classic TIFF page 0 (.tif/.tiff), ICO (.ico),\n"
                             L"Phase B: CUR (.cur), TGA (.tga).",
                    L"OZ Image Viewer", MB_OK | MB_ICONINFORMATION);
        LocalFree(argv);
        return 2;
    }
    LocalFree(argv);

    // --- pre-window validation for navigation benchmark ----------------------
    if (nav_benchmark) {
        const std::string option_error = oz::viewer_core::validate_nav_bench_options(nav_opts);
        if (!option_error.empty()) {
            std::fwprintf(stderr, L"ozviewer: --navigation-benchmark option error: %hs\n",
                          option_error.c_str());
            return 1;
        }
        std::error_code ec;
        if (std::filesystem::exists(nav_opts.output_path, ec) && !nav_opts.overwrite) {
            std::fwprintf(stderr, L"ozviewer: output file exists: %ls (use --overwrite to replace)\n",
                          nav_opts.output_path.c_str());
            return 3;
        }
        nav_corpus_validation = oz::viewer_core::validate_navigation_corpus(nav_opts.corpus_dir);
        if (!nav_corpus_validation.ok) {
            std::fwprintf(stderr, L"ozviewer: corpus validation failed: %hs\n",
                          nav_corpus_validation.error.c_str());
            return 2;
        }
        path_str = (nav_opts.corpus_dir / nav_corpus_validation.entries[0].rel_filename).wstring();
    }

    // --- marker session is the first entry action --------------------------
    oz::viewer_core::marker_emitter markers(benchmark);

    // --- per-monitor-v2 DPI awareness before HWND creation ------------------
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // --- register window class ----------------------------------------------
    const wchar_t kClassName[] = L"OZViewerStage1";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS; // Enable double clicks
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // Application icon embedded by ozviewer.rc (assets/icons/ozviewer.ico).
    // LoadImageW picks the best-matching frame from the RT_GROUP_ICON; a null
    // result leaves the previous system default, so this cannot regress startup.
    wc.hIcon = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(IDI_OZVIEWER_APPICON), IMAGE_ICON, 0, 0,
        LR_DEFAULTSIZE));
    wc.hIconSm = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(IDI_OZVIEWER_APPICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wc.lpszClassName = kClassName;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!RegisterClassExW(&wc)) {
        std::fwprintf(stderr, L"ozviewer: RegisterClassExW failed\n");
        return 1;
    }

    // Minimal native File->Open menu and Edit menu (interactive mode only; benchmark creates NO menu)
    HMENU hMenu = nullptr;
    HMENU hFileMenu = nullptr;
#if OZVIEWER_ENABLE_EDITING
    HMENU hEditMenu = nullptr;
    HMENU hCropRatioMenu = nullptr;
#endif
    if (!benchmark && !nav_benchmark) {
        hFileMenu = CreatePopupMenu();
        AppendMenuW(hFileMenu, MF_STRING, kIdFileOpen, L"&Open...\tCtrl+O");
        AppendMenuW(hFileMenu, MF_STRING | MF_GRAYED, kIdFileOpenLocation,
                    L"Open File &Location");
        AppendMenuW(hFileMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hFileMenu, MF_STRING | MF_GRAYED, kIdFileCopyImage, L"Copy &Image\tCtrl+C");
        AppendMenuW(hFileMenu, MF_STRING | MF_GRAYED, kIdFileCopy, L"Copy &File");
        AppendMenuW(hFileMenu, MF_STRING | MF_GRAYED, kIdFileCopyPath, L"Copy File &Path");
#if OZVIEWER_ENABLE_EDITING
        // Save/Save As exist only in interactive editing mode; benchmark mode
        // creates no menu at all.
        AppendMenuW(hFileMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hFileMenu, MF_STRING | MF_GRAYED, kIdFileSave, L"&Save\tCtrl+S");
        AppendMenuW(hFileMenu, MF_STRING | MF_GRAYED, kIdFileSaveAs,
                    L"Save &As...\tCtrl+Shift+S");
#endif
        hMenu = CreateMenu();
        AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hFileMenu), L"&File");

#if OZVIEWER_ENABLE_EDITING
        hEditMenu = CreatePopupMenu();
        AppendMenuW(hEditMenu, MF_STRING | MF_GRAYED, kIdEditUndo, L"&Undo\tCtrl+Z");
        AppendMenuW(hEditMenu, MF_STRING | MF_GRAYED, kIdEditRedo, L"&Redo\tCtrl+Y");
        AppendMenuW(hEditMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hEditMenu, MF_STRING | MF_GRAYED, kIdEditRotateLeft, L"Rotate &Left\tCtrl+L");
        AppendMenuW(hEditMenu, MF_STRING | MF_GRAYED, kIdEditRotateRight, L"Rotate &Right\tCtrl+R");
        AppendMenuW(hEditMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hEditMenu, MF_STRING | MF_GRAYED, kIdEditFlipH, L"Flip &Horizontal\tCtrl+Shift+H");
        AppendMenuW(hEditMenu, MF_STRING | MF_GRAYED, kIdEditFlipV, L"Flip &Vertical\tCtrl+Shift+V");
        AppendMenuW(hEditMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hEditMenu, MF_STRING | MF_GRAYED, kIdEditCrop, L"&Crop...\tC");
        AppendMenuW(hEditMenu, MF_STRING | MF_GRAYED, kIdEditResize, L"&Resize...\tCtrl+Alt+R");
        AppendMenuW(hEditMenu, MF_STRING | MF_GRAYED, kIdEditCropApply, L"Apply Crop\tEnter");
        AppendMenuW(hEditMenu, MF_STRING | MF_GRAYED, kIdEditCropCancel, L"Cancel Crop\tEsc");
        AppendMenuW(hEditMenu, MF_STRING | MF_GRAYED, kIdEditCropReset, L"Reset Crop\tR");
        hCropRatioMenu = CreatePopupMenu();
        AppendMenuW(hCropRatioMenu, MF_STRING | MF_GRAYED, kIdCropRatioFree, L"&Free");
        AppendMenuW(hCropRatioMenu, MF_STRING | MF_GRAYED, kIdCropRatioOriginal, L"&Original");
        AppendMenuW(hCropRatioMenu, MF_STRING | MF_GRAYED, kIdCropRatio1x1, L"1:&1");
        AppendMenuW(hCropRatioMenu, MF_STRING | MF_GRAYED, kIdCropRatio3x2, L"3:2");
        AppendMenuW(hCropRatioMenu, MF_STRING | MF_GRAYED, kIdCropRatio4x3, L"4:3");
        AppendMenuW(hCropRatioMenu, MF_STRING | MF_GRAYED, kIdCropRatio5x4, L"5:4");
        AppendMenuW(hCropRatioMenu, MF_STRING | MF_GRAYED, kIdCropRatio16x9, L"16:9");
        AppendMenuW(hCropRatioMenu, MF_STRING | MF_GRAYED, kIdCropRatio9x16, L"9:16");
        AppendMenuW(hCropRatioMenu, MF_STRING | MF_GRAYED, kIdCropRatioASeries, L"A-series");
        AppendMenuW(hEditMenu, MF_POPUP | MF_GRAYED,
                    reinterpret_cast<UINT_PTR>(hCropRatioMenu), L"Crop &Ratio");
        AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hEditMenu), L"&Edit");
#endif
    }

    app_state app;
    app.benchmark = benchmark;
    app.nav_benchmark = nav_benchmark;
    app.markers = &markers;
    app.hMenuFile = hFileMenu;
#if OZVIEWER_ENABLE_EDITING
    app.hMenuEdit = hEditMenu;
    app.hMenuCropRatio = hCropRatioMenu;
#endif
    if (!zero_arg_launch) {
        app.initial_path = std::filesystem::path(path_str);
    }

    app.hwnd = CreateWindowExW(0, kClassName, L"OZ Image Viewer", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 1024, 768, nullptr, hMenu,
                               wc.hInstance, &app);

    if (!app.hwnd) {
        std::fwprintf(stderr, L"ozviewer: CreateWindowExW failed\n");
        if (hMenu) DestroyMenu(hMenu);
        return 4;
    }

    // Interactive Drag & Drop acceptance and zero-arg Open button
    // (disabled in both --benchmark and --navigation-benchmark modes)
    if (!benchmark && !nav_benchmark) {
        DragAcceptFiles(app.hwnd, TRUE);

        // Minimal native Open button for zero-arg / no-image window
        app.btn_open = CreateWindowExW(0, L"BUTTON", L"Open Image...",
                                       WS_CHILD | BS_PUSHBUTTON,
                                       0, 0, 160, 44,
                                       app.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBtnOpen)),
                                       wc.hInstance, nullptr);
    }

    // --- renderer ------------------------------------------------------------
    if (!oz::viewer::create_renderer(app.hwnd, app.renderer)) {
        std::fwprintf(stderr, L"ozviewer: renderer creation failed\n");
        return 4;
    }
    markers.record(oz::markers::marker_id::renderer_ready);

    RECT rc{};
    GetClientRect(app.hwnd, &rc);
    if (!oz::viewer::resize_renderer(app.renderer, static_cast<std::uint32_t>(rc.right),
                                     static_cast<std::uint32_t>(rc.bottom))) {
        std::fwprintf(stderr, L"ozviewer: renderer resize failed\n");
        return 4;
    }

    if (nav_benchmark) {
        const auto status = app.nav_bench_session.init(
            nav_opts,
            static_cast<std::int32_t>(rc.right),
            static_cast<std::int32_t>(rc.bottom),
            app.renderer.dpi,
            app.renderer.warp,
            &nav_corpus_validation);
        if (status != oz::viewer_core::nav_bench_status::success) {
            std::fwprintf(stderr, L"ozviewer: navigation benchmark session init failed (status %d)\n",
                          static_cast<int>(status));
            return static_cast<int>(status);
        }
    }

    ShowWindow(app.hwnd, SW_SHOW);
    UpdateWindow(app.hwnd);

    // Stage 7: Initialize interactive-only modern chrome (toolbar + status bar)
    if (!benchmark && !nav_benchmark) {
        app.chrome = std::make_unique<oz::viewer::ui_chrome>();
        if (app.chrome->create(app.hwnd, hInstance, app.renderer.dpi)) {
            app.chrome->update_icons(app.renderer.d2d_context.Get());
            const auto layout = current_ui_layout(app);
            app.chrome->update_layout(layout, app.is_fullscreen);
            update_edit_menu_state(app);
        }

#if OZVIEWER_ENABLE_EDITING
        app.crop_strip_ui = std::make_unique<oz::viewer::crop_strip>();
        app.crop_strip_ui->create(app.hwnd, hInstance, app.renderer.dpi);
#endif
    }

#if OZVIEWER_ENABLE_EDITING
    if (!benchmark && !nav_benchmark) {
        app.edit_ctrl = std::make_unique<oz::viewer::edit_controller>();
        app.edit_ctrl->start(app.hwnd, kMsgEditComplete);
        app.edit_ctrl_started = true;
        // Single-flight Save worker: created only in interactive non-benchmark
        // mode, exactly like the edit controller (zero edit facilities in
        // benchmark/navigation-benchmark runs).
        app.save_ctrl = std::make_unique<oz::viewer::save_controller>();
        app.save_ctrl->start(app.hwnd, kMsgSaveComplete);
        app.save_ctrl_started = true;
    }
#endif

    // --- initial fast-first decode worker (if launched with image path) -------
    if (!zero_arg_launch) {
        app.initial_decode_thread = std::jthread(initial_decode_worker_func, std::ref(app), app.initial_path,
                                                 static_cast<std::int32_t>(rc.right),
                                                 static_cast<std::int32_t>(rc.bottom),
                                                 app.doc.generation);
    } else {
        // Zero-arg interactive launch: clear viewport and show centered Open button
        oz::viewer::render_blank(app.renderer);
        update_open_button_layout(app);
    }

    // --- message loop ----------------------------------------------------------
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Pre-filter keyboard messages for accessibility traversal and shortcut precedence
        if (msg.message == WM_KEYDOWN) {
            const HWND focused = GetFocus();
            const bool shift_down = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool ctrl_down = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool alt_down = (GetKeyState(VK_MENU) & 0x8000) != 0;
#if OZVIEWER_ENABLE_EDITING
            const bool crop_active = (app.crop.has_value() && app.crop->active);

            // 1. Strict Crop Modal Precedence:
            // Esc, Enter, and 'R' (without modifiers) MUST route to the crop handler
            // even if a child control inside crop strip or toolbar has keyboard focus.
            if (crop_active) {
                if (msg.wParam == VK_ESCAPE) {
                    cancel_crop_mode(app);
                    continue;
                }
                if (msg.wParam == VK_RETURN) {
                    apply_crop_mode(app);
                    continue;
                }
                if (msg.wParam == 'R' && !ctrl_down && !shift_down && !alt_down) {
                    reset_crop_mode(app);
                    continue;
                }
            }
#endif

            // 2. Global Hotkeys & Modifiers:
            // Hotkeys (Ctrl+O, Ctrl+S, Ctrl+Shift+S, Ctrl+Z, Ctrl+Y, Ctrl+L, Ctrl+R,
            // Ctrl+Alt+R, Ctrl+C, Ctrl+V, F11, etc.) must work regardless of child control focus,
            // unless native text copy/paste applies to an Edit/RichEdit control or Resize dialog owns focus.
            if (ctrl_down || alt_down || msg.wParam == VK_F11) {
                if (msg.wParam == 'C' || msg.wParam == 'c' || msg.wParam == 'V' || msg.wParam == 'v') {
                    HWND resize_hwnd = nullptr;
#if OZVIEWER_ENABLE_EDITING
                    resize_hwnd = app.resize_dlg;
#endif
                    const auto ctx = current_ui_command_context(app);
                    const auto copy_img_st = oz::viewer::derive_command_state(oz::viewer::ui_command_id::file_copy_image, ctx);
                    const auto action = oz::viewer::route_shortcut_key(
                        msg.wParam, ctrl_down, shift_down, alt_down,
                        focused, resize_hwnd,
#if OZVIEWER_ENABLE_EDITING
                        crop_active,
#else
                        false,
#endif
                        copy_img_st);

                    if (action == oz::viewer::shortcut_action::dialog_default ||
                        action == oz::viewer::shortcut_action::native_text_copy ||
                        action == oz::viewer::shortcut_action::native_text_paste) {
                        // Let standard dispatch handle native text copy/paste for edit control or resize dialog
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                        continue;
                    }
                }

                SendMessageW(app.hwnd, WM_KEYDOWN, msg.wParam, msg.lParam);
                continue;
            }

            // 3. F6 Pane Traversal:
            // Cycles focus between: Main Canvas (app.hwnd) -> Toolbar -> Crop Strip (if active) -> Open Button (if visible)
            if (msg.wParam == VK_F6) {
                HWND next_target = app.hwnd;
#if OZVIEWER_ENABLE_EDITING
                const bool crop_strip_avail = (crop_active && app.crop_strip_ui && app.crop_strip_ui->is_created());
#else
                const bool crop_strip_avail = false;
#endif
                const bool tb_avail = (app.chrome && app.chrome->is_created() && !app.is_fullscreen);
                const bool open_btn_avail = (app.btn_open && IsWindowVisible(app.btn_open));

                if (focused == app.hwnd) {
                    if (tb_avail) {
                        next_target = app.chrome->toolbar_hwnd();
                    } else if (crop_strip_avail) {
                        next_target = app.crop_strip_ui->first_tab_stop();
                    } else if (open_btn_avail) {
                        next_target = app.btn_open;
                    }
                } else if (app.chrome && app.chrome->contains_hwnd(focused)) {
                    if (crop_strip_avail) {
                        next_target = app.crop_strip_ui->first_tab_stop();
                    } else if (open_btn_avail) {
                        next_target = app.btn_open;
                    } else {
                        next_target = app.hwnd;
                    }
                } else if (app.crop_strip_ui && app.crop_strip_ui->contains_hwnd(focused)) {
                    if (open_btn_avail) {
                        next_target = app.btn_open;
                    } else {
                        next_target = app.hwnd;
                    }
                } else {
                    next_target = app.hwnd;
                }

                if (next_target && IsWindow(next_target)) {
                    SetFocus(next_target);
                }
                continue;
            }

            // 4. Tab Navigation inside contextual Crop Strip:
            // Enables cycling through crop strip controls (Ratio Combo -> Reset -> Apply -> Cancel)
            if (msg.wParam == VK_TAB && app.crop_strip_ui && app.crop_strip_ui->contains_hwnd(focused)) {
                HWND next_ctrl = GetNextDlgTabItem(app.crop_strip_ui->hwnd(), focused, shift_down ? TRUE : FALSE);
                if (next_ctrl && IsWindow(next_ctrl)) {
                    SetFocus(next_ctrl);
                }
                continue;
            }

            // 5. If focused is the Toolbar or a non-text child control, route standard non-text keys to main window
            if (app.chrome && app.chrome->contains_hwnd(focused)) {
                if (msg.wParam != VK_TAB && msg.wParam != VK_LEFT && msg.wParam != VK_RIGHT &&
                    msg.wParam != VK_SPACE && msg.wParam != VK_RETURN) {
                    SendMessageW(app.hwnd, WM_KEYDOWN, msg.wParam, msg.lParam);
                    continue;
                }
            }
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // --- teardown --------------------------------------------------------------
#if OZVIEWER_ENABLE_EDITING
    if (app.save_ctrl_started && app.save_ctrl) {
        // Cooperative cancel plus join: the window is still alive here, so the
        // worker can post its final completion safely.
        app.save_ctrl->stop();
        app.save_ctrl_started = false;
    }
    if (app.edit_ctrl_started && app.edit_ctrl) {
        app.edit_ctrl->stop();
        app.edit_ctrl_started = false;
    }
#endif
    if (app.copy_ctrl_started && app.copy_ctrl) {
        app.copy_ctrl->stop();
        app.copy_ctrl_started = false;
    }
    app.cancel_initial_decode.store(true, std::memory_order_relaxed);
    app.cancel_enum.store(true, std::memory_order_relaxed);
    app.work_queue.request_stop();

    if (app.initial_decode_thread.joinable()) {
        app.initial_decode_thread.join();
    }
    if (app.enum_thread.joinable()) {
        app.enum_thread.join();
    }
    if (app.persistent_worker_thread.joinable()) {
        app.persistent_worker_thread.join();
    }

    app.renderer = oz::viewer::renderer{};
    if (app.font_open_btn) {
        DeleteObject(app.font_open_btn);
        app.font_open_btn = nullptr;
    }
    DestroyWindow(app.hwnd);

    return static_cast<int>(msg.wParam);
}

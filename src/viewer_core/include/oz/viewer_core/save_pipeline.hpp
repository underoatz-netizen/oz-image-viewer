// oz/viewer_core/save_pipeline.hpp - bounded non-destructive Save/Save As
// export pipeline (Stage 5 Slice 6, Stage 6).
//
// Conservative contract:
// - Output formats: PNG, JPEG, WebP (lossless straight BGRA via static WebP::webp
//   when OZVIEWER_HAVE_WEBP_ENCODER=1), TIFF (Microsoft WIC 32bpp BGRA + ZIP),
//   BMP (Microsoft WIC 24bpp BGR uncompressed, opaque only).
// - PNG, WebP, TIFF keep alpha (canonical premultiplied PBGRA8 is converted to straight
//   BGRA8 before encoding).
// - JPEG and BMP require every pixel to be fully opaque. There is no implicit matte
//   and no silent format/quality fallback: an alpha pixel is an explicit
//   alpha_not_supported failure that tells the user to use PNG, WebP, or TIFF.
// - JPEG quality is a fixed configured value (default 0.90); encoder creation
//   and quality/format configuration are validated at runtime.
// - TIFF requires GUID_VendorMicrosoft, single-page, 32bpp BGRA straight alpha,
//   and TiffCompressionMethod=WICTiffCompressionZIP as VT_UI1.
// - BMP requires GUID_VendorMicrosoft, 24bpp BGR uncompressed BI_RGB, opaque only.
// - The commit step is a same-directory unique temporary file encoded first,
//   then revalidated and either ReplaceFileW (existing target) or
//   MoveFileExW without REPLACE_EXISTING (new target). There is no direct
//   truncation or copy-over of the destination.
// - Materialized save pixels are worker-local. Nothing in this module touches
//   the renderer, the retained source bitmap, the CPU cache, or edit history.
//
// The worker entry point is synchronous and side-effect-bounded so it can be
// driven by the single-flight save_controller in the viewer and by focused
// deterministic tests.
#pragma once

#include "oz/viewer_core/codec.hpp"
#include "oz/viewer_core/edit_pipeline.hpp"
#include "oz/viewer_core/edit_state.hpp"
#include "oz/viewer_core/image_document.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace oz::viewer_core {

// Fixed conservative JPEG quality (no quality dialog in this slice).
inline constexpr float kDefaultJpegQuality = 0.90f;

enum class save_format {
    png,
    jpeg,
    webp,
    tiff,
    bmp
};

// Bounded error taxonomy for the Save worker. `none` is the only success value.
enum class save_error {
    none,
    cancelled,                     // cooperative cancellation before replacement
    invalid_request,               // bad destination/format/source/commands
    stale_source,                  // source file identity changed before save
    destination_conflict,          // destination appeared/changed since capture
    materialization_failed,        // bounded CPU materialization/allocation failed
    alpha_not_supported,           // JPEG/BMP requested for pixels with alpha != 255
    encoder_unavailable,           // requested encoder (WIC/WebP) is not available
    encoder_configuration_failed,  // encoder rejected format/quality configuration
    encode_failed,                 // pixel/stream write failed
    temp_create_failed,            // unique same-directory temp file could not be created
    flush_failed,                  // commit/flush of the encoded temp file failed
    replace_failed,                // replacement/move failed with destination untouched
    commit_state_uncertain,        // replacement outcome ambiguous; never auto-clean
    cleanup_failed,                // temp cleanup failed (reported as detail suffix)
};

[[nodiscard]] std::string_view to_string(save_error error) noexcept;
[[nodiscard]] std::string_view to_string(save_format format) noexcept;

// Whether WebP encoder capability is compiled in.
[[nodiscard]] bool webp_encoder_available() noexcept;

// --- typed extension / routing policy ---------------------------------------

// .png -> png; .jpg/.jpeg -> jpeg; .webp -> webp; .tif/.tiff -> tiff; .bmp -> bmp
// (case-insensitive); anything else (including an empty or unknown extension) -> nullopt.
// Extensions are the only routing signal for Save As output; unsupported typed
// extensions must be rejected by the caller rather than written with mismatched bytes.
[[nodiscard]] std::optional<save_format> save_format_for_extension(
    std::wstring_view extension) noexcept;

// Canonical default extension for a format (.png, .jpg, .webp, .tif, .bmp).
[[nodiscard]] const wchar_t* preferred_extension(save_format format) noexcept;

// Save-in-place is defined for PNG, JPEG, BMP, and WebP (when encoder available).
// TIFF is Save-As-only. Every other V1 source codec (AVIF/HEIC/GIF/ICO/CUR/TGA/etc.)
// routes to Save As.
[[nodiscard]] bool codec_supports_in_place_save(codec_id codec) noexcept;

enum class save_route {
    none,
    save_in_place,
    save_as
};

struct save_routing {
    save_route route = save_route::none;
    save_format format = save_format::png;
    bool source_supports_in_place = false;
};

// Save routing policy:
// - Save As requested -> save_as (PNG selected by default).
// - Otherwise an in-place eligible source with a known source path -> save_in_place with
//   the matching format.
// - Otherwise (TIFF, unsupported codec, unknown/no source path) -> save_as with PNG
//   selected.
[[nodiscard]] save_routing route_save_request(codec_id source_codec,
                                              bool has_source_path,
                                              bool save_as_requested) noexcept;

// --- filesystem identity helpers --------------------------------------------

struct file_fingerprint {
    bool exists = false;
    std::uint64_t file_size = 0;
    std::uint64_t last_write_time = 0;
};

// Size/last-write fingerprint via a metadata-only CreateFileW. Never
// enumerates directories and never opens file contents.
[[nodiscard]] file_fingerprint query_file_fingerprint(
    const std::filesystem::path& path) noexcept;

// True when both paths resolve to the same existing file identity (volume
// serial + file index), or their normalized case-insensitive paths match.
[[nodiscard]] bool same_file_identity(const std::filesystem::path& a,
                                      const std::filesystem::path& b) noexcept;

// --- save request/result ----------------------------------------------------

struct save_source_fingerprint {
    std::filesystem::path path{};
    std::uint64_t file_size = 0;
    std::uint64_t last_write_time = 0;
    edit_dimensions dimensions{};
    // When true, the worker revalidates the source file identity before
    // materialization and again before commit; a mismatch is stale_source.
    bool expected = false;
};

struct save_destination_baseline {
    bool existed = false;
    std::uint64_t file_size = 0;
    std::uint64_t last_write_time = 0;
};

struct save_request {
    std::filesystem::path destination{};
    save_format format = save_format::png;
    float jpeg_quality = kDefaultJpegQuality;

    // Immutable canonical PBGRA8 source view. The caller owns the pixels for
    // the whole duration of execute_save.
    edit_source_view source{};
    std::span<const edit_command> commands{};
    std::size_t memory_budget = 0;

    save_source_fingerprint source_fingerprint{};
    save_destination_baseline destination_baseline{};
};

struct save_result {
    save_error error = save_error::none;
    std::string detail;     // bounded ASCII diagnostic detail
    bool committed = false; // destination now contains the encoded output
    file_fingerprint committed_fingerprint{}; // valid post-commit destination identity
};

// Synchronous bounded Save worker body:
//  1. validates the request and revalidates the captured source fingerprint;
//  2. materializes the captured command sequence exactly once from the
//     immutable canonical source via materialize_edits();
//  3. validates materialized dimensions/stride/byte count and memory admission;
//  4. converts canonical PBGRA8 to straight BGRA (PNG) or opaque 24bpp BGR
//     (JPEG, explicit alpha_not_supported failure otherwise);
//  5. encodes only to a unique same-directory temp file created with
//     CreateFileW(... CREATE_NEW ...);
//  6. revalidates source/destination identity and commits with ReplaceFileW or
//     MoveFileExW (no direct overwrite fallback); ambiguous replacement is
//     commit_state_uncertain and never treated as clean;
//  7. always attempts temp cleanup on every pre-commit failure/cancel path.
//
// `cancel_check` is polled at phase boundaries (before materialization, before
// encoding, and before the replacement step). Once the replacement step has
// begun, cancellation cannot revoke it. Never throws; allocation failures are
// reported as materialization_failed/encode_failed.
[[nodiscard]] save_result execute_save(const save_request& request,
                                       const std::function<bool()>& cancel_check) noexcept;

// Direct deterministic encoder entry points (used by execute_save and focused
// pixel tests). Both take canonical top-down PBGRA8 and write one complete
// file. PNG converts to straight BGRA; JPEG enforces fully opaque pixels.
[[nodiscard]] save_result encode_png_file(const std::filesystem::path& destination,
                                          const std::uint8_t* pixels,
                                          std::int32_t width, std::int32_t height,
                                          std::int32_t stride) noexcept;

[[nodiscard]] save_result encode_jpeg_file(const std::filesystem::path& destination,
                                           const std::uint8_t* pixels,
                                           std::int32_t width, std::int32_t height,
                                           std::int32_t stride, float quality) noexcept;

[[nodiscard]] save_result encode_webp_file(const std::filesystem::path& destination,
                                           const std::uint8_t* pixels,
                                           std::int32_t width, std::int32_t height,
                                           std::int32_t stride,
                                           const std::function<bool()>& cancel_check = {}) noexcept;

[[nodiscard]] save_result encode_tiff_file(const std::filesystem::path& destination,
                                           const std::uint8_t* pixels,
                                           std::int32_t width, std::int32_t height,
                                           std::int32_t stride) noexcept;

[[nodiscard]] save_result encode_bmp_file(const std::filesystem::path& destination,
                                          const std::uint8_t* pixels,
                                          std::int32_t width, std::int32_t height,
                                          std::int32_t stride) noexcept;

// Test seam for simulating durability flush failures (reopen or FlushFileBuffers).
// Thread-safe, scoped, resettable. Production code never activates this.
struct scoped_durability_flush_failure {
    explicit scoped_durability_flush_failure(bool fail_reopen = true);
    ~scoped_durability_flush_failure();
    scoped_durability_flush_failure(const scoped_durability_flush_failure&) = delete;
    scoped_durability_flush_failure& operator=(const scoped_durability_flush_failure&) = delete;
};

// --- save completion acceptance policy ---------------------------------------

// Outcome of evaluating a save completion against the pending save state and
// the live document. Mirrors the UI-thread decision in the viewer exactly so
// the acceptance policy is deterministically testable without a GUI.
enum class save_completion_action {
    none,                       // no state change (unknown/error/cancelled)
    checkpoint,                 // committed same-source match: advance checkpoint + baseline
    saved_unverified_baseline,  // committed but fingerprint missing: warn, remain modified
    saved_commands_changed,     // committed but commands changed during save: warn, remain modified
    saved_silent,               // committed Save As copy: silent, remain dirty
};

struct save_completion_policy_input {
    bool committed = false;
    save_error error = save_error::none;
    bool document_matches = false;
    bool same_source = false;
    std::span<const edit_command> active_commands{};
    std::span<const edit_command> captured_commands{};
    file_fingerprint committed_fingerprint{};
};

// Pure acceptance decision for a save completion. `document_matches` is
// computed by the caller (document present + instance ids match). Never throws.
[[nodiscard]] save_completion_action evaluate_save_completion_policy(
    const save_completion_policy_input& input) noexcept;

// Production request-identity gate for save completions. Mirrors the
// UI-thread guard in the viewer's handle_save_completion exactly so the
// pending/request-ID boundary is deterministically testable: a completion is
// eligible for policy evaluation only when a save is pending AND its request
// ID matches the pending request. Never throws.
[[nodiscard]] bool save_completion_is_for_pending(
    bool has_pending, std::uint64_t pending_request_id,
    std::uint64_t completion_request_id) noexcept;

// Pure pre-submit eligibility for an in-place Save: the destination
// fingerprint must still match the document's authoritative source baseline.
// A document without a valid baseline is eligible (the worker revalidates the
// captured source identity instead). Mirrors the UI-thread check in the
// viewer's start_save exactly so it is deterministically testable.
[[nodiscard]] bool save_pre_submit_eligible(
    const document_source_baseline& baseline,
    const file_fingerprint& destination_fingerprint) noexcept;

}  // namespace oz::viewer_core

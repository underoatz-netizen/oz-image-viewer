// oz/viewer_core/edit_state.hpp - pure non-destructive edit model and history foundation (Stage 5 Slice 1).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace oz::viewer_core {

// Hard bounds from codec model: dimensions must fit within [1, kMaxEditDimension].
inline constexpr std::int32_t kMaxEditDimension = 65535;

// History constraints defined by Stage 5 specification:
// At most 64 undoable transitions and 256 total lifetime commands.
inline constexpr std::size_t kMaxUndoTransitions = 64;
inline constexpr std::size_t kMaxTotalCommands = 256;

// ---------------------------------------------------------------------------
// Canonical Dimensions & Point/Rect Value Types
// ---------------------------------------------------------------------------

struct edit_dimensions {
    std::int32_t width = 0;
    std::int32_t height = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return width > 0 && height > 0 &&
               width <= kMaxEditDimension && height <= kMaxEditDimension;
    }

    [[nodiscard]] constexpr bool operator==(const edit_dimensions& other) const noexcept = default;
};

struct edit_point {
    std::int32_t x = 0;
    std::int32_t y = 0;

    [[nodiscard]] constexpr bool operator==(const edit_point& other) const noexcept = default;
};

// Half-open integer rectangle [x, x + width) x [y, y + height).
struct edit_rect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return width > 0 && height > 0 &&
               x >= 0 && y >= 0 &&
               x <= kMaxEditDimension - width &&
               y <= kMaxEditDimension - height;
    }

    [[nodiscard]] constexpr std::int32_t right() const noexcept {
        return x + width;
    }

    [[nodiscard]] constexpr std::int32_t bottom() const noexcept {
        return y + height;
    }

    [[nodiscard]] constexpr bool operator==(const edit_rect& other) const noexcept = default;
};

// Half-open floating-point rectangle for sub-pixel source cropping / affine proxy presentation.
struct edit_rectf {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return width > 0.0f && height > 0.0f &&
               x >= 0.0f && y >= 0.0f;
    }

    [[nodiscard]] constexpr float right() const noexcept {
        return x + width;
    }

    [[nodiscard]] constexpr float bottom() const noexcept {
        return y + height;
    }

    [[nodiscard]] constexpr bool operator==(const edit_rectf& other) const noexcept = default;
};

// ---------------------------------------------------------------------------
// Discrete D4 Symmetry (Dihedral Group D4)
//
// Represents any orthogonal orientation/reflection as an affine coordinate
// transform on a canvas of dimension (w, h):
//   [x']   [m00 m01] [x]   [tx]
//   [y'] = [m10 m11] [y] + [ty]
//
// Standard basis representations (8 elements):
//   identity:           x' = x,         y' = y
//   rot90_cw (right):   x' = h - 1 - y, y' = x
//   rot180:             x' = w - 1 - x, y' = h - 1 - y
//   rot270_cw (left):   x' = y,         y' = w - 1 - x
//   flip_horizontal:    x' = w - 1 - x, y' = y
//   flip_vertical:      x' = x,         y' = h - 1 - y
//   transpose:          x' = y,         y' = x
//   transverse:         x' = h - 1 - y, y' = w - 1 - x
// ---------------------------------------------------------------------------

struct d4_transform {
    // 2x2 linear matrix entries in {-1, 0, 1}
    std::int32_t m00 = 1;
    std::int32_t m01 = 0;
    std::int32_t m10 = 0;
    std::int32_t m11 = 1;

    // Normalizing translation offsets: [x'] = M * [x] + offset(canvas_dim, M)
    // For integer pixel coordinates in [0, w) x [0, h), x' is mapped back into [0, out_w).
    // Specifically:
    // If column 0 of M is negative (-1), offset_x += (in_w - 1).
    // If column 1 of M is negative (-1), offset_x += (in_h - 1), etc.

    [[nodiscard]] static constexpr d4_transform make_identity() noexcept {
        return {1, 0, 0, 1};
    }

    [[nodiscard]] static constexpr d4_transform make_rot90_cw() noexcept {
        // x' = h - 1 - y, y' = x  => [0 -1; 1 0]
        return {0, -1, 1, 0};
    }

    [[nodiscard]] static constexpr d4_transform make_rot180() noexcept {
        // x' = w - 1 - x, y' = h - 1 - y => [-1 0; 0 -1]
        return {-1, 0, 0, -1};
    }

    [[nodiscard]] static constexpr d4_transform make_rot90_ccw() noexcept {
        // x' = y, y' = w - 1 - x => [0 1; -1 0]
        return {0, 1, -1, 0};
    }

    [[nodiscard]] static constexpr d4_transform make_flip_h() noexcept {
        // x' = w - 1 - x, y' = y => [-1 0; 0 1]
        return {-1, 0, 0, 1};
    }

    [[nodiscard]] static constexpr d4_transform make_flip_v() noexcept {
        // x' = x, y' = h - 1 - y => [1 0; 0 -1]
        return {1, 0, 0, -1};
    }

    [[nodiscard]] constexpr bool swaps_dimensions() const noexcept {
        return m00 == 0;
    }

    [[nodiscard]] constexpr edit_dimensions transform_dimensions(const edit_dimensions& in) const noexcept {
        if (swaps_dimensions()) {
            return {in.height, in.width};
        }
        return in;
    }

    // Compose transforms: (second * first), meaning first applied then second.
    [[nodiscard]] constexpr d4_transform compose(const d4_transform& next) const noexcept {
        return {
            next.m00 * m00 + next.m01 * m10,
            next.m00 * m01 + next.m01 * m11,
            next.m10 * m00 + next.m11 * m10,
            next.m10 * m01 + next.m11 * m11
        };
    }

    // For orthogonal 2x2 matrices with det in {-1, 1}, inverse equals transpose.
    [[nodiscard]] constexpr d4_transform inverse() const noexcept {
        return {m00, m10, m01, m11};
    }

    // Map a discrete pixel coordinate (x, y) on an input canvas of size in_dims.
    [[nodiscard]] constexpr edit_point map_point(const edit_point& p, const edit_dimensions& in_dims) const noexcept {
        // Compute linear part
        std::int32_t lx = m00 * p.x + m01 * p.y;
        std::int32_t ly = m10 * p.x + m11 * p.y;

        // Translation offsets to place coordinates in [0, out_w - 1] x [0, out_h - 1]
        std::int32_t tx = 0;
        if (m00 < 0) tx += in_dims.width - 1;
        if (m01 < 0) tx += in_dims.height - 1;

        std::int32_t ty = 0;
        if (m10 < 0) ty += in_dims.width - 1;
        if (m11 < 0) ty += in_dims.height - 1;

        return {lx + tx, ly + ty};
    }

    [[nodiscard]] constexpr bool operator==(const d4_transform& other) const noexcept = default;
};

// Map a half-open integer rectangle through a D4 transform. Unlike map_point(),
// this works on pixel *bounds* rather than pixel centers: the transform uses
// the input width/height (not width-1/height-1) for reflected axes, preserving
// exact [left, right) x [top, bottom) crop semantics.
//
// `transform` maps from `input_dims` into
// `transform.transform_dimensions(input_dims)`. Returns nullopt when either
// the input rectangle or the transformed result is invalid.
[[nodiscard]] std::optional<edit_rect> map_half_open_rect(
    const d4_transform& transform,
    const edit_rect& rect,
    const edit_dimensions& input_dims) noexcept;

// ---------------------------------------------------------------------------
// Edit Command Definitions
// ---------------------------------------------------------------------------

enum class rotate_direction {
    left_90,   // 90 degrees counter-clockwise
    right_90   // 90 degrees clockwise
};

enum class flip_direction {
    horizontal,
    vertical
};

enum class resize_filter {
    automatic,  // Automatic high-quality per specification (Lanczos / Bicubic)
    bilinear,
    bicubic,
    lanczos
};

enum class resize_unit {
    pixels,
    percent
};

struct rotate_command {
    rotate_direction direction = rotate_direction::right_90;

    [[nodiscard]] constexpr bool operator==(const rotate_command&) const noexcept = default;
};

struct flip_command {
    flip_direction direction = flip_direction::horizontal;

    [[nodiscard]] constexpr bool operator==(const flip_command&) const noexcept = default;
};

// Crop in current command-input canvas coordinates.
// Must be a non-empty half-open rect wholly contained within the input canvas.
struct crop_command {
    edit_rect rect{};

    [[nodiscard]] constexpr bool operator==(const crop_command&) const noexcept = default;
};

// Resize command specifying canonical output dimensions and filter.
struct resize_command {
    edit_dimensions target{};
    resize_filter filter = resize_filter::automatic;

    [[nodiscard]] constexpr bool operator==(const resize_command&) const noexcept = default;
};

// Monomorphic value-type edit operation.
using edit_command = std::variant<rotate_command, flip_command, crop_command, resize_command>;

// ---------------------------------------------------------------------------
// Crop Ratio Helpers & Preset Calculations
// ---------------------------------------------------------------------------

enum class crop_ratio_preset {
    free,
    original,
    square_1x1,
    ratio_3x2,
    ratio_4x3,
    ratio_5x4,
    ratio_16x9,
    ratio_9x16,
    a_series
};

// Mathematical aspect ratio (width / height) for presets.
// Returns nullopt for Free.
// For A-series, standard ISO 216 paper ratio is 1 / sqrt(2) for portrait or sqrt(2) / 1 for landscape.
// The helper adapts to the orientation of the canvas / selection.
[[nodiscard]] std::optional<double> get_crop_aspect_ratio(
    crop_ratio_preset preset,
    const edit_dimensions& canvas_dims,
    bool portrait = false) noexcept;

// Clamp/validate crop rectangle to canvas.
// Returns a valid half-open rectangle clamped within [0, canvas.width) x [0, canvas.height),
// with minimum size 1x1. If input is completely invalid or zero-sized, clamps to canvas or 1x1.
[[nodiscard]] edit_rect clamp_crop_rect(
    const edit_rect& requested,
    const edit_dimensions& canvas_dims) noexcept;

// Compute maximum centered crop rect matching target aspect ratio inside canvas.
[[nodiscard]] edit_rect compute_centered_aspect_crop(
    const edit_dimensions& canvas_dims,
    double target_aspect_ratio) noexcept;

// ---------------------------------------------------------------------------
// Resize Calculation Helpers
// ---------------------------------------------------------------------------

struct resize_calculation_params {
    edit_dimensions current_dims{};
    std::optional<std::int32_t> requested_width{};
    std::optional<std::int32_t> requested_height{};
    std::optional<double> requested_percent{};
    bool maintain_aspect_ratio = true;
    resize_unit unit = resize_unit::pixels;
};

// Computes canonical output dimensions under checked arithmetic and [1, kMaxEditDimension] limits.
// Returns std::nullopt on overflow, zero/negative, or invalid parameters.
[[nodiscard]] std::optional<edit_dimensions> calculate_resize_dimensions(
    const resize_calculation_params& params) noexcept;

// Affine proxy presentation plan calculation (Stage 5 Slice 5):
// Computes an affine proxy mapping from the final logical resized canvas coordinates
// back to the retained immutable source bitmap coordinates.
// Resizing scales logical coordinates back to source coordinates without mutating history.
struct affine_proxy_plan {
    bool valid = false;
    edit_dimensions logical_dimensions{};
    d4_transform transform = d4_transform::make_identity();
    edit_rectf source_rectf{};
};

// Computes the affine proxy plan for a sequence of commands and an optional draft resize.
// Maps crop boundaries and rotations/flips back through inverse transforms and inverse scale factors.
[[nodiscard]] affine_proxy_plan compute_affine_proxy_plan(
    const edit_dimensions& source_dimensions,
    std::span<const edit_command> commands,
    const std::optional<edit_dimensions>& draft_resize = std::nullopt) noexcept;

// ---------------------------------------------------------------------------
// Canvas Dimension & Command Evaluation Math
// ---------------------------------------------------------------------------

// Computes the canvas dimensions after applying a single command to an input canvas.
// Returns std::nullopt if the command is invalid for the given input dimensions (e.g. out-of-bounds crop).
[[nodiscard]] std::optional<edit_dimensions> evaluate_command_dimensions(
    const edit_dimensions& in_dims,
    const edit_command& cmd) noexcept;

// Validates whether a command can be legally applied to an input canvas of given dimensions.
[[nodiscard]] bool is_command_valid(
    const edit_dimensions& in_dims,
    const edit_command& cmd) noexcept;

// ---------------------------------------------------------------------------
// Edit History & State
//
// Authoritative chronological log with Undo/Redo cursor.
// Cursor points to the index in commands_ such that:
// - undoable count = cursor_
// - redoable count = commands_.size() - cursor_
// - active commands applied = commands_[0 ... cursor_ - 1]
//
// Floor/limit behavior:
// - Max 64 undo transitions: If applying a command would exceed 64 undoable commands,
//   the oldest command is pushed off the undo stack floor (oldest command dropped, count remains 64).
// - Max 256 total lifetime commands: Push rejected if total commands reaches 256.
// ---------------------------------------------------------------------------

class edit_history {
public:
    edit_history() = default;
    explicit edit_history(edit_dimensions source_dims) noexcept
        : source_dims_(source_dims) {}

    [[nodiscard]] edit_dimensions source_dimensions() const noexcept {
        return source_dims_;
    }

    void set_source_dimensions(edit_dimensions dims) noexcept {
        source_dims_ = dims;
        clear();
    }

    // Reset history to clean slate
    void clear() noexcept {
        commands_.clear();
        cursor_ = 0;
        undo_floor_ = 0;
        revision_ = 0;
        total_pushed_commands_ = 0;
    }

    // Check if can undo/redo
    [[nodiscard]] bool can_undo() const noexcept {
        return cursor_ > undo_floor_;
    }

    [[nodiscard]] bool can_redo() const noexcept {
        return cursor_ < commands_.size();
    }

    [[nodiscard]] std::size_t undo_count() const noexcept {
        return cursor_ > undo_floor_ ? (cursor_ - undo_floor_) : 0;
    }

    [[nodiscard]] std::size_t redo_count() const noexcept {
        return commands_.size() - cursor_;
    }

    [[nodiscard]] std::size_t total_commands() const noexcept {
        return commands_.size();
    }

    [[nodiscard]] std::size_t undo_floor() const noexcept {
        return undo_floor_;
    }

    [[nodiscard]] std::uint64_t revision() const noexcept {
        return revision_;
    }

    [[nodiscard]] std::size_t lifetime_pushed_commands() const noexcept {
        return total_pushed_commands_;
    }

    [[nodiscard]] bool is_dirty() const noexcept {
        return cursor_ > 0;
    }

    // Get all retained commands in history [0, commands_.size())
    [[nodiscard]] const std::vector<edit_command>& all_commands() const noexcept {
        return commands_;
    }

    // Get active commands [0, cursor_)
    [[nodiscard]] std::span<const edit_command> active_commands() const noexcept {
        return std::span<const edit_command>(commands_.data(), cursor_);
    }

    // Compute active canvas dimensions after applying all commands up to cursor
    [[nodiscard]] std::optional<edit_dimensions> current_dimensions() const noexcept;

    // Push new command.
    // Truncates any redo commands beyond cursor.
    // Recomputes undo floor: max(undo_floor, cursor - 64).
    // Returns true on success, false if rejected (exceeds lifetime limit or command invalid on current canvas).
    bool push_command(edit_command cmd);

    // Undo 1 transition. Returns true if undone, false if cannot undo.
    bool undo() noexcept;

    // Redo 1 transition. Returns true if redone, false if cannot redo.
    bool redo() noexcept;

private:
    edit_dimensions source_dims_{};
    std::vector<edit_command> commands_{};
    std::size_t cursor_ = 0;
    std::size_t undo_floor_ = 0;
    std::uint64_t revision_ = 0;
    std::size_t total_pushed_commands_ = 0;
};

}  // namespace oz::viewer_core

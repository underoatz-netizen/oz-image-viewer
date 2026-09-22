// oz/viewer_core/edit_pipeline.hpp - bounded pure CPU edit materialization pipeline (Stage 5 Slice 2).
#pragma once

#include "oz/viewer_core/edit_state.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace oz::viewer_core {

// Error categories for edit materialization pipeline
enum class edit_pipeline_error {
    none,
    invalid_source,       // null or invalid source dimensions / stride / buffer
    invalid_command,      // out-of-bounds crop or illegal dimension target
    memory_limit,         // exceeds admission budget or max decoded bytes limit (2 GiB)
    allocation_failed,    // memory allocation threw bad_alloc
    scaler_failed         // WIC COM / scaler error
};

// Result of materializing active edit commands over an immutable source image
struct edit_pipeline_result {
    edit_pipeline_error error = edit_pipeline_error::none;
    std::string detail;
    std::vector<std::uint8_t> pixels; // canonical PBGRA8, top-down
    edit_dimensions dimensions{};
    std::int32_t stride = 0;
};

// Memory admission policy for materialization.
// Computes maximum allowed bytes for edit materialization buffers:
// min(1 GiB, available physical memory / 4).
[[nodiscard]] std::size_t compute_edit_admission_budget(std::uint64_t available_phys_bytes) noexcept;

// Query current system available physical memory via GlobalMemoryStatusEx and compute budget.
[[nodiscard]] std::size_t get_system_edit_admission_budget() noexcept;

// Read-only view of a canonical source image in top-down SDR sRGB premultiplied BGRA8 (PBGRA8).
struct edit_source_view {
    std::span<const std::uint8_t> pixels{};
    edit_dimensions dimensions{};
    std::int32_t stride = 0;

    [[nodiscard]] bool is_valid() const noexcept {
        if (!dimensions.is_valid()) return false;
        if (stride < dimensions.width * 4) return false;
        const std::size_t min_bytes = static_cast<std::size_t>(stride) * static_cast<std::size_t>(dimensions.height);
        return pixels.size() >= min_bytes && pixels.data() != nullptr;
    }
};

// Pure CPU bounded edit materialization.
// - Takes an immutable canonical PBGRA8 top-down source and an active sequence of commands.
// - Enforces checked arithmetic, <=65535 dims, <=2GiB output, and memory admission budget.
// - Rotate 90 CW/CCW, flips, and crop are executed via bounded CPU pixel kernels.
// - Resize is executed codec-independently with WIC on 32bppPBGRA:
//     * exact same dimensions: exact copy
//     * downscale: WICBitmapInterpolationModeFant
//     * upscale: WICBitmapInterpolationModeHighQualityCubic
// - Returns canonical PBGRA8 result, leaving source and caller state untouched on error.
[[nodiscard]] edit_pipeline_result materialize_edits(
    const edit_source_view& source,
    std::span<const edit_command> active_commands,
    std::size_t memory_budget_bytes = 0) noexcept;

}  // namespace oz::viewer_core

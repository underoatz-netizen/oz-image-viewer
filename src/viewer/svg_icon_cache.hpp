// oz/viewer/svg_icon_cache.hpp - SVG icon resources and Direct2D lazy render cache (Stage 7 modern chrome).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef small
#undef small
#endif
#include <d2d1_1.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace oz::viewer {

using Microsoft::WRL::ComPtr;

enum class icon_id : std::uint8_t {
    open,
    save,
    undo,
    redo,
    rotate_left,
    rotate_right,
    crop,
    resize,
    fit,
    one_to_one,
    fullscreen
};

// Returns standard SVG XML text for an icon
[[nodiscard]] std::string_view get_svg_icon_data(icon_id id) noexcept;

// Short text fallback label for an icon when SVG drawing or Direct2D 1.3 is unavailable
[[nodiscard]] std::wstring_view get_icon_fallback_label(icon_id id) noexcept;

// Cache key for rasterized icon bitmaps
struct icon_cache_key {
    icon_id id;
    std::int32_t size_px;
    COLORREF color;

    bool operator==(const icon_cache_key& o) const noexcept {
        return id == o.id && size_px == o.size_px && color == o.color;
    }
};

struct icon_cache_key_hash {
    std::size_t operator()(const icon_cache_key& k) const noexcept {
        return (static_cast<std::size_t>(k.id) << 24) ^
               (static_cast<std::size_t>(k.size_px) << 16) ^
               static_cast<std::size_t>(k.color);
    }
};

// Lazy, on-demand SVG icon renderer and bitmap cache using Direct2D (ID2D1DeviceContext5 SVG support or software D2D fallback).
class svg_icon_cache {
public:
    svg_icon_cache() = default;
    ~svg_icon_cache() = default;

    svg_icon_cache(const svg_icon_cache&) = delete;
    svg_icon_cache& operator=(const svg_icon_cache&) = delete;

    // Check if Direct2D SVG support is available on this system
    [[nodiscard]] bool is_svg_supported(ID2D1DeviceContext* d2d_context) noexcept;

    // Retrieve or rasterize cached D2D bitmap for the icon
    [[nodiscard]] ID2D1Bitmap* get_or_render_icon(ID2D1DeviceContext* d2d_context,
                                                  icon_id id,
                                                  std::int32_t size_px,
                                                  COLORREF color);

    // Rasterize icon directly into an RGBA/BGRA 32-bit pixel buffer for native HBITMAP / ImageList integration
    [[nodiscard]] bool rasterize_icon_pixels(ID2D1DeviceContext* d2d_context,
                                            icon_id id,
                                            std::int32_t size_px,
                                            COLORREF color,
                                            void* out_pixels_bgra32);

    // Invalidate cached bitmaps (e.g. on DPI change or theme change)
    void clear() noexcept {
        cache_.clear();
    }

private:
    std::unordered_map<icon_cache_key, ComPtr<ID2D1Bitmap>, icon_cache_key_hash> cache_;
    bool checked_svg_support_ = false;
    bool has_svg_support_ = false;
};

}  // namespace oz::viewer

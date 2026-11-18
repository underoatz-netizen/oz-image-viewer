// oz/viewer/lucide_icons.hpp - Official Lucide 1.71.0 icon artwork and GDI+ stroke renderer.
//
// Every Lucide icon in this file carries the verbatim element body from
// upstream Lucide 1.71.0 (https://github.com/lucide-icons/lucide/releases/tag/1.71.0).
// Provenance and licences: third_party/lucide/LUCIDE_LOCK.md and THIRD-PARTY-NOTICES.md.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#ifdef small
#undef small
#endif
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Windows SDK <gdiplus.h> uses unqualified min/max in GdiplusTypes.h when
// NOMINMAX is defined unless std::min/std::max are brought in.
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

namespace oz::viewer {

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
    fullscreen,
    paste_image,
    save_as,
    open_location,
    copy_image,
    copy_file,
    copy_path,
    print,
    delete_image,
    flip_h,
    flip_v,
    zoom_in,
    zoom_out,
    nav_first,
    nav_prev,
    nav_next,
    nav_last,
    overflow,
    crop_free,
    crop_original,
    crop_1x1,
    crop_3x2,
    crop_4x3,
    crop_5x4,
    crop_16x9,
    crop_9x16,
    crop_a_series,
    crop_reset,
    crop_apply,
    crop_cancel
};

inline constexpr std::size_t kIconIdCount = 40;

namespace lucide {

inline constexpr int kGridUnits = 24;
inline constexpr double kStrokeWidthUnits = 2.0;

inline constexpr const char* kUpstreamVersion = "1.71.0";
inline constexpr const char* kUpstreamRepository = "https://github.com/lucide-icons/lucide";

struct icon_asset {
    icon_id id = icon_id::open;
    const char* name = "";
    const char* markup = "";
    bool feather_derived = false;
    bool upstream_lucide = true;
};

inline constexpr icon_asset kAssets[kIconIdCount] = {
    {icon_id::open, "folder-open",
     "<path d=\"m6 14 1.5-2.9A2 2 0 0 1 9.24 10H20a2 2 0 0 1 1.94 2.5l-1.54 6a2 2 0 0 1-1.95 1.5H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h3.9a2 2 0 0 1 1.69.9l.81 1.2a2 2 0 0 0 1.67.9H18a2 2 0 0 1 2 2v2\" />",
     false, true},
    {icon_id::save, "save",
     "<path d=\"M15.2 3a2 2 0 0 1 1.4.6l3.8 3.8a2 2 0 0 1 .6 1.4V19a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z\" />\n"
     "<path d=\"M17 21v-7a1 1 0 0 0-1-1H8a1 1 0 0 0-1 1v7\" />\n"
     "<path d=\"M7 3v4a1 1 0 0 0 1 1h7\" />",
     false, true},
    {icon_id::undo, "undo-2",
     "<path d=\"M9 14 4 9l5-5\" />\n"
     "<path d=\"M4 9h10.5a5.5 5.5 0 0 1 5.5 5.5a5.5 5.5 0 0 1-5.5 5.5H11\" />",
     false, true},
    {icon_id::redo, "redo-2",
     "<path d=\"m15 14 5-5-5-5\" />\n"
     "<path d=\"M20 9H9.5A5.5 5.5 0 0 0 4 14.5A5.5 5.5 0 0 0 9.5 20H13\" />",
     false, true},
    {icon_id::rotate_left, "rotate-ccw",
     "<path d=\"M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8\" />\n"
     "<path d=\"M3 3v5h5\" />",
     false, true},
    {icon_id::rotate_right, "rotate-cw",
     "<path d=\"M21 12a9 9 0 1 1-9-9c2.52 0 4.93 1 6.74 2.74L21 8\" />\n"
     "<path d=\"M21 3v5h-5\" />",
     false, true},
    {icon_id::crop, "crop",
     "<path d=\"M6 2v14a2 2 0 0 0 2 2h14\" />\n"
     "<path d=\"M18 22V8a2 2 0 0 0-2-2H2\" />",
     false, true},
    {icon_id::resize, "scaling",
     "<path d=\"M12 3H5a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7\" />\n"
     "<path d=\"M14 15H9v-5\" />\n"
     "<path d=\"M16 3h5v5\" />\n"
     "<path d=\"M21 3 9 15\" />",
     false, true},
    {icon_id::fit, "scan",
     "<path d=\"M3 7V5a2 2 0 0 1 2-2h2\" />\n"
     "<path d=\"M17 3h2a2 2 0 0 1 2 2v2\" />\n"
     "<path d=\"M21 17v2a2 2 0 0 1-2 2h-2\" />\n"
     "<path d=\"M7 21H5a2 2 0 0 1-2-2v-2\" />",
     false, true},
    {icon_id::one_to_one, "maximize-2",
     "<path d=\"M15 3h6v6\" />\n"
     "<path d=\"m21 3-7 7\" />\n"
     "<path d=\"m3 21 7-7\" />\n"
     "<path d=\"M9 21H3v-6\" />",
     false, true},
    {icon_id::fullscreen, "fullscreen",
     "<path d=\"M3 7V5a2 2 0 0 1 2-2h2\" />\n"
     "<path d=\"M17 3h2a2 2 0 0 1 2 2v2\" />\n"
     "<path d=\"M21 17v2a2 2 0 0 1-2 2h-2\" />\n"
     "<path d=\"M7 21H5a2 2 0 0 1-2-2v-2\" />\n"
     "<rect width=\"10\" height=\"8\" x=\"7\" y=\"8\" rx=\"1\" />",
     false, true},
    {icon_id::paste_image, "clipboard-paste",
     "<path d=\"M11 14h10\" />\n"
     "<path d=\"M16 4h2a2 2 0 0 1 2 2v1.344\" />\n"
     "<path d=\"m17 18 4-4-4-4\" />\n"
     "<path d=\"M8 4H6a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h12a2 2 0 0 0 1.793-1.113\" />\n"
     "<rect x=\"8\" y=\"2\" width=\"8\" height=\"4\" rx=\"1\" />",
     false, true},
    {icon_id::save_as, "save-all",
     "<path d=\"M10 2v3a1 1 0 0 0 1 1h5\" />\n"
     "<path d=\"M18 18v-6a1 1 0 0 0-1-1h-6a1 1 0 0 0-1 1v6\" />\n"
     "<path d=\"M18 22H4a2 2 0 0 1-2-2V6\" />\n"
     "<path d=\"M8 18a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9.172a2 2 0 0 1 1.414.586l2.828 2.828A2 2 0 0 1 22 6.828V16a2 2 0 0 1-2.01 2z\" />",
     false, true},
    {icon_id::open_location, "folder-search",
     "<path d=\"M10.7 20H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h3.9a2 2 0 0 1 1.69.9l.81 1.2a2 2 0 0 0 1.67.9H20a2 2 0 0 1 2 2v4.1\" />\n"
     "<path d=\"m21 21-1.9-1.9\" />\n"
     "<circle cx=\"17\" cy=\"17\" r=\"3\" />",
     false, true},
    {icon_id::copy_image, "image",
     "<rect width=\"18\" height=\"18\" x=\"3\" y=\"3\" rx=\"2\" ry=\"2\" />\n"
     "<circle cx=\"9\" cy=\"9\" r=\"2\" />\n"
     "<path d=\"m21 15-3.086-3.086a2 2 0 0 0-2.828 0L6 21\" />",
     false, true},
    {icon_id::copy_file, "files",
     "<path d=\"M15 2h-4a2 2 0 0 0-2 2v11a2 2 0 0 0 2 2h8a2 2 0 0 0 2-2V8\" />\n"
     "<path d=\"M16.706 2.706A2.4 2.4 0 0 0 15 2v5a1 1 0 0 0 1 1h5a2.4 2.4 0 0 0-.706-1.706z\" />\n"
     "<path d=\"M5 7a2 2 0 0 0-2 2v11a2 2 0 0 0 2 2h8a2 2 0 0 0 1.732-1\" />",
     false, true},
    {icon_id::copy_path, "link",
     "<path d=\"M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71\" />\n"
     "<path d=\"M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71\" />",
     true, true},
    {icon_id::print, "printer",
     "<path d=\"M6 18H4a2 2 0 0 1-2-2v-5a2 2 0 0 1 2-2h16a2 2 0 0 1 2 2v5a2 2 0 0 1-2 2h-2\" />\n"
     "<path d=\"M6 9V3a1 1 0 0 1 1-1h10a1 1 0 0 1 1 1v6\" />\n"
     "<rect x=\"6\" y=\"14\" width=\"12\" height=\"8\" rx=\"1\" />",
     false, true},
    {icon_id::delete_image, "trash",
     "<path d=\"M10 11v6\" />\n"
     "<path d=\"M14 11v6\" />\n"
     "<path d=\"M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6\" />\n"
     "<path d=\"M3 6h18\" />\n"
     "<path d=\"M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2\" />",
     true, true},
    {icon_id::flip_h, "fold-horizontal",
     "<path d=\"M2 12h6\" />\n"
     "<path d=\"M22 12h-6\" />\n"
     "<path d=\"M12 2v2\" />\n"
     "<path d=\"M12 8v2\" />\n"
     "<path d=\"M12 14v2\" />\n"
     "<path d=\"M12 20v2\" />\n"
     "<path d=\"m19 9-3 3 3 3\" />\n"
     "<path d=\"m5 15 3-3-3-3\" />",
     false, true},
    {icon_id::flip_v, "fold-vertical",
     "<path d=\"M12 22v-6\" />\n"
     "<path d=\"M12 8V2\" />\n"
     "<path d=\"M4 12H2\" />\n"
     "<path d=\"M10 12H8\" />\n"
     "<path d=\"M16 12h-2\" />\n"
     "<path d=\"M22 12h-2\" />\n"
     "<path d=\"m15 19-3-3-3 3\" />\n"
     "<path d=\"m15 5-3 3-3-3\" />",
     false, true},
    {icon_id::zoom_in, "zoom-in",
     "<circle cx=\"11\" cy=\"11\" r=\"8\" />\n"
     "<line x1=\"21\" x2=\"16.65\" y1=\"21\" y2=\"16.65\" />\n"
     "<line x1=\"11\" x2=\"11\" y1=\"8\" y2=\"14\" />\n"
     "<line x1=\"8\" x2=\"14\" y1=\"11\" y2=\"11\" />",
     true, true},
    {icon_id::zoom_out, "zoom-out",
     "<circle cx=\"11\" cy=\"11\" r=\"8\" />\n"
     "<line x1=\"21\" x2=\"16.65\" y1=\"21\" y2=\"16.65\" />\n"
     "<line x1=\"8\" x2=\"14\" y1=\"11\" y2=\"11\" />",
     true, true},
    {icon_id::nav_first, "chevrons-left",
     "<path d=\"m11 17-5-5 5-5\" />\n"
     "<path d=\"m18 17-5-5 5-5\" />",
     true, true},
    {icon_id::nav_prev, "chevron-left",
     "<path d=\"m15 18-6-6 6-6\" />",
     true, true},
    {icon_id::nav_next, "chevron-right",
     "<path d=\"m9 18 6-6-6-6\" />",
     true, true},
    {icon_id::nav_last, "chevrons-right",
     "<path d=\"m6 17 5-5-5-5\" />\n"
     "<path d=\"m13 17 5-5-5-5\" />",
     true, true},
    {icon_id::overflow, "ellipsis-vertical",
     "<circle cx=\"12\" cy=\"12\" r=\"1\" />\n"
     "<circle cx=\"12\" cy=\"5\" r=\"1\" />\n"
     "<circle cx=\"12\" cy=\"19\" r=\"1\" />",
     false, true},
    {icon_id::crop_free, "square-dashed",
     "<path d=\"M5 3a2 2 0 0 0-2 2\" />\n"
     "<path d=\"M19 3a2 2 0 0 1 2 2\" />\n"
     "<path d=\"M21 19a2 2 0 0 1-2 2\" />\n"
     "<path d=\"M5 21a2 2 0 0 1-2-2\" />\n"
     "<path d=\"M9 3h1\" />\n"
     "<path d=\"M9 21h1\" />\n"
     "<path d=\"M14 3h1\" />\n"
     "<path d=\"M14 21h1\" />\n"
     "<path d=\"M3 9v1\" />\n"
     "<path d=\"M21 9v1\" />\n"
     "<path d=\"M3 14v1\" />\n"
     "<path d=\"M21 14v1\" />",
     false, true},
    {icon_id::crop_original, "proportions",
     "<rect width=\"20\" height=\"16\" x=\"2\" y=\"4\" rx=\"2\" />\n"
     "<path d=\"M12 9v11\" />\n"
     "<path d=\"M2 9h13a2 2 0 0 1 2 2v9\" />",
     false, true},
    {icon_id::crop_1x1, "square",
     "<rect width=\"18\" height=\"18\" x=\"3\" y=\"3\" rx=\"2\" />",
     true, true},
    {icon_id::crop_3x2, "aspect-3x2",
     "<rect width=\"18\" height=\"12\" x=\"3\" y=\"6\" rx=\"2\" />",
     false, false},
    {icon_id::crop_4x3, "aspect-4x3",
     "<rect width=\"16\" height=\"12\" x=\"4\" y=\"6\" rx=\"2\" />",
     false, false},
    {icon_id::crop_5x4, "aspect-5x4",
     "<rect width=\"15\" height=\"12\" x=\"4.5\" y=\"6\" rx=\"2\" />",
     false, false},
    {icon_id::crop_16x9, "rectangle-horizontal",
     "<rect width=\"20\" height=\"12\" x=\"2\" y=\"6\" rx=\"2\" />",
     false, true},
    {icon_id::crop_9x16, "rectangle-vertical",
     "<rect width=\"12\" height=\"20\" x=\"6\" y=\"2\" rx=\"2\" />",
     false, true},
    {icon_id::crop_a_series, "file",
     "<path d=\"M6 22a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h8a2.4 2.4 0 0 1 1.704.706l3.588 3.588A2.4 2.4 0 0 1 20 8v12a2 2 0 0 1-2 2z\" />\n"
     "<path d=\"M14 2v5a1 1 0 0 0 1 1h5\" />",
     false, true},
    {icon_id::crop_reset, "refresh-ccw",
     "<path d=\"M21 12a9 9 0 0 0-9-9 9.75 9.75 0 0 0-6.74 2.74L3 8\" />\n"
     "<path d=\"M3 3v5h5\" />\n"
     "<path d=\"M3 12a9 9 0 0 0 9 9 9.75 9.75 0 0 0 6.74-2.74L21 16\" />\n"
     "<path d=\"M16 16h5v5\" />",
     false, true},
    {icon_id::crop_apply, "check",
     "<path d=\"M20 6 9 17l-5-5\" />",
     true, true},
    {icon_id::crop_cancel, "x",
     "<path d=\"M18 6 6 18\" />\n"
     "<path d=\"m6 6 12 12\" />",
     true, true},
};

inline const icon_asset* FindAsset(icon_id id) noexcept {
    for (const icon_asset& asset : kAssets) {
        if (asset.id == id) {
            return &asset;
        }
    }
    return nullptr;
}

inline const icon_asset& Asset(icon_id id) noexcept {
    const icon_asset* found = FindAsset(id);
    return found != nullptr ? *found : kAssets[0];
}

struct Point {
    double x = 0.0;
    double y = 0.0;
};

struct Segment {
    enum class Kind { Line, Cubic, Ellipse };
    Kind kind = Kind::Line;
    Point from;
    Point control1;
    Point control2;
    Point to;
};

struct Figure {
    std::vector<Segment> segments;
    bool closed = false;
};

struct Bounds {
    double minX = 0.0;
    double minY = 0.0;
    double maxX = 0.0;
    double maxY = 0.0;
    bool valid = false;
};

namespace detail {

inline constexpr double kPi = 3.14159265358979323846;

inline bool IsSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',';
}

inline char ToLower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool EqualsIgnoreCase(const std::string& a, const char* b) noexcept {
    size_t i = 0;
    for (; i < a.size() && b[i] != '\0'; ++i) {
        if (ToLower(a[i]) != ToLower(b[i])) {
            return false;
        }
    }
    return i == a.size() && b[i] == '\0';
}

inline std::string Attribute(const std::string& attributes, const char* name) {
    size_t pos = 0;
    while (pos < attributes.size()) {
        const size_t equals = attributes.find('=', pos);
        if (equals == std::string::npos) {
            return std::string();
        }
        size_t keyStart = equals;
        while (keyStart > 0 && !IsSpace(attributes[keyStart - 1])) {
            --keyStart;
        }
        const std::string key = attributes.substr(keyStart, equals - keyStart);

        size_t valuePos = equals + 1;
        while (valuePos < attributes.size() && IsSpace(attributes[valuePos])) {
            ++valuePos;
        }
        if (valuePos >= attributes.size()) {
            return std::string();
        }
        const char quote = attributes[valuePos];
        std::string value;
        if (quote == '"' || quote == '\'') {
            const size_t valueEnd = attributes.find(quote, valuePos + 1);
            if (valueEnd == std::string::npos) {
                return std::string();
            }
            value = attributes.substr(valuePos + 1, valueEnd - valuePos - 1);
            pos = valueEnd + 1;
        } else {
            size_t valueEnd = valuePos;
            while (valueEnd < attributes.size() && !IsSpace(attributes[valueEnd])) {
                ++valueEnd;
            }
            value = attributes.substr(valuePos, valueEnd - valuePos);
            pos = valueEnd;
        }
        if (EqualsIgnoreCase(key, name)) {
            return value;
        }
    }
    return std::string();
}

inline bool ReadNumber(const std::string& text, size_t& pos, double& value) {
    while (pos < text.size() && IsSpace(text[pos])) {
        ++pos;
    }
    const size_t start = pos;
    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
        ++pos;
    }
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        ++pos;
    }
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            ++pos;
        }
    }
    if (pos == start) {
        return false;
    }
    if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
        const size_t exponentStart = pos;
        ++pos;
        if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
            ++pos;
        }
        const size_t digitsStart = pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            ++pos;
        }
        if (pos == digitsStart) {
            pos = exponentStart;
        }
    }
    try {
        value = std::stod(text.substr(start, pos - start));
    } catch (...) {
        return false;
    }
    return true;
}

inline void SampleArc(Point from, double rx, double ry, double rotationDeg,
                      bool largeArc, bool sweep, Point to,
                      std::vector<Point>& out) {
    if (from.x == to.x && from.y == to.y) {
        return;
    }
    rx = std::fabs(rx);
    ry = std::fabs(ry);
    if (rx == 0.0 || ry == 0.0) {
        out.push_back(to);
        return;
    }

    const double phi = rotationDeg * kPi / 180.0;
    const double cosPhi = std::cos(phi);
    const double sinPhi = std::sin(phi);
    const double dx = (from.x - to.x) / 2.0;
    const double dy = (from.y - to.y) / 2.0;
    const double x1p = cosPhi * dx + sinPhi * dy;
    const double y1p = -sinPhi * dx + cosPhi * dy;

    const double lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1.0) {
        const double grown = std::sqrt(lambda);
        rx *= grown;
        ry *= grown;
    }

    const double sign = (largeArc == sweep) ? -1.0 : 1.0;
    const double numerator = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    const double denominator = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    double coefficient = 0.0;
    if (denominator > 0.0) {
        coefficient = sign * std::sqrt((std::max)(0.0, numerator / denominator));
    }
    const double cxp = coefficient * (rx * y1p / ry);
    const double cyp = coefficient * (-ry * x1p / rx);
    const double cx = cosPhi * cxp - sinPhi * cyp + (from.x + to.x) / 2.0;
    const double cy = sinPhi * cxp + cosPhi * cyp + (from.y + to.y) / 2.0;

    const double ux = (x1p - cxp) / rx;
    const double uy = (y1p - cyp) / ry;
    const double vx = (-x1p - cxp) / rx;
    const double vy = (-y1p - cyp) / ry;

    const double theta1 = std::atan2(uy, ux);
    double delta = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    if (!sweep && delta > 0.0) {
        delta -= 2.0 * kPi;
    } else if (sweep && delta < 0.0) {
        delta += 2.0 * kPi;
    }

    const int steps = (std::max)(2, static_cast<int>(std::ceil(std::fabs(delta) / (kPi / 36.0))));
    for (int i = 1; i <= steps; ++i) {
        const double theta = theta1 + delta * (static_cast<double>(i) / static_cast<double>(steps));
        const double cosT = std::cos(theta);
        const double sinT = std::sin(theta);
        out.push_back(Point{cx + rx * cosPhi * cosT - ry * sinPhi * sinT,
                            cy + rx * sinPhi * cosT + ry * cosPhi * sinT});
    }
}

inline void SampleCentreArc(double cx, double cy, double rx, double ry,
                            double startDeg, double sweepDeg,
                            std::vector<Point>& out) {
    const int steps = 12;
    for (int i = 1; i <= steps; ++i) {
        const double angle = (startDeg + sweepDeg * (static_cast<double>(i) / static_cast<double>(steps))) * kPi / 180.0;
        out.push_back(Point{cx + rx * std::cos(angle), cy + ry * std::sin(angle)});
    }
}

class MarkupReader {
public:
    explicit MarkupReader(const std::string& markup) : text_(markup) {}

    bool Next(std::string& tag, std::string& attributes) {
        while (cursor_ < text_.size()) {
            const size_t open = text_.find('<', cursor_);
            if (open == std::string::npos) {
                cursor_ = text_.size();
                return false;
            }
            cursor_ = open + 1;
            if (cursor_ < text_.size() && (text_[cursor_] == '!' || text_[cursor_] == '?')) {
                const size_t end = text_.find('>', cursor_);
                if (end == std::string::npos) {
                    cursor_ = text_.size();
                    return false;
                }
                cursor_ = end + 1;
                continue;
            }
            if (cursor_ < text_.size() && text_[cursor_] == '/') {
                const size_t end = text_.find('>', cursor_);
                if (end == std::string::npos) {
                    cursor_ = text_.size();
                    return false;
                }
                cursor_ = end + 1;
                continue;
            }

            const size_t nameStart = cursor_;
            while (cursor_ < text_.size() && !IsSpace(text_[cursor_]) &&
                   text_[cursor_] != '>' && text_[cursor_] != '/') {
                ++cursor_;
            }
            tag = text_.substr(nameStart, cursor_ - nameStart);

            const size_t attributesStart = cursor_;
            const size_t close = text_.find('>', cursor_);
            if (close == std::string::npos) {
                cursor_ = text_.size();
                return false;
            }
            attributes = text_.substr(attributesStart, close - attributesStart);
            cursor_ = close + 1;
            if (tag.empty()) {
                continue;
            }
            return true;
        }
        return false;
    }

private:
    std::string text_;
    size_t cursor_ = 0;
};

inline bool ParsePathData(const std::string& data, std::vector<Figure>& figures, std::string* error) {
    Figure current;
    bool open = false;
    Point cursor{0.0, 0.0};
    Point figureStart{0.0, 0.0};
    char command = '\0';
    size_t pos = 0;

    const auto fail = [&](const char* reason) {
        if (error != nullptr) {
            *error = reason;
        }
        return false;
    };
    const auto flush = [&]() {
        if (!current.segments.empty()) {
            figures.push_back(current);
        }
        current = Figure{};
    };

    while (pos < data.size()) {
        while (pos < data.size() && IsSpace(data[pos])) {
            ++pos;
        }
        if (pos >= data.size()) {
            break;
        }
        const char c = data[pos];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            command = c;
            ++pos;
        } else if (command == '\0') {
            return fail("path data must start with a command");
        }

        switch (command) {
            case 'M':
            case 'm': {
                double x = 0.0, y = 0.0;
                if (!ReadNumber(data, pos, x) || !ReadNumber(data, pos, y)) {
                    return fail("moveto needs two coordinates");
                }
                if (command == 'm') {
                    x += cursor.x;
                    y += cursor.y;
                }
                flush();
                cursor = Point{x, y};
                figureStart = cursor;
                open = true;
                command = (command == 'M') ? 'L' : 'l';
                break;
            }
            case 'L':
            case 'l': {
                double x = 0.0, y = 0.0;
                if (!ReadNumber(data, pos, x) || !ReadNumber(data, pos, y)) {
                    return fail("lineto needs two coordinates");
                }
                if (command == 'l') {
                    x += cursor.x;
                    y += cursor.y;
                }
                if (!open) {
                    return fail("lineto without a moveto");
                }
                Segment segment{Segment::Kind::Line, cursor, {}, {}, Point{x, y}};
                current.segments.push_back(segment);
                cursor = segment.to;
                break;
            }
            case 'H':
            case 'h': {
                double x = 0.0;
                if (!ReadNumber(data, pos, x)) {
                    return fail("horizontal lineto needs one coordinate");
                }
                if (command == 'h') {
                    x += cursor.x;
                }
                if (!open) {
                    return fail("horizontal lineto without a moveto");
                }
                Segment segment{Segment::Kind::Line, cursor, {}, {}, Point{x, cursor.y}};
                current.segments.push_back(segment);
                cursor = segment.to;
                break;
            }
            case 'V':
            case 'v': {
                double y = 0.0;
                if (!ReadNumber(data, pos, y)) {
                    return fail("vertical lineto needs one coordinate");
                }
                if (command == 'v') {
                    y += cursor.y;
                }
                if (!open) {
                    return fail("vertical lineto without a moveto");
                }
                Segment segment{Segment::Kind::Line, cursor, {}, {}, Point{cursor.x, y}};
                current.segments.push_back(segment);
                cursor = segment.to;
                break;
            }
            case 'C':
            case 'c': {
                double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0, x = 0.0, y = 0.0;
                if (!ReadNumber(data, pos, x1) || !ReadNumber(data, pos, y1) ||
                    !ReadNumber(data, pos, x2) || !ReadNumber(data, pos, y2) ||
                    !ReadNumber(data, pos, x) || !ReadNumber(data, pos, y)) {
                    return fail("cubic curve needs six coordinates");
                }
                if (command == 'c') {
                    x1 += cursor.x; y1 += cursor.y;
                    x2 += cursor.x; y2 += cursor.y;
                    x += cursor.x; y += cursor.y;
                }
                if (!open) {
                    return fail("cubic curve without a moveto");
                }
                Segment segment{Segment::Kind::Cubic, cursor, Point{x1, y1}, Point{x2, y2}, Point{x, y}};
                current.segments.push_back(segment);
                cursor = segment.to;
                break;
            }
            case 'A':
            case 'a': {
                double rx = 0.0, ry = 0.0, rotation = 0.0, largeArc = 0.0, sweep = 0.0, x = 0.0, y = 0.0;
                if (!ReadNumber(data, pos, rx) || !ReadNumber(data, pos, ry) ||
                    !ReadNumber(data, pos, rotation) || !ReadNumber(data, pos, largeArc) ||
                    !ReadNumber(data, pos, sweep) || !ReadNumber(data, pos, x) ||
                    !ReadNumber(data, pos, y)) {
                    return fail("elliptical arc needs seven coordinates");
                }
                if (command == 'a') {
                    x += cursor.x;
                    y += cursor.y;
                }
                if (!open) {
                    return fail("elliptical arc without a moveto");
                }
                std::vector<Point> sampled;
                SampleArc(cursor, rx, ry, rotation, largeArc != 0.0, sweep != 0.0, Point{x, y}, sampled);
                Point previous = cursor;
                for (const Point& point : sampled) {
                    current.segments.push_back(Segment{Segment::Kind::Line, previous, {}, {}, point});
                    previous = point;
                }
                cursor = Point{x, y};
                break;
            }
            case 'Z':
            case 'z': {
                if (!open) {
                    return fail("closepath without a moveto");
                }
                current.segments.push_back(Segment{Segment::Kind::Line, cursor, {}, {}, figureStart});
                cursor = figureStart;
                current.closed = true;
                flush();
                open = false;
                break;
            }
            default:
                return fail("unsupported path command");
        }
    }
    flush();
    return true;
}

inline bool ParseAsset(const icon_asset& asset, std::vector<Figure>& figures, std::string* error) {
    const auto fail = [&](const char* reason) {
        if (error != nullptr) {
            *error = reason;
        }
        return false;
    };

    MarkupReader reader(asset.markup);
    std::string tag;
    std::string attributes;
    while (reader.Next(tag, attributes)) {
        if (EqualsIgnoreCase(tag, "path")) {
            if (!ParsePathData(Attribute(attributes, "d"), figures, error)) {
                return false;
            }
            continue;
        }
        if (EqualsIgnoreCase(tag, "circle")) {
            const double cx = std::stod(Attribute(attributes, "cx"));
            const double cy = std::stod(Attribute(attributes, "cy"));
            const double r = std::stod(Attribute(attributes, "r"));
            Figure figure;
            figure.segments.push_back(Segment{Segment::Kind::Ellipse, Point{cx, cy}, {}, {}, Point{r, r}});
            figures.push_back(figure);
            continue;
        }
        if (EqualsIgnoreCase(tag, "line")) {
            const double x1 = std::stod(Attribute(attributes, "x1"));
            const double y1 = std::stod(Attribute(attributes, "y1"));
            const double x2 = std::stod(Attribute(attributes, "x2"));
            const double y2 = std::stod(Attribute(attributes, "y2"));
            Figure figure;
            figure.segments.push_back(Segment{Segment::Kind::Line, Point{x1, y1}, {}, {}, Point{x2, y2}});
            figures.push_back(figure);
            continue;
        }
        if (EqualsIgnoreCase(tag, "rect")) {
            const double x = std::stod(Attribute(attributes, "x"));
            const double y = std::stod(Attribute(attributes, "y"));
            const double width = std::stod(Attribute(attributes, "width"));
            const double height = std::stod(Attribute(attributes, "height"));
            double rx = 0.0;
            double ry = 0.0;
            if (!Attribute(attributes, "rx").empty()) {
                rx = std::stod(Attribute(attributes, "rx"));
            }
            if (!Attribute(attributes, "ry").empty()) {
                ry = std::stod(Attribute(attributes, "ry"));
            } else {
                ry = rx;
            }
            rx = (std::min)(rx, width / 2.0);
            ry = (std::min)(ry, height / 2.0);

            std::vector<Point> points;
            points.push_back(Point{x + rx, y});
            points.push_back(Point{x + width - rx, y});
            SampleCentreArc(x + width - rx, y + ry, rx, ry, 270.0, 90.0, points);
            points.push_back(Point{x + width, y + height - ry});
            SampleCentreArc(x + width - rx, y + height - ry, rx, ry, 0.0, 90.0, points);
            points.push_back(Point{x + rx, y + height});
            SampleCentreArc(x + rx, y + height - ry, rx, ry, 90.0, 90.0, points);
            points.push_back(Point{x, y + ry});
            SampleCentreArc(x + rx, y + ry, rx, ry, 180.0, 90.0, points);

            Figure figure;
            for (size_t i = 1; i < points.size(); ++i) {
                figure.segments.push_back(Segment{Segment::Kind::Line, points[i - 1], {}, {}, points[i]});
            }
            figure.segments.push_back(Segment{Segment::Kind::Line, points.back(), {}, {}, points.front()});
            figure.closed = true;
            figures.push_back(figure);
            continue;
        }
        return fail("unsupported SVG element");
    }
    return true;
}

inline void ExtendBounds(Bounds& bounds, const Point& point) {
    if (!bounds.valid) {
        bounds.minX = bounds.maxX = point.x;
        bounds.minY = bounds.maxY = point.y;
        bounds.valid = true;
        return;
    }
    bounds.minX = (std::min)(bounds.minX, point.x);
    bounds.maxX = (std::max)(bounds.maxX, point.x);
    bounds.minY = (std::min)(bounds.minY, point.y);
    bounds.maxY = (std::max)(bounds.maxY, point.y);
}

}  // namespace detail

inline bool BuildGeometry(icon_id id, std::vector<Figure>& figures,
                          Bounds* bounds, std::string* error) {
    figures.clear();
    if (bounds != nullptr) {
        *bounds = Bounds{};
    }
    const icon_asset* asset = FindAsset(id);
    if (asset == nullptr) {
        if (error != nullptr) {
            *error = "unknown icon_id";
        }
        return false;
    }
    if (!detail::ParseAsset(*asset, figures, error)) {
        return false;
    }
    if (bounds != nullptr) {
        for (const Figure& figure : figures) {
            for (const Segment& segment : figure.segments) {
                if (segment.kind == Segment::Kind::Ellipse) {
                    detail::ExtendBounds(*bounds, Point{segment.from.x - segment.to.x, segment.from.y - segment.to.y});
                    detail::ExtendBounds(*bounds, Point{segment.from.x + segment.to.x, segment.from.y + segment.to.y});
                    continue;
                }
                detail::ExtendBounds(*bounds, segment.from);
                detail::ExtendBounds(*bounds, segment.to);
                if (segment.kind == Segment::Kind::Cubic) {
                    detail::ExtendBounds(*bounds, segment.control1);
                    detail::ExtendBounds(*bounds, segment.control2);
                }
            }
        }
    }
    return true;
}

inline bool BuildPath(icon_id id, Gdiplus::GraphicsPath& path, std::string* error) {
    std::vector<Figure> figures;
    if (!BuildGeometry(id, figures, nullptr, error)) {
        return false;
    }
    path.Reset();
    bool pathHasFigure = false;
    for (const Figure& figure : figures) {
        bool firstStrokeOfFigure = true;
        for (const Segment& segment : figure.segments) {
            if (segment.kind == Segment::Kind::Ellipse) {
                path.StartFigure();
                path.AddEllipse(static_cast<Gdiplus::REAL>(segment.from.x - segment.to.x),
                                static_cast<Gdiplus::REAL>(segment.from.y - segment.to.y),
                                static_cast<Gdiplus::REAL>(segment.to.x * 2.0),
                                static_cast<Gdiplus::REAL>(segment.to.y * 2.0));
                pathHasFigure = true;
                firstStrokeOfFigure = false;
                continue;
            }
            if (firstStrokeOfFigure && pathHasFigure) {
                path.StartFigure();
                pathHasFigure = false;
            }
            firstStrokeOfFigure = false;
            if (segment.kind == Segment::Kind::Cubic) {
                path.AddBezier(static_cast<Gdiplus::REAL>(segment.from.x),
                               static_cast<Gdiplus::REAL>(segment.from.y),
                               static_cast<Gdiplus::REAL>(segment.control1.x),
                               static_cast<Gdiplus::REAL>(segment.control1.y),
                               static_cast<Gdiplus::REAL>(segment.control2.x),
                               static_cast<Gdiplus::REAL>(segment.control2.y),
                               static_cast<Gdiplus::REAL>(segment.to.x),
                               static_cast<Gdiplus::REAL>(segment.to.y));
            } else {
                path.AddLine(static_cast<Gdiplus::REAL>(segment.from.x),
                             static_cast<Gdiplus::REAL>(segment.from.y),
                             static_cast<Gdiplus::REAL>(segment.to.x),
                             static_cast<Gdiplus::REAL>(segment.to.y));
            }
            pathHasFigure = true;
        }
    }
    return true;
}

inline bool EnsureGdiplusStarted() noexcept {
    static const bool started = [] {
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token = 0;
        return Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    }();
    return started;
}

inline bool StrokeIcon(Gdiplus::Graphics& graphics, const RECT& rect,
                       const Gdiplus::GraphicsPath& path, COLORREF color) {
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) {
        return false;
    }
    const int side = (std::min)(width, height);
    const double scale = static_cast<double>(side) / kGridUnits;
    const double offsetX = rect.left + (width - side) / 2.0;
    const double offsetY = rect.top + (height - side) / 2.0;

    Gdiplus::Matrix previous;
    graphics.GetTransform(&previous);
    Gdiplus::Matrix matrix;
    matrix.Scale(static_cast<Gdiplus::REAL>(scale),
                 static_cast<Gdiplus::REAL>(scale),
                 Gdiplus::MatrixOrderAppend);
    matrix.Translate(static_cast<Gdiplus::REAL>(offsetX),
                     static_cast<Gdiplus::REAL>(offsetY),
                     Gdiplus::MatrixOrderAppend);
    graphics.SetTransform(&matrix);

    Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)),
                     static_cast<Gdiplus::REAL>(kStrokeWidthUnits));
    pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapFlat);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    graphics.DrawPath(&pen, &path);
    graphics.SetTransform(&previous);
    return true;
}

inline bool DrawIcon(Gdiplus::Graphics& graphics, const RECT& rect, icon_id id, COLORREF color) {
    Gdiplus::GraphicsPath path;
    std::string error;
    if (!BuildPath(id, path, &error)) {
        return false;
    }
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    return StrokeIcon(graphics, rect, path, color);
}

inline bool DrawIcon(HDC dc, const RECT& rect, icon_id id, COLORREF color) {
    if (dc == nullptr) {
        return false;
    }
    if (!EnsureGdiplusStarted()) {
        return false;
    }
    Gdiplus::Graphics graphics(dc);
    return DrawIcon(graphics, rect, id, color);
}

}  // namespace lucide
}  // namespace oz::viewer

// oz/viewer/viewer_strings.hpp - English-only localization-readiness string accessor (Stage 10.3).
//
// One narrow viewer-private accessor owns every viewer-owned interactive
// presentation string. All functions return owning std::wstring values and are
// noexcept (allocation failure yields a bounded fallback, never an exception).
//
// Contract:
// - viewer_string(id) tries the English STRINGTABLE in ozviewer.rc first and
//   falls back to the compiled-in English table, so the separate seam-test
//   executable (which links this TU without .rc resources) observes identical
//   wording. Use viewer_string_from_resources() to detect a resource omission.
// - Never retain a temporary c_str() pointer. Retained toolbar/tooltip/control
//   text must point at stable storage owned by the creating module (see
//   ui_chrome.cpp's toolbar text store), never at a temporary returned here.
// - Destructive/confirmation callers must fail closed: when a required prompt
//   string resolves empty, take no action (cancel) instead of prompting.
// - Formatting is typed and bounded: only the listed helpers format resource
//   templates, user/filename data is passed as arguments (never as the format
//   string), and a malformed template yields a safe nonempty failure result.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "ozviewer_resources.h"

#include <cstddef>
#include <initializer_list>
#include <string>

namespace oz::viewer {

// True when every listed id resolves to a nonempty string (resource or
// compiled fallback). Destructive/confirmation flows must cancel when false.
[[nodiscard]] bool viewer_confirm_texts_available(
    std::initializer_list<UINT> ids) noexcept;

// Owning load: OS resource string when present and nonempty, otherwise the
// compiled-in English fallback. Unknown ids yield an empty string.
[[nodiscard]] std::wstring viewer_string(UINT id) noexcept;

// Resource-only probe: true and `out` set iff the OS STRINGTABLE provides a
// nonempty string for `id`. Used to detect resource omissions.
[[nodiscard]] bool viewer_string_from_resources(UINT id, std::wstring& out) noexcept;

// Compiled-in English fallback (what the test executable observes).
[[nodiscard]] std::wstring viewer_string_fallback(UINT id) noexcept;

// True iff `id` is a known viewer string id.
[[nodiscard]] bool viewer_string_id_known(UINT id) noexcept;

// Typed bounded formatters. Each loads its resource template
// (IDS_*_FMT) and formats with fixed bounds; a missing/empty/malformed
// template yields the documented safe nonempty failure text. Inputs are
// always format arguments, never the format string.
// "Output: %d x %d" style templates require exactly two %d conversions.
[[nodiscard]] std::wstring format_status_dimensions(int width, int height) noexcept;
[[nodiscard]] std::wstring format_crop_dimensions(int width, int height) noexcept;
[[nodiscard]] std::wstring format_output_dimensions(int width, int height) noexcept;
[[nodiscard]] std::wstring format_zoom_fit(double percent) noexcept;
[[nodiscard]] std::wstring format_zoom_percent(double percent) noexcept;
[[nodiscard]] std::wstring format_nav_position(std::size_t one_based,
                                               std::size_t total) noexcept;
// Tooltip composition: "tip (shortcut)" or "tip". Inputs are concatenated,
// never interpreted as a format string.
[[nodiscard]] std::wstring format_tooltip_with_shortcut(
    const std::wstring& tip, const std::wstring& shortcut) noexcept;

// Explicit-template primitives (used by the formatters above and directly by
// tests for malformed-template coverage). `failure` must be nonempty; when it
// is empty a hardcoded L"-" guard is returned so the result is never empty.
[[nodiscard]] std::wstring format_int_pair_template(const std::wstring& tmpl,
                                                    int a, int b,
                                                    const wchar_t* failure) noexcept;
[[nodiscard]] std::wstring format_nav_template(const std::wstring& tmpl,
                                               std::size_t a, std::size_t b,
                                               const wchar_t* failure) noexcept;
[[nodiscard]] std::wstring format_zoom_template(const std::wstring& tmpl, double percent,
                                                const wchar_t* failure) noexcept;

}  // namespace oz::viewer

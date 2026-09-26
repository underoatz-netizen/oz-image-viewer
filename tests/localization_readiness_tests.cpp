// localization_readiness_tests.cpp - focused unit tests for Stage 10.3 English localization readiness.
// Tests the STRINGTABLE resources embedded in the test binary or loaded dynamically,
// verifying accessor lookup, resource presence, fail-closed contract, formatting,
// and parity with compiled-in fallback text.
#include "viewer_strings.hpp"
#include "ozviewer_resources.h"
#include "drop_flow.hpp"

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(expr) do { \
    ++g_checks; \
    if (!(expr)) { \
        ++g_failures; \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
    } \
} while (0)

void test_embedded_resources_match_fallback() {
    using namespace oz::viewer;

    // Verify that every registered English string in the fallback table
    // can be successfully loaded from the embedded STRINGTABLE resource.
    const UINT ids[] = {
        IDS_APP_TITLE,
        IDS_APP_TITLE_CROP,
        IDS_FALLBACK_IMAGE_NAME,
        IDS_STATUS_NO_IMAGE,
        IDS_OPEN_BUTTON,
        IDS_TOOLBAR_NAME,
        IDS_STATE_CROP_MODE,
        IDS_STATE_SAVING,
        IDS_STATE_PRINTING,
        IDS_STATE_RESIZING,
        IDS_STATE_MODIFIED,
        IDS_DIMENSIONS_FMT,
        IDS_ZOOM_FIT_FMT,
        IDS_ZOOM_FMT,
        IDS_NAV_FMT,
        IDS_OUTPUT_DIM_FMT,
        IDS_OUTPUT_INVALID,
        IDS_OUTPUT_DASH,
        IDS_CROP_DIM_EMPTY,
        IDS_TB_TIP_OPEN,
        IDS_TB_TIP_SAVE,
        IDS_TB_TIP_UNDO,
        IDS_TB_TIP_REDO,
        IDS_TB_TIP_ROTATE_LEFT,
        IDS_TB_TIP_ROTATE_RIGHT,
        IDS_TB_TIP_CROP,
        IDS_TB_TIP_RESIZE,
        IDS_TB_TIP_FIT,
        IDS_TB_TIP_ACTUAL,
        IDS_TB_TIP_FULLSCREEN,
        IDS_TB_SHORTCUT_OPEN,
        IDS_TB_SHORTCUT_SAVE,
        IDS_TB_SHORTCUT_UNDO,
        IDS_TB_SHORTCUT_REDO,
        IDS_TB_SHORTCUT_ROTATE_LEFT,
        IDS_TB_SHORTCUT_ROTATE_RIGHT,
        IDS_TB_SHORTCUT_CROP,
        IDS_TB_SHORTCUT_RESIZE,
        IDS_TB_SHORTCUT_FIT,
        IDS_TB_SHORTCUT_ACTUAL,
        IDS_TB_SHORTCUT_FULLSCREEN,
        IDS_TB_ACC_OPEN,
        IDS_TB_ACC_SAVE,
        IDS_TB_ACC_UNDO,
        IDS_TB_ACC_REDO,
        IDS_TB_ACC_ROTATE_LEFT,
        IDS_TB_ACC_ROTATE_RIGHT,
        IDS_TB_ACC_CROP,
        IDS_TB_ACC_RESIZE,
        IDS_TB_ACC_FIT,
        IDS_TB_ACC_ACTUAL,
        IDS_TB_ACC_FULLSCREEN,
        IDS_TB_TIP_PASTE_IMAGE,
        IDS_TB_TIP_SAVE_AS,
        IDS_TB_TIP_OPEN_LOCATION,
        IDS_TB_TIP_COPY_IMAGE,
        IDS_TB_TIP_COPY_FILE,
        IDS_TB_TIP_COPY_PATH,
        IDS_TB_TIP_PRINT,
        IDS_TB_TIP_DELETE,
        IDS_TB_TIP_FLIP_H,
        IDS_TB_TIP_FLIP_V,
        IDS_TB_TIP_ZOOM_IN,
        IDS_TB_TIP_ZOOM_OUT,
        IDS_TB_TIP_FIRST,
        IDS_TB_TIP_PREV,
        IDS_TB_TIP_NEXT,
        IDS_TB_TIP_LAST,
        IDS_TB_SHORTCUT_PASTE_IMAGE,
        IDS_TB_SHORTCUT_SAVE_AS,
        IDS_TB_SHORTCUT_COPY_IMAGE,
        IDS_TB_SHORTCUT_PRINT,
        IDS_TB_SHORTCUT_DELETE,
        IDS_TB_SHORTCUT_FLIP_H,
        IDS_TB_SHORTCUT_FLIP_V,
        IDS_TB_SHORTCUT_ZOOM_IN,
        IDS_TB_SHORTCUT_ZOOM_OUT,
        IDS_TB_SHORTCUT_FIRST,
        IDS_TB_SHORTCUT_PREV,
        IDS_TB_SHORTCUT_NEXT,
        IDS_TB_SHORTCUT_LAST,
        IDS_TB_ACC_PASTE_IMAGE,
        IDS_TB_ACC_SAVE_AS,
        IDS_TB_ACC_OPEN_LOCATION,
        IDS_TB_ACC_COPY_IMAGE,
        IDS_TB_ACC_COPY_FILE,
        IDS_TB_ACC_COPY_PATH,
        IDS_TB_ACC_PRINT,
        IDS_TB_ACC_DELETE,
        IDS_TB_ACC_FLIP_H,
        IDS_TB_ACC_FLIP_V,
        IDS_TB_ACC_ZOOM_IN,
        IDS_TB_ACC_ZOOM_OUT,
        IDS_TB_ACC_FIRST,
        IDS_TB_ACC_PREV,
        IDS_TB_ACC_NEXT,
        IDS_TB_ACC_LAST,
        IDS_MENU_FILE_OPEN,
        IDS_MENU_FILE_OPEN_LOCATION,
        IDS_MENU_FILE_COPY_IMAGE,
        IDS_MENU_FILE_COPY_FILE,
        IDS_MENU_FILE_COPY_PATH,
        IDS_MENU_FILE_DELETE,
        IDS_MENU_FILE_PRINT,
        IDS_MENU_FILE_SAVE,
        IDS_MENU_FILE_SAVE_AS,
        IDS_MENU_FILE_POPUP,
        IDS_MENU_EDIT_UNDO,
        IDS_MENU_EDIT_REDO,
        IDS_MENU_EDIT_ROTATE_LEFT,
        IDS_MENU_EDIT_ROTATE_RIGHT,
        IDS_MENU_EDIT_FLIP_H,
        IDS_MENU_EDIT_FLIP_V,
        IDS_MENU_EDIT_CROP,
        IDS_MENU_EDIT_RESIZE,
        IDS_MENU_EDIT_CROP_APPLY,
        IDS_MENU_EDIT_CROP_CANCEL,
        IDS_MENU_EDIT_CROP_RESET,
        IDS_MENU_EDIT_RATIO_POPUP,
        IDS_MENU_EDIT_POPUP,
        IDS_MENU_RATIO_FREE,
        IDS_MENU_RATIO_ORIGINAL,
        IDS_MENU_RATIO_1X1,
        IDS_MENU_RATIO_3X2,
        IDS_MENU_RATIO_4X3,
        IDS_MENU_RATIO_5X4,
        IDS_MENU_RATIO_16X9,
        IDS_MENU_RATIO_9X16,
        IDS_MENU_RATIO_ASERIES,
        IDS_CTX_FIT,
        IDS_CTX_ACTUAL,
        IDS_CTX_COPY_IMAGE,
        IDS_CTX_COPY_FILE,
        IDS_CTX_COPY_PATH,
        IDS_CTX_OPEN_LOCATION,
        IDS_CTX_DELETE,
        IDS_CROP_ASPECT_LABEL,
        IDS_CROP_BTN_RESET,
        IDS_CROP_BTN_APPLY,
        IDS_CROP_BTN_CANCEL,
        IDS_CROP_RATIO_FREE,
        IDS_CROP_RATIO_ORIGINAL,
        IDS_CROP_RATIO_1X1,
        IDS_CROP_RATIO_3X2,
        IDS_CROP_RATIO_4X3,
        IDS_CROP_RATIO_5X4,
        IDS_CROP_RATIO_16X9,
        IDS_CROP_RATIO_9X16,
        IDS_CROP_RATIO_ASERIES,
        IDS_RESIZE_UNIT,
        IDS_RESIZE_PIXELS,
        IDS_RESIZE_PERCENT,
        IDS_RESIZE_DIMENSIONS,
        IDS_RESIZE_WIDTH,
        IDS_RESIZE_HEIGHT,
        IDS_RESIZE_SCALE,
        IDS_RESIZE_ASPECT,
        IDS_RESIZE_APPLY,
        IDS_RESIZE_CANCEL,
        IDS_RESIZE_RESET,
        IDS_RESIZE_TITLE,
        IDS_RESIZE_UNAVAILABLE,
        IDS_RESIZE_VALIDATING,
        IDS_RESIZE_FAILED_GENERIC,
        IDS_RESIZE_ERR_SUPERSEDED,
        IDS_RESIZE_ERR_SOURCE_CHANGED,
        IDS_RESIZE_ERR_UNEXPECTED_DIMS,
        IDS_RESIZE_ERR_PRESENT_FAILED,
        IDS_RESIZE_ERR_SUBMIT_FAILED,
        IDS_SAVE_ERR_GENERIC,
        IDS_SAVE_ERR_CANCELLED,
        IDS_SAVE_ERR_INVALID,
        IDS_SAVE_ERR_STALE,
        IDS_SAVE_ERR_CONFLICT,
        IDS_SAVE_ERR_MATERIAL,
        IDS_SAVE_ERR_ALPHA,
        IDS_SAVE_ERR_ENCODER_UNAVAILABLE,
        IDS_SAVE_ERR_ENCODER_CONFIG,
        IDS_SAVE_ERR_ENCODE,
        IDS_SAVE_ERR_TEMP,
        IDS_SAVE_ERR_FLUSH,
        IDS_SAVE_ERR_REPLACE,
        IDS_SAVE_ERR_UNCERTAIN,
        IDS_SAVE_ERR_CLEANUP,
        IDS_SAVE_PROMPT_EXT_HEAD,
        IDS_SAVE_PROMPT_EXT_BODY,
        IDS_SAVE_PROMPT_TIFF_HEAD,
        IDS_SAVE_PROMPT_TIFF_BODY,
        IDS_SAVE_PROMPT_SAMESRC_HEAD,
        IDS_SAVE_PROMPT_SAMESRC_BODY,
        IDS_SAVE_PROMPT_SRCGONE_HEAD,
        IDS_SAVE_PROMPT_SRCGONE_BODY,
        IDS_SAVE_PROMPT_NOSOURCE_HEAD,
        IDS_SAVE_PROMPT_NOSOURCE_BODY,
        IDS_SAVE_PROMPT_BUSY_HEAD,
        IDS_SAVE_PROMPT_BUSY_BODY,
        IDS_SAVE_PROMPT_TIMEOUT_HEAD,
        IDS_SAVE_PROMPT_TIMEOUT_BODY,
        IDS_SAVE_PROMPT_UNVERIFIED_HEAD,
        IDS_SAVE_PROMPT_UNVERIFIED_BODY,
        IDS_SAVE_PROMPT_DOCCANGED_HEAD,
        IDS_SAVE_PROMPT_DOCCANGED_BODY,
        IDS_DISCARD_HEAD_CROP,
        IDS_DISCARD_HEAD_DIRTY,
        IDS_DISCARD_BODY_CROP,
        IDS_DISCARD_BODY_DIRTY,
        IDS_PRINT_ERR_INVALID,
        IDS_PRINT_ERR_ADMISSION,
        IDS_PRINT_ERR_ACQUIRE,
        IDS_PRINT_ERR_MATERIALIZE,
        IDS_PRINT_ERR_CAPS,
        IDS_PRINT_ERR_SPOOL,
        IDS_PRINT_TITLE,
        IDS_PRINT_UNCHANGED,
        IDS_DELETE_TITLE,
        IDS_DELETE_HEAD_GENERIC,
        IDS_DELETE_DETAIL_GENERIC,
        IDS_DELETE_HEAD_IDENTITY,
        IDS_DELETE_HEAD_NOTFOUND,
        IDS_DELETE_HEAD_NOTRECYCLABLE,
        IDS_DELETE_HEAD_NEXT_FAILED,
        IDS_DELETE_DETAIL_NEXT_FAILED,
        IDS_DROP_HEAD_UNABLE,
        IDS_DROP_HEAD_INVALID,
        IDS_DROP_DETAIL_INVALID,
        IDS_DROP_HEAD_MIXED,
        IDS_DROP_DETAIL_MIXED,
        IDS_DROP_HEAD_MULTIFOLDER,
        IDS_DROP_DETAIL_MULTIFOLDER,
        IDS_DROP_HEAD_OVERFLOW,
        IDS_DROP_DETAIL_OVERFLOW,
        IDS_DROP_HEAD_FOLDER_EMPTY,
        IDS_DROP_HEAD_FILES_NONE,
        IDS_DROP_DETAIL_NOTHING,
        IDS_DROP_HEAD_BUSY,
        IDS_DROP_DETAIL_BUSY,
        IDS_FILTER_ALL_SUPPORTED,
        IDS_FILTER_JPEG_OPEN,
        IDS_FILTER_PNG_OPEN,
        IDS_FILTER_WEBP_OPEN,
        IDS_FILTER_AVIF_OPEN,
        IDS_FILTER_HEIC_OPEN,
        IDS_FILTER_BMP_OPEN,
        IDS_FILTER_GIF_OPEN,
        IDS_FILTER_TIFF_OPEN,
        IDS_FILTER_ICO_OPEN,
        IDS_FILTER_CUR_OPEN,
        IDS_FILTER_TGA_OPEN,
        IDS_FILTER_ALL_FILES,
        IDS_OPEN_TITLE,
        IDS_SAVE_FILTER_PNG,
        IDS_SAVE_FILTER_JPEG,
        IDS_SAVE_FILTER_WEBP,
        IDS_SAVE_FILTER_TIFF,
        IDS_SAVE_FILTER_BMP,
        IDS_SAVE_IMAGE_FILTER_PNG,
        IDS_SAVE_IMAGE_FILTER_JPEG,
        IDS_SAVE_IMAGE_FILTER_WEBP,
        IDS_SAVE_IMAGE_FILTER_TIFF,
        IDS_SAVE_IMAGE_FILTER_BMP,
        IDS_SAVE_TITLE,
        IDS_USAGE_BODY,
        IDS_OPEN_LOCATION_TITLE,
        IDS_OPEN_LOCATION_ERR_HEAD,
        IDS_OPEN_LOCATION_ERR_BODY,
        IDS_COPY_FILE_TITLE,
        IDS_COPY_FILE_ERR_HEAD,
        IDS_COPY_FILE_ERR_BODY,
        IDS_COPY_PATH_TITLE,
        IDS_COPY_PATH_ERR_HEAD,
        IDS_COPY_PATH_ERR_BODY,
        IDS_COPY_IMAGE_TITLE,
        IDS_COPY_IMAGE_ERR_HEAD,
        IDS_COPY_IMAGE_ERR_BODY_DECODE,
        IDS_COPY_IMAGE_ERR_BODY_CLIPBOARD,
        IDS_OPEN_IMAGE_ERR_HEAD,
        IDS_OPEN_IMAGE_ERR_BODY,
        IDS_DISPLAY_IMAGE_ERR_HEAD
    };

    for (const UINT id : ids) {
        std::wstring res_str;
        const bool from_res = viewer_string_from_resources(id, res_str);
        CHECK(from_res);
        CHECK(!res_str.empty());

        const std::wstring fb_str = viewer_string_fallback(id);
        CHECK(!fb_str.empty());
        CHECK(res_str == fb_str);
    }
}

void test_fail_closed_contract() {
    using namespace oz::viewer;

    // A valid list of confirmation strings passes
    CHECK(viewer_confirm_texts_available({
        IDS_APP_TITLE,
        IDS_DISCARD_HEAD_DIRTY,
        IDS_DISCARD_BODY_DIRTY
    }));

    // Missing string id causes rejection (fail-closed)
    CHECK(!viewer_confirm_texts_available({
        IDS_APP_TITLE,
        88888, // non-existent
        IDS_DISCARD_BODY_DIRTY
    }));
}

void test_drop_policy_text_parity() {
    using namespace oz::viewer;

    // The dependency-free drop_flow policy seam owns the English drop-rejection
    // wording; the viewer now resolves it through the shared string ids at the
    // presentation boundary. Tie the seam's literals to the resource/fallback
    // table so the two cannot drift.
    const drop_reject_text overload = describe_drop_overload();
    CHECK(viewer_string_fallback(IDS_APP_TITLE) == std::wstring(overload.title));
    CHECK(viewer_string_fallback(IDS_DROP_HEAD_BUSY) == std::wstring(overload.headline));
    CHECK(viewer_string_fallback(IDS_DROP_DETAIL_BUSY) == std::wstring(overload.detail));

    const drop_reject_text invalid =
        describe_drop_rejection(drop_classify_status::invalid_entry);
    CHECK(viewer_string_fallback(IDS_DROP_HEAD_INVALID) == std::wstring(invalid.headline));
    CHECK(viewer_string_fallback(IDS_DROP_DETAIL_INVALID) == std::wstring(invalid.detail));

    const drop_reject_text mixed = describe_drop_rejection(drop_classify_status::mixed);
    CHECK(viewer_string_fallback(IDS_DROP_HEAD_MIXED) == std::wstring(mixed.headline));
    CHECK(viewer_string_fallback(IDS_DROP_DETAIL_MIXED) == std::wstring(mixed.detail));

    const drop_reject_text multifolder =
        describe_drop_rejection(drop_classify_status::multiple_folders);
    CHECK(viewer_string_fallback(IDS_DROP_HEAD_MULTIFOLDER) == std::wstring(multifolder.headline));
    CHECK(viewer_string_fallback(IDS_DROP_DETAIL_MULTIFOLDER) == std::wstring(multifolder.detail));

    const drop_reject_text overflow = describe_drop_rejection(drop_classify_status::overflow);
    CHECK(viewer_string_fallback(IDS_DROP_HEAD_OVERFLOW) == std::wstring(overflow.headline));
    CHECK(viewer_string_fallback(IDS_DROP_DETAIL_OVERFLOW) == std::wstring(overflow.detail));

    const drop_reject_text folder_empty = describe_no_supported_images(true);
    CHECK(viewer_string_fallback(IDS_DROP_HEAD_FOLDER_EMPTY) == std::wstring(folder_empty.headline));
    CHECK(viewer_string_fallback(IDS_DROP_DETAIL_NOTHING) == std::wstring(folder_empty.detail));

    const drop_reject_text files_none = describe_no_supported_images(false);
    CHECK(viewer_string_fallback(IDS_DROP_HEAD_FILES_NONE) == std::wstring(files_none.headline));
    CHECK(viewer_string_fallback(IDS_DROP_DETAIL_NOTHING) == std::wstring(files_none.detail));
}

void test_formatting_safety() {
    using namespace oz::viewer;

    // Well-formed inputs
    CHECK(format_status_dimensions(1920, 1080) == L"1920 \u00D7 1080");
    CHECK(format_crop_dimensions(400, 300) == L"400 \u00D7 300");
    CHECK(format_output_dimensions(100, 200) == L"Output: 100 \u00D7 200");
    CHECK(format_zoom_fit(50.0) == L"Fit (50%)");
    CHECK(format_zoom_percent(200.0) == L"200%");
    CHECK(format_nav_position(2, 5) == L"2 / 5");

    // Boundary/invalid inputs yield non-empty safe fallback
    CHECK(format_status_dimensions(0, 0) == L"-");
    CHECK(format_crop_dimensions(-1, 50) == L"-");
    CHECK(format_output_dimensions(-10, -20) == L"Output: invalid");
    CHECK(format_nav_position(0, 5) == L"-");
    CHECK(format_nav_position(6, 5) == L"-");

    // Malformed format string templates must not crash and return fallback
    CHECK(format_int_pair_template(L"Malformed %s", 1, 2, L"SafeFallback") == L"SafeFallback");
    CHECK(format_int_pair_template(L"Only one %d", 1, 2, L"SafeFallback") == L"SafeFallback");
    CHECK(format_nav_template(L"Wrong %s / %s", 1, 2, L"SafeFallback") == L"SafeFallback");
    CHECK(format_zoom_template(L"Not float %d", 50.0, L"SafeFallback") == L"SafeFallback");
}

} // namespace

int main() {
    test_embedded_resources_match_fallback();
    test_fail_closed_contract();
    test_formatting_safety();
    test_drop_policy_text_parity();

    std::printf("localization_readiness_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

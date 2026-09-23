// oz/viewer/paste_install.hpp - Stage 9.5 temporary paste-document staging seam.
//
// Ctrl+V builds a complete temporary document install from the clipboard
// payload: the display image, the domain image_document, and the payload-sized
// canonical source buffer retained for Copy Image. This seam builds every one
// of those artifacts - including the pixel copy into the retained source -
// BEFORE the caller mutates any UI/document/presentation state. A
// clipboard-payload-driven allocation fault (bad_alloc/length_error) is caught
// here and reported as a quiet `false` with `out` untouched, so the caller's
// prior display, document, view, and flags remain exactly as they were.
// Without this staging the source copy would run after presentation, and a
// bad_alloc would escape Ctrl+V with the new document already visible.
#pragma once

#include "clipboard_input.hpp"

#include "oz/viewer_core/cache.hpp"
#include "oz/viewer_core/codec.hpp"

#if OZVIEWER_ENABLE_EDITING
#include "edit_controller.hpp"
#include "oz/viewer_core/image_document.hpp"
#endif

#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

namespace oz::viewer {

// Fully pre-built install for a pasted clipboard bitmap. `image` is the
// presentation image; `document`/`source` mirror the Editing-ON temporary
// document state (pathless document + retained canonical source whose
// document_instance_id already matches the pre-built document).
struct paste_install {
    std::shared_ptr<oz::viewer_core::decoded_image_data> image;
#if OZVIEWER_ENABLE_EDITING
    std::unique_ptr<oz::viewer_core::image_document> document;
    std::shared_ptr<edit_source_buffer> source;
#endif
};

// Stages the complete paste install from a validated clipboard bitmap.
//
// Returns false quietly (leaving `out` untouched) when the bitmap is invalid
// or when an allocation-capable construction - including the payload-sized
// source copy for Copy Image - fails. On success every field is fully built,
// and the caller's remaining work is limited to no-throw moves/swaps.
[[nodiscard]] inline bool prepare_paste_install(clipboard_bitmap&& bitmap,
                                                paste_install& out) {
    if (!bitmap.is_valid()) {
        return false;
    }

    const std::int32_t width = bitmap.width;
    const std::int32_t height = bitmap.height;
    const std::int32_t stride = bitmap.stride;

    // Build into a local first; `out` is only assigned after every artifact
    // exists, so a fault can never publish a partial install.
    paste_install staged;
    try {
        auto image = std::make_shared<oz::viewer_core::decoded_image_data>();
        image->pixels = std::move(bitmap.pixels);
        image->width = width;
        image->height = height;
        image->stride = stride;
        image->scale_den = 1;
        image->src_width = width;
        image->src_height = height;
        image->codec = oz::viewer_core::codec_id::bmp_wic;
        image->source_file_size = 0;
        image->source_last_write_time = 0;
        staged.image = std::move(image);

#if OZVIEWER_ENABLE_EDITING
        oz::viewer_core::document_metadata meta;
        meta.source_path.clear();
        meta.source_dimensions = {width, height};
        meta.codec = oz::viewer_core::codec_id::bmp_wic;
        meta.codec_name = "bmp";  // matches codec_id::bmp_wic (paste is DIB-only)
        meta.file_size = 0;
        meta.last_write_time = 0;

        staged.document = std::make_unique<oz::viewer_core::image_document>(meta);

        // Payload-sized canonical source copy for Copy Image: the clipboard
        // install's only large allocation, completed before any caller swap.
        auto source = std::make_shared<edit_source_buffer>();
        source->document_instance_id = staged.document->instance_id();
        source->source_path.clear();
        source->file_size = 0;
        source->last_write_time = 0;
        source->dimensions = {width, height};
        source->stride = stride;
        source->pixels = staged.image->pixels;
        staged.source = std::move(source);
#endif
    } catch (const std::bad_alloc&) {
        return false;  // Quiet: `out` untouched, caller state untouched.
    } catch (const std::length_error&) {
        return false;  // Quiet: `out` untouched, caller state untouched.
    }

    out = std::move(staged);
    return true;
}

}  // namespace oz::viewer

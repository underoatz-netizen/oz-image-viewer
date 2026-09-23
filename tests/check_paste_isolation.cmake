# check_paste_isolation.cmake - source-scan CTest for the Stage 9.5
# clipboard-input (Ctrl+V paste) isolation contract.
#
# 1. The paste entry point (start_paste_from_clipboard) must refuse both
#    automated modes (--benchmark and --navigation-benchmark) before any
#    clipboard inspection.
# 2. The WM_KEYDOWN Ctrl+V dispatch must be guarded by the shared
#    noninteractive_mode predicate so benchmark runs never inspect or
#    mutate the clipboard.
# 3. main.cpp must not call raw Win32 clipboard read APIs directly; all
#    clipboard input stays behind the clipboard_input boundary
#    (read_clipboard_image / parse_dib_bytes), mirroring the Stage 9.3
#    output boundary rule enforced by check_benchmark_ui_isolation.cmake.
# 4. The message-loop prefilter must give native text paste (Edit/RichEdit
#    focus) and dialog ownership precedence for the 'V' key.
# 5. M1 planner contract: the active-presentation planner must observe
#    suppress_edit_presentation before any edit-history/draft path (so a pasted
#    bitmap gets a plain fit plan, never the replaced document's transform),
#    and the paste swap must arm/restore suppression around the presentation.
#
# Usage: cmake -DMAIN_CPP=<src/viewer/main.cpp> -DVIEWER_DIR=<src/viewer>
#        -P check_paste_isolation.cmake

if(NOT EXISTS "${MAIN_CPP}")
    message(FATAL_ERROR "main.cpp not found: ${MAIN_CPP}")
endif()
if(NOT DEFINED VIEWER_DIR)
    message(FATAL_ERROR "VIEWER_DIR not set")
endif()

set(INPUT_HPP "${VIEWER_DIR}/clipboard_input.hpp")
set(INPUT_CPP "${VIEWER_DIR}/clipboard_input.cpp")
foreach(f IN ITEMS "${INPUT_HPP}" "${INPUT_CPP}")
    if(NOT EXISTS "${f}")
        message(FATAL_ERROR "required clipboard input boundary file not found: ${f}")
    endif()
endforeach()

file(READ "${MAIN_CPP}" content)
file(READ "${INPUT_CPP}" input_content)

# 1. Paste entry point exists and refuses both non-interactive modes first.
string(FIND "${content}" "static void start_paste_from_clipboard(app_state& app) {" paste_pos)
if(paste_pos EQUAL -1)
    message(FATAL_ERROR "start_paste_from_clipboard definition not found in main.cpp")
endif()
string(SUBSTRING "${content}" ${paste_pos} 300 paste_window)
string(FIND "${paste_window}" "app.benchmark || app.nav_benchmark" paste_guard_pos)
if(paste_guard_pos EQUAL -1)
    message(FATAL_ERROR "start_paste_from_clipboard must guard against benchmark and nav_benchmark")
endif()

# The temporary-document commit must bump the document generation so stale
# file workers cannot overwrite the pasted pixels.
string(FIND "${content}" "static bool open_pasted_document(app_state& app, oz::viewer::clipboard_bitmap&& bitmap) {" commit_pos)
if(commit_pos EQUAL -1)
    message(FATAL_ERROR "open_pasted_document definition not found in main.cpp")
endif()
# Delimit the whole definition (first column-0 closing brace after the
# signature) so the ordering assertions below cannot be satisfied by text from
# a neighboring function and a long definition is never truncated by a fixed
# window.
string(SUBSTRING "${content}" ${commit_pos} -1 commit_tail)
string(FIND "${commit_tail}" "\n}" commit_end_rel)
if(commit_end_rel EQUAL -1)
    message(FATAL_ERROR "could not delimit the open_pasted_document body")
endif()
string(SUBSTRING "${content}" ${commit_pos} ${commit_end_rel} commit_body)
string(FIND "${commit_body}" "app.doc.next_generation()" commit_gen_pos)
if(commit_gen_pos EQUAL -1)
    message(FATAL_ERROR "open_pasted_document must bump app.doc generation for stale-worker rejection")
endif()

# The temporary document must carry no source file path.
string(FIND "${content}" "app.doc.active_path.clear();" clear_pos)
if(clear_pos EQUAL -1)
    message(FATAL_ERROR "paste commit must clear app.doc.active_path (temporary document has no file path)")
endif()

# 2. WM_KEYDOWN Ctrl+V dispatch is guarded by noninteractive_mode.
string(FIND "${content}" "Paste Image from clipboard: Ctrl+V" paste_key_pos)
if(paste_key_pos EQUAL -1)
    message(FATAL_ERROR "WM_KEYDOWN Ctrl+V paste block marker not found in main.cpp")
endif()
math(EXPR paste_key_window_start "${paste_key_pos} - 400")
if(paste_key_window_start LESS 0)
    set(paste_key_window_start 0)
endif()
string(SUBSTRING "${content}" ${paste_key_window_start} 600 paste_key_window)
string(FIND "${paste_key_window}" "noninteractive_mode(*app)" paste_key_guard_pos)
if(paste_key_guard_pos EQUAL -1)
    message(FATAL_ERROR "WM_KEYDOWN Ctrl+V dispatch must be guarded by noninteractive_mode(*app)")
endif()

# 3. No direct Win32 clipboard read calls in main.cpp; the boundary owns them.
foreach(sym IN ITEMS "GetClipboardData" "IsClipboardFormatAvailable")
    string(FIND "${content}" "${sym}" direct_pos)
    if(NOT direct_pos EQUAL -1)
        message(FATAL_ERROR "Direct ${sym} call found in main.cpp; clipboard reads must stay behind the clipboard_input boundary")
    endif()
endforeach()
foreach(sym IN ITEMS "GetClipboardData" "IsClipboardFormatAvailable")
    string(FIND "${input_content}" "${sym}" boundary_pos)
    if(boundary_pos EQUAL -1)
        message(FATAL_ERROR "clipboard_input.cpp does not own ${sym} (boundary assumption changed)")
    endif()
endforeach()

# The paste path must go through the bounded boundary entry point.
string(FIND "${content}" "read_clipboard_image(app.hwnd" read_pos)
if(read_pos EQUAL -1)
    message(FATAL_ERROR "main.cpp paste path must call read_clipboard_image(app.hwnd, ...)")
endif()

# 4. The message-loop prefilter routes the 'V' key with native/dialog precedence.
string(FIND "${content}" "msg.wParam == 'V'" prefilter_pos)
if(prefilter_pos EQUAL -1)
    message(FATAL_ERROR "message-loop prefilter does not handle the 'V' key")
endif()
string(SUBSTRING "${content}" ${prefilter_pos} 2500 prefilter_window)
string(FIND "${prefilter_window}" "native_text_paste" native_paste_pos)
if(native_paste_pos EQUAL -1)
    message(FATAL_ERROR "message-loop prefilter must divert native_text_paste to standard dispatch")
endif()

# 5. M1 (Stage 9.5 correctness re-review): the active-presentation planner must
#    observe suppress_edit_presentation BEFORE any edit-history/draft path.
#    During a transactional document swap (file open / navigation commit /
#    clipboard paste) app.image_doc still describes the document being
#    replaced; if the planner consults it, the incoming bitmap inherits the
#    prior transform and source_rect. The earlier scan only asserted textual
#    suppression assignments in open_pasted_document, which could not prove
#    planner behavior; this proof targets the planner contract itself.
#
#    (a) compute_active_presentation_plan routes the edit-vs-plain decision
#        through the suppression-aware seam with app.suppress_edit_presentation
#        as its first argument.
#    (b) Every edit-history/draft reference in the planner body sits after the
#        suppression-aware decision, so no edit-state path can run ahead of the
#        suppression predicate.
#    (c) The seam predicate returns the plain plan before reading history or
#        draft state (suppression ordering inside presentation_edits.hpp).
#    (d) Both presentation paths remain: the edit branch (affine proxy +
#        source_rect) and the plain non-edit fallback for the incoming bitmap.
set(PRESENTATION_EDITS_HPP "${VIEWER_DIR}/presentation_edits.hpp")
if(NOT EXISTS "${PRESENTATION_EDITS_HPP}")
    message(FATAL_ERROR "M1: suppression-aware presentation seam not found: ${PRESENTATION_EDITS_HPP}")
endif()
file(READ "${PRESENTATION_EDITS_HPP}" edits_content)

string(FIND "${content}" "compute_active_presentation_plan(app_state& app, int view_w, int view_h) {" planner_sig_pos)
if(planner_sig_pos EQUAL -1)
    message(FATAL_ERROR "M1: compute_active_presentation_plan definition not found in main.cpp")
endif()
string(SUBSTRING "${content}" ${planner_sig_pos} -1 planner_tail)
string(FIND "${planner_tail}" "\n}" planner_end_rel)
if(planner_end_rel EQUAL -1)
    message(FATAL_ERROR "M1: could not delimit the compute_active_presentation_plan body")
endif()
string(SUBSTRING "${planner_tail}" 0 ${planner_end_rel} planner_body)

# (a) The suppression-aware seam is the planner's edit-plan decision point.
string(FIND "${planner_body}" "should_use_edit_presentation(app.suppress_edit_presentation," m1_seam_call_pos)
if(m1_seam_call_pos EQUAL -1)
    message(FATAL_ERROR "M1: compute_active_presentation_plan must decide the edit plan via should_use_edit_presentation(app.suppress_edit_presentation, ...)")
endif()

# (b) Every edit-history/draft reference in the planner body must sit after the
#     suppression-aware decision, so no edit-state path can run ahead of the
#     suppression predicate.
foreach(sym IN ITEMS "history()" "active_commands()" "draft_resize")
    string(FIND "${planner_body}" "${sym}" m1_direct_pos)
    if(m1_direct_pos EQUAL -1)
        message(FATAL_ERROR "M1: compute_active_presentation_plan no longer references ${sym}; the edit branch was lost")
    endif()
    if(NOT m1_seam_call_pos LESS m1_direct_pos)
        message(FATAL_ERROR "M1: compute_active_presentation_plan must observe suppression (seam call) before referencing ${sym}")
    endif()
endforeach()

# (c) The seam checks suppression before any history/draft read.
string(FIND "${edits_content}" "if (suppress_edit_presentation)" m1_suppress_first_pos)
if(m1_suppress_first_pos EQUAL -1)
    message(FATAL_ERROR "M1: should_use_edit_presentation must return the plain plan when suppression is true")
endif()
foreach(sym IN ITEMS "history()" "draft_resize.has_value()")
    string(FIND "${edits_content}" "${sym}" m1_seam_read_pos)
    if(m1_seam_read_pos EQUAL -1)
        message(FATAL_ERROR "M1: should_use_edit_presentation no longer detects ${sym} on the non-suppressed path")
    endif()
    if(NOT m1_suppress_first_pos LESS m1_seam_read_pos)
        message(FATAL_ERROR "M1: suppression must be observed before ${sym} in should_use_edit_presentation")
    endif()
endforeach()

# (d) Both plan paths survive the repair.
foreach(sym IN ITEMS "compute_affine_proxy_plan" "plan.source_rectf" "app.current->src_width")
    string(FIND "${planner_body}" "${sym}" m1_path_pos)
    if(m1_path_pos EQUAL -1)
        message(FATAL_ERROR "M1: compute_active_presentation_plan lost its ${sym} path; edit and plain plans must both remain")
    endif()
endforeach()

# 5b. The paste swap must still arm suppression before presenting the pasted
#     bitmap and restore it on both the success and the prior-image-restore
#     failure paths, so the planner proof above is exercised by Ctrl+V paste.
#     (commit_body is the full delimited definition from section 1.)
string(FIND "${commit_body}" "old_suppress_edit_presentation" m1_save_pos)
if(m1_save_pos EQUAL -1)
    message(FATAL_ERROR "M1: open_pasted_document must save old_suppress_edit_presentation")
endif()
string(FIND "${commit_body}" "suppress_edit_presentation = true" m1_set_pos)
if(m1_set_pos EQUAL -1)
    message(FATAL_ERROR "M1: open_pasted_document must set suppress_edit_presentation = true before presenting the pasted bitmap")
endif()
string(FIND "${commit_body}" "upload_and_present(app)" m1_present_pos)
if(m1_present_pos EQUAL -1)
    message(FATAL_ERROR "M1: open_pasted_document must present via upload_and_present(app)")
endif()
if(NOT m1_set_pos LESS m1_present_pos)
    message(FATAL_ERROR "M1: suppression must be enabled before upload_and_present(app)")
endif()
# Two restores required: failure-path restore (prior-image restore) + success-path restore.
string(FIND "${commit_body}" "suppress_edit_presentation = old_suppress_edit_presentation" m1_restore1_pos)
if(m1_restore1_pos EQUAL -1)
    message(FATAL_ERROR "M1: open_pasted_document must restore suppress_edit_presentation")
endif()
if(NOT m1_present_pos LESS m1_restore1_pos)
    message(FATAL_ERROR "M1: suppression restore must come after upload_and_present(app)")
endif()
string(LENGTH "suppress_edit_presentation = old_suppress_edit_presentation" m1_restore_len)
math(EXPR m1_after_restore1 "${m1_restore1_pos} + ${m1_restore_len}")
string(SUBSTRING "${commit_body}" ${m1_after_restore1} -1 commit_tail)
string(FIND "${commit_tail}" "suppress_edit_presentation = old_suppress_edit_presentation" m1_restore2_pos)
if(m1_restore2_pos EQUAL -1)
    message(FATAL_ERROR "M1: open_pasted_document must restore suppression on BOTH success and prior-image-restore failure paths (expected 2 restores)")
endif()
string(FIND "${commit_body}" "app.current = prev_image" m1_prev_restore_pos)
if(m1_prev_restore_pos EQUAL -1)
    message(FATAL_ERROR "M1: open_pasted_document must restore app.current = prev_image on present failure")
endif()

# 6. M2: read_clipboard_image must copy DIB bytes while locks are held, then
#    release GlobalLock/clipboard scope BEFORE calling parse_dib_bytes (the
#    documented copy-then-parse contract). Parsing while the lock scope is
#    still active is rejected.
string(FIND "${input_content}" "paste_result read_clipboard_image(" m2_func_pos)
if(m2_func_pos EQUAL -1)
    message(FATAL_ERROR "M2: read_clipboard_image definition not found in clipboard_input.cpp")
endif()
string(SUBSTRING "${input_content}" ${m2_func_pos} 6000 m2_body)
string(FIND "${m2_body}" "scoped_clipboard_read_lock clip_lock" m2_clip_pos)
if(m2_clip_pos EQUAL -1)
    message(FATAL_ERROR "M2: read_clipboard_image must hold scoped_clipboard_read_lock")
endif()
string(FIND "${m2_body}" "scoped_clipboard_handle_lock mem_lock" m2_mem_pos)
if(m2_mem_pos EQUAL -1)
    message(FATAL_ERROR "M2: read_clipboard_image must hold scoped_clipboard_handle_lock")
endif()
string(FIND "${m2_body}" "std::memcpy(local.data(), locked" m2_copy_pos)
if(m2_copy_pos EQUAL -1)
    message(FATAL_ERROR "M2: read_clipboard_image must memcpy DIB bytes from the locked handle into a local copy")
endif()
string(FIND "${m2_body}" "parse_dib_bytes(local.data()" m2_parse_pos)
if(m2_parse_pos EQUAL -1)
    message(FATAL_ERROR "M2: read_clipboard_image must parse the local copy via parse_dib_bytes(local.data(), ...)")
endif()
if(NOT m2_copy_pos LESS m2_parse_pos)
    message(FATAL_ERROR "M2: local copy must happen before parse_dib_bytes")
endif()
# There must be a lock-scope exit between the copy and the parse.
string(SUBSTRING "${m2_body}" ${m2_copy_pos} -1 m2_after_copy)
string(FIND "${m2_after_copy}" "parse_dib_bytes(local.data()" m2_rel_parse_pos)
if(m2_rel_parse_pos EQUAL -1)
    message(FATAL_ERROR "M2: internal scan error (parse not found after copy)")
endif()
string(SUBSTRING "${m2_after_copy}" 0 ${m2_rel_parse_pos} m2_between)
string(FIND "${m2_between}" "}" m2_close_pos)
if(m2_close_pos EQUAL -1)
    message(FATAL_ERROR "M2: scoped clipboard/GlobalLock scope must close between the local copy and parse_dib_bytes (locks released before parsing)")
endif()

# 7. Stage 9.5 paste-install allocation staging (repeat final review MUST_FIX).
#    The earlier paste path built the retained Copy Image source - including the
#    payload-sized img->pixels copy - AFTER upload_and_present, so a bad_alloc
#    escaped Ctrl+V with the new document already visible. The repair stages the
#    complete temporary document install (display image, pathless document,
#    payload source copy) in prepare_paste_install before any state mutation and
#    reports allocation faults as a quiet false. This section pins the ordering
#    in both the seam and its call site; the seam test proves the staged
#    ownership/shape, which a text scan cannot.
set(PASTE_INSTALL_HPP "${VIEWER_DIR}/paste_install.hpp")
if(NOT EXISTS "${PASTE_INSTALL_HPP}")
    message(FATAL_ERROR "paste-install staging seam not found: ${PASTE_INSTALL_HPP}")
endif()
file(READ "${PASTE_INSTALL_HPP}" install_content)

# (a) main.cpp must route the paste install through the staging seam.
string(FIND "${content}" "#include \"paste_install.hpp\"" install_include_pos)
if(install_include_pos EQUAL -1)
    message(FATAL_ERROR "main.cpp must include the paste-install staging seam")
endif()
string(FIND "${commit_body}" "prepare_paste_install(std::move(bitmap), install)" stage_call_pos)
if(stage_call_pos EQUAL -1)
    message(FATAL_ERROR "open_pasted_document must stage the paste install via prepare_paste_install(std::move(bitmap), install)")
endif()
# Staging must precede the first document-state mutation (the generation bump).
if(NOT stage_call_pos LESS commit_gen_pos)
    message(FATAL_ERROR "paste install must be fully staged BEFORE app.doc.next_generation() mutates document state")
endif()

# (b) No pixel copy may remain in the pasted-document commit itself: the only
#     payload-sized copy lives in the pre-mutation staging seam.
string(REGEX MATCHALL "->pixels" commit_pixel_refs "${commit_body}")
list(LENGTH commit_pixel_refs commit_pixel_ref_count)
if(NOT commit_pixel_ref_count EQUAL 0)
    message(FATAL_ERROR "open_pasted_document must not perform pixel copies; the payload copy belongs to prepare_paste_install")
endif()

# (c) The staged document and retained source are installed by no-throw moves
#     after the successful presentation.
string(FIND "${commit_body}" "app.image_doc = std::move(install.document)" doc_move_pos)
if(doc_move_pos EQUAL -1)
    message(FATAL_ERROR "open_pasted_document must install the pre-built image_document via a move")
endif()
string(FIND "${commit_body}" "app.full_res_source = std::move(install.source)" source_move_pos)
if(source_move_pos EQUAL -1)
    message(FATAL_ERROR "open_pasted_document must install the pre-built retained source via a move")
endif()
foreach(pos IN ITEMS "${doc_move_pos}" "${source_move_pos}")
    if(NOT m1_present_pos LESS pos)
        message(FATAL_ERROR "staged document/source installation must follow the successful upload_and_present(app) commit point")
    endif()
endforeach()

# (d) Inside the seam, the payload-sized source copy must complete (and the
#     document identity must already be resolved) before the install is
#     published to the caller.
string(FIND "${install_content}" "source->pixels = staged.image->pixels;" install_copy_pos)
if(install_copy_pos EQUAL -1)
    message(FATAL_ERROR "prepare_paste_install must copy the payload into the retained source before publishing the install")
endif()
string(FIND "${install_content}" "document->instance_id()" install_doc_id_pos)
if(install_doc_id_pos EQUAL -1)
    message(FATAL_ERROR "prepare_paste_install must bind the source document_instance_id to the pre-built document")
endif()
if(NOT install_doc_id_pos LESS install_copy_pos)
    message(FATAL_ERROR "source document_instance_id must be resolved before the payload copy")
endif()
string(FIND "${install_content}" "out = std::move(staged);" install_publish_pos)
if(install_publish_pos EQUAL -1)
    message(FATAL_ERROR "prepare_paste_install must publish the finished install to the caller output parameter")
endif()
if(NOT install_copy_pos LESS install_publish_pos)
    message(FATAL_ERROR "the payload copy must complete before the install is published")
endif()

# (e) Allocation faults are caught narrowly and return false quietly with the
#     caller output untouched (the catch must not touch `out`).
foreach(fault IN ITEMS "catch (const std::bad_alloc&)" "catch (const std::length_error&)")
    string(FIND "${install_content}" "${fault}" fault_pos)
    if(fault_pos EQUAL -1)
        message(FATAL_ERROR "prepare_paste_install must catch ${fault} from the staging allocations")
    endif()
    string(SUBSTRING "${install_content}" ${fault_pos} 120 fault_window)
    string(FIND "${fault_window}" "return false;" fault_return_pos)
    if(fault_return_pos EQUAL -1)
        message(FATAL_ERROR "prepare_paste_install must return false quietly from ${fault}")
    endif()
    if(NOT fault_pos LESS install_publish_pos)
        message(FATAL_ERROR "each allocation catch must be positioned before the install is published")
    endif()
endforeach()

# The seam is a pure staging helper: it must not reference application state.
string(FIND "${install_content}" "app." install_app_pos)
if(NOT install_app_pos EQUAL -1)
    message(FATAL_ERROR "prepare_paste_install must not reference app_state; it may only build the staged install")
endif()

# 8. Paste presentation view rollback (Stage 9.5 repeat final review MUST_FIX).
#    open_pasted_document fits app.view to the pasted dimensions before
#    presentation. On the double-present failure (upload_and_present + recovery
#    both fail) it must restore the pre-paste view before redrawing the old
#    bitmap, matching the try_commit_candidate transaction; the successful
#    paste keeps the new fit view.
#    (a) The pre-paste view is captured before set_fit mutates it.
#    (b) The failure rollback restores that view after the recovery attempt
#        and before the old-image redraw, on the return-false path.
#    (c) No view restore exists on the success path (new fit view preserved).
string(FIND "${commit_body}" "old_view = app.view" paste_view_save_pos)
if(paste_view_save_pos EQUAL -1)
    message(FATAL_ERROR "paste view rollback: open_pasted_document must capture the pre-paste view (old_view = app.view)")
endif()
string(FIND "${commit_body}" "set_fit(app.view" paste_view_fit_pos)
if(paste_view_fit_pos EQUAL -1)
    message(FATAL_ERROR "paste view rollback: open_pasted_document must fit app.view to the pasted dimensions via set_fit(app.view, ...)")
endif()
if(NOT paste_view_save_pos LESS paste_view_fit_pos)
    message(FATAL_ERROR "paste view rollback: pre-paste view capture must precede set_fit(app.view, ...)")
endif()
string(FIND "${commit_body}" "app.view = old_view" paste_view_restore_pos)
if(paste_view_restore_pos EQUAL -1)
    message(FATAL_ERROR "paste view rollback: open_pasted_document must restore app.view = old_view on present failure")
endif()
if(NOT paste_view_fit_pos LESS paste_view_restore_pos)
    message(FATAL_ERROR "paste view rollback: view restore must follow the pasted set_fit")
endif()
string(FIND "${commit_body}" "recover_and_present(app)" paste_view_recover_pos)
if(paste_view_recover_pos EQUAL -1)
    message(FATAL_ERROR "paste view rollback: open_pasted_document must attempt recover_and_present(app)")
endif()
if(NOT paste_view_recover_pos LESS paste_view_restore_pos)
    message(FATAL_ERROR "paste view rollback: view restore must follow the recover_and_present(app) attempt")
endif()
string(FIND "${commit_body}" "redraw_current_view(app)" paste_view_redraw_pos)
if(paste_view_redraw_pos EQUAL -1)
    message(FATAL_ERROR "paste view rollback: open_pasted_document must redraw the restored image on the failure path")
endif()
if(NOT paste_view_restore_pos LESS paste_view_redraw_pos)
    message(FATAL_ERROR "paste view rollback: prior view must be restored BEFORE redraw_current_view(app) so the old bitmap never draws with pasted geometry")
endif()
string(FIND "${commit_body}" "app.current = prev_image" paste_view_prev_pos)
if(paste_view_prev_pos EQUAL -1)
    message(FATAL_ERROR "paste view rollback: open_pasted_document must restore app.current = prev_image on present failure")
endif()
if(NOT paste_view_prev_pos LESS paste_view_redraw_pos)
    message(FATAL_ERROR "paste view rollback: prior image restore must precede the old-image redraw")
endif()
# The restore belongs to the double-present failure rollback: it must sit
# after the recovery attempt and before the failure return that follows the
# old-image redraw, and no second restore may follow on success. (The commit
# body contains an earlier quiet "return false;" from the allocation-staging
# guard, so the failure return is anchored to the redraw that follows the
# restore rather than to the first return in the body.)
string(SUBSTRING "${commit_body}" ${paste_view_redraw_pos} -1 paste_view_from_redraw)
string(FIND "${paste_view_from_redraw}" "return false;" paste_view_fail_rel)
if(paste_view_fail_rel EQUAL -1)
    message(FATAL_ERROR "paste view rollback: open_pasted_document must return false on double-present failure")
endif()
math(EXPR paste_view_fail_return_pos "${paste_view_redraw_pos} + ${paste_view_fail_rel}")
if(NOT paste_view_restore_pos LESS paste_view_fail_return_pos)
    message(FATAL_ERROR "paste view rollback: view restore must precede the failure return")
endif()
string(LENGTH "return false;" paste_view_return_len)
math(EXPR paste_view_success_start "${paste_view_fail_return_pos} + ${paste_view_return_len}")
string(SUBSTRING "${commit_body}" ${paste_view_success_start} -1 paste_view_success_tail)
string(FIND "${paste_view_success_tail}" "app.view = old_view" paste_view_second_restore_pos)
if(NOT paste_view_second_restore_pos EQUAL -1)
    message(FATAL_ERROR "paste view rollback: successful paste must keep the new fit view (no second view restore after the failure return)")
endif()

message(STATUS "check_paste_isolation: OK (paste stays in the clipboard_input boundary, guarded from benchmark modes, text/dialog ownership preserved, the presentation planner observes suppression before edit state, and the payload-sized paste install is staged before any state mutation with quiet allocation-fault failure)")

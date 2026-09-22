add_executable(ozbench_tests runner_tests.cpp)
target_link_libraries(ozbench_tests PRIVATE ozbench_lib oz_benchmark_markers)

add_test(NAME ozbench_tests
         COMMAND ozbench_tests ${CMAKE_SOURCE_DIR} $<TARGET_FILE:ozbench_fixture>)

# --- Stage 1 reference collector tests ---------------------------------------

add_executable(ozbench_collector_tests collector_tests.cpp)
target_link_libraries(ozbench_collector_tests PRIVATE oz_collector ozbench_lib
                      oz_benchmark_markers)

# Probe math, manifest sealing/tamper, redaction and plan schema always run;
# Desktop Duplication cases skip cleanly when no graphical desktop is present.
add_test(NAME ozbench_collector_tests
         COMMAND ozbench_collector_tests $<TARGET_FILE:ozbench_control_window>)

# CLI end-to-end capture against the control-window fixture (skips without a
# graphical desktop; never a false pass).
add_test(NAME ozbench_cli_capture_control_window
         COMMAND ${CMAKE_COMMAND}
                 -DOZBENCH=$<TARGET_FILE:ozbench>
                 -DCONTROL=$<TARGET_FILE:ozbench_control_window>
                 -DPLAN=${CMAKE_SOURCE_DIR}/plans/control-window.json
                 -DOUT=${CMAKE_BINARY_DIR}/capture-e2e.jsonl
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/check_capture.cmake)

# --- Stage 1: viewer core tests ---------------------------------------------

# Source-scan: reject folder-enumeration / sibling-derivation APIs in viewer
# sources. Dependency-free, so it runs in every configuration (including the
# Stage 0 no-vcpkg preset).
add_test(NAME ozviewer_no_enumeration_scan
         COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${CMAKE_SOURCE_DIR}/src/viewer
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/check_no_enumeration.cmake)

# Containment-scan: enforces that enumeration/sibling derivation is strictly
# isolated to ozviewer_core/navigation.cpp and forbidden in src/viewer.
add_test(NAME ozviewer_navigation_containment_scan
         COMMAND ${CMAKE_COMMAND}
                 -DVIEWER_DIR=${CMAKE_SOURCE_DIR}/src/viewer
                 -DNAV_FILE=${CMAKE_SOURCE_DIR}/src/viewer_core/navigation.cpp
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/check_navigation_containment.cmake)

# Stage 5 Slice 6 isolation-scan: the save controller is instantiated only in
# the interactive non-benchmark path (zero save UI/controller/workers in
# benchmark and navigation-benchmark runs).
add_test(NAME ozviewer_save_isolation_scan
         COMMAND ${CMAKE_COMMAND}
                 -DMAIN_CPP=${CMAKE_SOURCE_DIR}/src/viewer/main.cpp
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/check_save_isolation.cmake)

# Stage 7 UI isolation-scan: asserts that menus, Open button, drag/drop, chrome,
# and controllers are strictly excluded in both --benchmark and --navigation-benchmark.
add_test(NAME ozviewer_benchmark_ui_isolation_scan
         COMMAND ${CMAKE_COMMAND}
                 -DMAIN_CPP=${CMAKE_SOURCE_DIR}/src/viewer/main.cpp
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/check_benchmark_ui_isolation.cmake)

# Stage 9.4 copy-source identity-scan: the editing-OFF copy path must hand the
# copy controller an immutable source with a nonzero document identity, since
# edit_source_buffer::is_valid() rejects a zero id.
add_test(NAME ozviewer_copy_source_identity_scan
         COMMAND ${CMAKE_COMMAND}
                 -DMAIN_CPP=${CMAKE_SOURCE_DIR}/src/viewer/main.cpp
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/check_copy_source_identity.cmake)

# Stage 9.4-OFF link guard: copy_controller.cpp (always built) calls
# read_file_stable_identity, so the always-built source_identity.cpp must own
# that symbol while edit_controller.cpp stays behind the editing gate.
add_test(NAME ozviewer_off_link_sources_scan
         COMMAND ${CMAKE_COMMAND}
                 -DVIEWER_DIR=${CMAKE_SOURCE_DIR}/src/viewer
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/check_off_link_sources.cmake)

# Stage 7 UI Seam focused unit tests (dependency-free; builds in all presets including no-vcpkg)
if(OZVIEWER_ENABLE_EDITING)
    add_executable(ozviewer_stage7_seam_tests
                   stage7_seam_tests.cpp
                   ${CMAKE_SOURCE_DIR}/src/viewer/ui_theme.cpp
                   ${CMAKE_SOURCE_DIR}/src/viewer/ui_chrome.cpp
                   ${CMAKE_SOURCE_DIR}/src/viewer/ui_command_state.cpp
                   ${CMAKE_SOURCE_DIR}/src/viewer/ui_layout.cpp
                   ${CMAKE_SOURCE_DIR}/src/viewer/crop_strip.cpp
                   ${CMAKE_SOURCE_DIR}/src/viewer/svg_icon_cache.cpp
                   ${CMAKE_SOURCE_DIR}/src/viewer/modern_dialogs.cpp
                   ${CMAKE_SOURCE_DIR}/src/viewer/shell_integration.cpp
                   ${CMAKE_SOURCE_DIR}/src/viewer/clipboard_output.cpp
                   ${CMAKE_SOURCE_DIR}/src/viewer/shortcut_routing.cpp
                   ${CMAKE_SOURCE_DIR}/src/viewer/context_menu.cpp)
    target_link_libraries(ozviewer_stage7_seam_tests PRIVATE ozviewer_core d2d1 comctl32 shell32)
    target_include_directories(ozviewer_stage7_seam_tests PRIVATE
                               ${CMAKE_CURRENT_SOURCE_DIR}
                               ${CMAKE_SOURCE_DIR}/src/viewer)
    target_compile_features(ozviewer_stage7_seam_tests PRIVATE cxx_std_23)
    add_test(NAME ozviewer_stage7_seam_tests COMMAND ozviewer_stage7_seam_tests)
endif()

# The remaining Stage 1 tests decode real JPEGs with the TurboJPEG C API and
# drive the viewer executable, so they are only built when libjpeg-turbo is
# present via the pinned vcpkg toolchain.
find_package(libjpeg-turbo CONFIG QUIET)
if(TARGET libjpeg-turbo::turbojpeg OR TARGET libjpeg-turbo::turbojpeg-static)
    add_executable(ozviewer_core_tests viewer_core_tests.cpp)
    target_link_libraries(ozviewer_core_tests PRIVATE ozviewer_core oz_benchmark_markers)
    target_include_directories(ozviewer_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # Internal core headers (webp.hpp) for the decoder-independent
    # orientation-probe unit tests; the registry remains the only caller.
    target_include_directories(ozviewer_core_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/viewer_core)

    target_sources(ozviewer_core_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src/viewer/shell_integration.cpp
        ${CMAKE_SOURCE_DIR}/src/viewer/clipboard_output.cpp
        ${CMAKE_SOURCE_DIR}/src/viewer/shortcut_routing.cpp)
    target_link_libraries(ozviewer_core_tests PRIVATE shell32)

    if(OZVIEWER_ENABLE_EDITING)
        # Stage 5 Slice 6 boundary tests exercise the real stable-read helper
        # (edit_controller.cpp) and the extracted Save acceptance helpers.
        # Stage 7 UI command state, layout, SVG icon cache, and modern dialogs are linked for focused unit testing.
        target_sources(ozviewer_core_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src/viewer/edit_controller.cpp
            ${CMAKE_SOURCE_DIR}/src/viewer/source_identity.cpp
            ${CMAKE_SOURCE_DIR}/src/viewer/ui_command_state.cpp
            ${CMAKE_SOURCE_DIR}/src/viewer/ui_layout.cpp
            ${CMAKE_SOURCE_DIR}/src/viewer/svg_icon_cache.cpp
            ${CMAKE_SOURCE_DIR}/src/viewer/modern_dialogs.cpp
            ${CMAKE_SOURCE_DIR}/src/viewer/context_menu.cpp)
        target_link_libraries(ozviewer_core_tests PRIVATE d2d1 comctl32)
        target_include_directories(ozviewer_core_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/viewer)
    endif()

    if(OZVIEWER_ENABLE_HEIC)
        find_package(libheif CONFIG QUIET)
        if(TARGET heif)
            add_custom_command(TARGET ozviewer_core_tests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/heif.dll"
                    "$<TARGET_FILE_DIR:ozviewer_core_tests>/heif.dll"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/libde265.dll"
                    "$<TARGET_FILE_DIR:ozviewer_core_tests>/libde265.dll"
                COMMENT "Bundling heif.dll and libde265.dll alongside ozviewer_core_tests.exe"
            )
        endif()
    endif()

    add_test(NAME ozviewer_core_tests
             COMMAND ozviewer_core_tests)

    # JPEG fixture generator for the integration test.
    add_executable(ozviewer_make_jpeg make_jpeg.cpp)
    target_link_libraries(ozviewer_make_jpeg PRIVATE ozviewer_core)

    # Stage 4 View Interaction and Latency Evidence Test executable
    add_executable(ozviewer_interaction_evidence interaction_evidence_test.cpp ../src/viewer/renderer.cpp)
    target_include_directories(ozviewer_interaction_evidence PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src/viewer)
    target_compile_features(ozviewer_interaction_evidence PRIVATE cxx_std_23)
    target_link_libraries(ozviewer_interaction_evidence PRIVATE ozviewer_core oz_benchmark_markers d2d1 d3d11 dxgi)

    # PNG fixture generator (decoder-independent mini writer; no codec libs).
    add_executable(ozviewer_make_png make_png.cpp)
    target_include_directories(ozviewer_make_png PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(ozviewer_make_png PRIVATE cxx_std_23)

    # Deterministic navigation corpus generator and validator tool
    add_executable(oz_navigation_corpus_tool navigation_corpus_tool.cpp)
    target_link_libraries(oz_navigation_corpus_tool PRIVATE ozviewer_core)

    # Explicit-invocation smoke check: generates/validates a small isolated
    # corpus and proves a wrong expected count is rejected. Not performance data.
    add_test(NAME oz_navigation_corpus_tool_smoke
             COMMAND ${CMAKE_COMMAND}
                     -DTOOL=$<TARGET_FILE:oz_navigation_corpus_tool>
                     -DOUT_DIR=${CMAKE_BINARY_DIR}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/check_navigation_corpus_tool.cmake)

    # Focused unit tests for navigation benchmark tooling (authoritative target)
    add_executable(navigation_benchmark_tests navigation_benchmark_tests.cpp)
    target_link_libraries(navigation_benchmark_tests PRIVATE ozviewer_core ozbench_lib)
    add_test(NAME navigation_benchmark_tests COMMAND navigation_benchmark_tests)

    # Integration benchmark: generate a JPEG, run `ozviewer --benchmark`, assert a
    # valid marker prefix through FirstPresent, no folder markers, and auto-exit.
    add_test(NAME ozviewer_benchmark_integration
             COMMAND ${CMAKE_COMMAND}
                     -DOZVIEWER=$<TARGET_FILE:ozviewer>
                     -DMAKE_JPEG=$<TARGET_FILE:ozviewer_make_jpeg>
                     -DOUT_DIR=${CMAKE_BINARY_DIR}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/check_viewer_benchmark.cmake)

    # Stage 3.1 PNG integration benchmark: generate a PNG with the
    # decoder-independent mini writer, run `ozviewer --benchmark`, assert the
    # same canonical marker prefix through FirstPresent and auto-exit.
    add_test(NAME ozviewer_png_benchmark_integration
             COMMAND ${CMAKE_COMMAND}
                     -DOZVIEWER=$<TARGET_FILE:ozviewer>
                     -DMAKE_PNG=$<TARGET_FILE:ozviewer_make_png>
                     -DOUT_DIR=${CMAKE_BINARY_DIR}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/check_viewer_png_benchmark.cmake)

    # Stage 3.2 WebP integration benchmark: decode the checked-in
    # decoder-independent lossless WebP fixture (no test encoder),
    # assert the same canonical marker prefix through FirstPresent and
    # auto-exit. Only registered when static WebP is enabled.
    if(OZVIEWER_ENABLE_WEBP)
        find_package(WebP CONFIG QUIET)
        if(TARGET WebP::webpdecoder AND TARGET WebP::webpdemux)
            add_test(NAME ozviewer_webp_benchmark_integration
                     COMMAND ${CMAKE_COMMAND}
                             -DOZVIEWER=$<TARGET_FILE:ozviewer>
                             -DWEBP_FIXTURE=${CMAKE_SOURCE_DIR}/tests/data/webp/bench_64x48.webp
                             -DOUT_DIR=${CMAKE_BINARY_DIR}
                             -P ${CMAKE_CURRENT_SOURCE_DIR}/check_viewer_webp_benchmark.cmake)
        endif()
    endif()

    # Stage 3.3 AVIF integration benchmark: decode the checked-in
    # decoder-independent AVIF fixture, assert the same canonical marker
    # prefix through FirstPresent and auto-exit. Only registered when static
    # AVIF is enabled.
    if(OZVIEWER_ENABLE_AVIF)
        find_package(libavif CONFIG QUIET)
        if(TARGET avif)
            add_test(NAME ozviewer_avif_benchmark_integration
                     COMMAND ${CMAKE_COMMAND}
                             -DOZVIEWER=$<TARGET_FILE:ozviewer>
                             -DAVIF_FIXTURE=${CMAKE_SOURCE_DIR}/tests/data/avif/white_1x1.avif
                             -DOUT_DIR=${CMAKE_BINARY_DIR}
                             -P ${CMAKE_CURRENT_SOURCE_DIR}/check_viewer_avif_benchmark.cmake)
        endif()
    endif()

    # Stage 3.4 HEIC integration benchmark: decode the checked-in
    # decoder-independent HEIC fixture, assert the same canonical marker
    # prefix through FirstPresent and auto-exit. Only registered when dynamic
    # HEIC is enabled.
    if(OZVIEWER_ENABLE_HEIC)
        find_package(libheif CONFIG QUIET)
        if(TARGET heif)
            add_test(NAME ozviewer_heic_benchmark_integration
                     COMMAND ${CMAKE_COMMAND}
                             -DOZVIEWER=$<TARGET_FILE:ozviewer>
                             -DHEIC_FIXTURE=${CMAKE_SOURCE_DIR}/tests/data/heic/rainbow-451x461.heic
                             -DOUT_DIR=${CMAKE_BINARY_DIR}
                             -P ${CMAKE_CURRENT_SOURCE_DIR}/check_viewer_heic_benchmark.cmake)
        endif()
    endif()
    # fixtures (tests/data/phasea/README.md, no test encoder), one per
    # format, asserting the same canonical marker prefix through
    # FirstPresent, no folder enumeration, and auto-exit.
    add_test(NAME ozviewer_phasea_bmp_benchmark
             COMMAND ${CMAKE_COMMAND}
                     -DOZVIEWER=$<TARGET_FILE:ozviewer>
                     -DFIXTURE=${CMAKE_SOURCE_DIR}/tests/data/phasea/bench_64x48.bmp
                     -DOUT_DIR=${CMAKE_BINARY_DIR}
                     -DOUT_NAME=viewer_benchmark_phasea_bmp.jsonl
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/check_viewer_phasea_benchmark.cmake)
    add_test(NAME ozviewer_phasea_gif_benchmark
             COMMAND ${CMAKE_COMMAND}
                     -DOZVIEWER=$<TARGET_FILE:ozviewer>
                     -DFIXTURE=${CMAKE_SOURCE_DIR}/tests/data/phasea/bench_64x48.gif
                     -DOUT_DIR=${CMAKE_BINARY_DIR}
                     -DOUT_NAME=viewer_benchmark_phasea_gif.jsonl
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/check_viewer_phasea_benchmark.cmake)
    add_test(NAME ozviewer_phasea_tiff_benchmark
             COMMAND ${CMAKE_COMMAND}
                     -DOZVIEWER=$<TARGET_FILE:ozviewer>
                     -DFIXTURE=${CMAKE_SOURCE_DIR}/tests/data/phasea/bench_64x48.tif
                     -DOUT_DIR=${CMAKE_BINARY_DIR}
                     -DOUT_NAME=viewer_benchmark_phasea_tiff.jsonl
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/check_viewer_phasea_benchmark.cmake)
    add_test(NAME ozviewer_phasea_ico_benchmark
             COMMAND ${CMAKE_COMMAND}
                     -DOZVIEWER=$<TARGET_FILE:ozviewer>
                     -DFIXTURE=${CMAKE_SOURCE_DIR}/tests/data/phasea/bench_32x32.ico
                     -DOUT_DIR=${CMAKE_BINARY_DIR}
                     -DOUT_NAME=viewer_benchmark_phasea_ico.jsonl
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/check_viewer_phasea_benchmark.cmake)

    # Phase B integration benchmarks: checked-in decoder-independent CUR and
    # TGA fixtures (tests/data/phaseb/README.md, no test encoder), asserting
    # the same canonical marker prefix through FirstPresent, no folder
    # enumeration, and auto-exit.
    add_test(NAME ozviewer_phaseb_cur_benchmark
             COMMAND ${CMAKE_COMMAND}
                     -DOZVIEWER=$<TARGET_FILE:ozviewer>
                     -DFIXTURE=${CMAKE_SOURCE_DIR}/tests/data/phaseb/bench_32x32.cur
                     -DOUT_DIR=${CMAKE_BINARY_DIR}
                     -DOUT_NAME=viewer_benchmark_phaseb_cur.jsonl
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/check_viewer_phasea_benchmark.cmake)
    add_test(NAME ozviewer_phaseb_tga_benchmark
             COMMAND ${CMAKE_COMMAND}
                     -DOZVIEWER=$<TARGET_FILE:ozviewer>
                     -DFIXTURE=${CMAKE_SOURCE_DIR}/tests/data/phaseb/bench_64x48.tga
                     -DOUT_DIR=${CMAKE_BINARY_DIR}
                     -DOUT_NAME=viewer_benchmark_phaseb_tga.jsonl
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/check_viewer_phasea_benchmark.cmake)
endif()

# CLI end-to-end: run the fixture plan, then summarize the JSONL.
add_test(NAME ozbench_cli_e2e
         COMMAND ozbench run ${CMAKE_SOURCE_DIR}/plans/fixture.json
                 --out ${CMAKE_BINARY_DIR}/e2e.jsonl
                 --set fixture_exe=$<TARGET_FILE:ozbench_fixture>)
set_tests_properties(ozbench_cli_e2e PROPERTIES FIXTURES_SETUP OZBENCH_E2E)

add_test(NAME ozbench_cli_summarize
         COMMAND ozbench summarize ${CMAKE_BINARY_DIR}/e2e.jsonl
                 --format csv --out ${CMAKE_BINARY_DIR}/e2e-summary.csv)
set_tests_properties(ozbench_cli_summarize PROPERTIES FIXTURES_REQUIRED OZBENCH_E2E)

# Disabled competitor template must be skipped with exit code 3.
add_test(NAME ozbench_cli_validate_skip
         COMMAND ${CMAKE_COMMAND} -DOZBENCH=$<TARGET_FILE:ozbench>
                 -DPLAN=${CMAKE_SOURCE_DIR}/plans/irfanview.json
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/check_skip.cmake)

# Enabled fixture plan validates cleanly when the fixture path is provided.
add_test(NAME ozbench_cli_validate_fixture
         COMMAND ozbench validate ${CMAKE_SOURCE_DIR}/plans/fixture.json
                 --set fixture_exe=$<TARGET_FILE:ozbench_fixture>)

# A timed-out subject must be recorded as timed_out:true and must not hang.
add_test(NAME ozbench_cli_run_timeout
         COMMAND ${CMAKE_COMMAND} -DOZBENCH=$<TARGET_FILE:ozbench>
                 -DPLAN=${CMAKE_SOURCE_DIR}/plans/fixture-timeout.json
                 -DFIXTURE=$<TARGET_FILE:ozbench_fixture>
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/check_timeout.cmake)

# Negative CLI overrides must be rejected before narrowing (exit 1).
add_test(NAME ozbench_cli_reject_negative_repetitions
         COMMAND ozbench run ${CMAKE_SOURCE_DIR}/plans/fixture.json
                 --repetitions -1)
set_tests_properties(ozbench_cli_reject_negative_repetitions PROPERTIES WILL_FAIL TRUE)

add_test(NAME ozbench_cli_reject_negative_warmups
         COMMAND ozbench run ${CMAKE_SOURCE_DIR}/plans/fixture.json
                 --warmups -3)
set_tests_properties(ozbench_cli_reject_negative_warmups PROPERTIES WILL_FAIL TRUE)

# Stage 4: Benchmark isolation test (proves no enumeration/menu/dialog/drop during benchmark)
if(TARGET ozviewer)
    if(OZVIEWER_ENABLE_WEBP)
        set(OZVIEWER_STAGE4_FIXTURE "${CMAKE_SOURCE_DIR}/tests/data/webp/bench_64x48.webp")
    else()
        set(OZVIEWER_STAGE4_FIXTURE "${CMAKE_SOURCE_DIR}/tests/data/phasea/bench_64x48.bmp")
    endif()
    add_test(NAME ozviewer_stage4_isolation
             COMMAND ${CMAKE_COMMAND}
                     -DOZVIEWER=$<TARGET_FILE:ozviewer>
                     -DFIXTURE=${OZVIEWER_STAGE4_FIXTURE}
                     -DOUT_DIR=${CMAKE_BINARY_DIR}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/check_stage4_isolation.cmake)

    # Stage 5 Slice 6: scripted UI smoke for File -> Save / Save As. Requires a
    # real interactive desktop and editing enabled; exits 77 (skip) when the viewer
    # window cannot be created, so headless sessions never report a false pass.
    if(OZVIEWER_ENABLE_EDITING)
        add_test(NAME ozviewer_save_ui_smoke
                 COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                         -File ${CMAKE_CURRENT_SOURCE_DIR}/check_viewer_save_smoke.ps1
                         -Viewer $<TARGET_FILE:ozviewer>
                         -MakePng $<TARGET_FILE:ozviewer_make_png>
                         -WorkDir ${CMAKE_BINARY_DIR}/save-smoke
                         -OpaqueBmp ${CMAKE_SOURCE_DIR}/tests/data/phasea/bench_64x48.bmp
                         -AlphaBmp ${CMAKE_SOURCE_DIR}/tests/data/phasea/alpha_2x2.bmp)
        set_tests_properties(ozviewer_save_ui_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    endif()
endif()
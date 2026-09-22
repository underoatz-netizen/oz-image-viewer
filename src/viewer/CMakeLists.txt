# ozviewer - Stage 1 minimal JPEG viewer (WIN32 GUI subsystem).
#
# The viewer is only buildable when libjpeg-turbo (TurboJPEG C API) is present
# via the pinned vcpkg toolchain. Without it the dependency-free ozviewer_core
# still builds and is tested, but the viewer executable is skipped so the
# Stage 0 no-vcpkg preset continues to configure and build cleanly.
find_package(libjpeg-turbo CONFIG QUIET)
if(TARGET libjpeg-turbo::turbojpeg OR TARGET libjpeg-turbo::turbojpeg-static)
    # ozviewer.rc embeds the application icon (assets/icons/ozviewer.ico) so the
    # built executable carries its Shell/window/taskbar identity. CMake's RC
    # language is already enabled for this MSVC project; no extra dependency.
    set(OZVIEWER_SOURCES main.cpp renderer.cpp modern_dialogs.cpp shell_integration.cpp clipboard_output.cpp copy_controller.cpp source_identity.cpp shortcut_routing.cpp ui_command_state.cpp ui_layout.cpp ui_theme.cpp svg_icon_cache.cpp ui_chrome.cpp crop_strip.cpp context_menu.cpp ozviewer.rc)
    if(OZVIEWER_ENABLE_EDITING)
        list(APPEND OZVIEWER_SOURCES edit_controller.cpp resize_dialog.cpp save_controller.cpp)
    endif()
    add_executable(ozviewer WIN32 ${OZVIEWER_SOURCES})
    target_include_directories(ozviewer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(ozviewer PRIVATE cxx_std_23)
    # The GUI viewer uses wide Win32 APIs and resource macros (IDC_ARROW, ...),
    # which require the UNICODE/_UNICODE text-mapping macros to resolve to the
    # W variants. Scoped to this target only; Stage 0 is unchanged.
    target_compile_definitions(ozviewer PRIVATE UNICODE _UNICODE)
    target_link_libraries(ozviewer PRIVATE ozviewer_core oz_benchmark_markers)

    # Direct2D / Direct3D / DXGI system libraries + Shell / Common Dialogs / Theme / DWM (Stage 4, 7).
    target_link_libraries(ozviewer PRIVATE d2d1 d3d11 dxgi comdlg32 shell32 comctl32 uxtheme dwmapi)

    # libjpeg-turbo is transitively linked via ozviewer_core; link it explicitly
    # too so the viewer's own decode path is unambiguous (static-MD triplet
    # provides turbojpeg-static instead of turbojpeg).
    if(TARGET libjpeg-turbo::turbojpeg)
        target_link_libraries(ozviewer PRIVATE libjpeg-turbo::turbojpeg)
    else()
        target_link_libraries(ozviewer PRIVATE libjpeg-turbo::turbojpeg-static)
    endif()

    # When dynamic HEIC is enabled, copy heif.dll and libde265.dll to the
    # target directory alongside ozviewer.exe for bundle distribution and tests.
    if(OZVIEWER_ENABLE_HEIC)
        find_package(libheif CONFIG QUIET)
        if(TARGET heif)
            add_custom_command(TARGET ozviewer POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/heif.dll"
                    "$<TARGET_FILE_DIR:ozviewer>/heif.dll"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/libde265.dll"
                    "$<TARGET_FILE_DIR:ozviewer>/libde265.dll"
                COMMENT "Bundling heif.dll and libde265.dll alongside ozviewer.exe"
            )
        endif()
    endif()
else()
    message(STATUS "libjpeg-turbo not found via vcpkg; skipping the ozviewer "
                   "executable. Configure with the pinned vcpkg toolchain to "
                   "build the Stage 1 viewer.")
endif()
# ozviewer_core - testable viewer core (fit math, codec registry with JPEG
# validation/decode + WIC PNG decode + static WebP decode, marker emission).
# No Windows GUI dependencies; links the Stage 0 marker library and
# libjpeg-turbo (TurboJPEG C API) plus the Windows SDK imaging/COM libraries
# for WIC PNG. Static WebP (decoder+demux only) is optional behind
# OZVIEWER_ENABLE_WEBP (default ON): the supported release configures with
# the oz-x64-windows-static-md overlay triplet and fails clearly when the
# decoder/demux targets are unavailable; no-vcpkg or OFF builds stay clean
# and never advertise WebP. Static AVIF decode (dav1d decoder only) is
# optional behind OZVIEWER_ENABLE_AVIF (default ON).
option(OZVIEWER_ENABLE_WEBP "Enable static WebP decode (libwebp decoder+demux)" ON)
option(OZVIEWER_ENABLE_WEBP_ENCODER "Enable static WebP encoder capability (WebP::webp)" ON)
option(OZVIEWER_ENABLE_AVIF "Enable static AVIF decode (libavif+dav1d)" ON)
option(OZVIEWER_ENABLE_HEIC "Enable dynamic HEIC/HEIF decode (dynamically loaded libheif/libde265)" ON)
option(OZVIEWER_ENABLE_EDITING "Enable Stage 5 non-destructive edit engine and document model" ON)

set(OZVIEWER_CORE_SOURCES
    avif.cpp
    codec.cpp
    exif_orientation.cpp
    fit.cpp
    heic.cpp
    heif_loader.cpp
    isobmff.cpp
    jpeg.cpp
    marker_emit.cpp
    navigation.cpp
    navigation_benchmark.cpp
    navigation_benchmark_corpus.cpp
    navigation_benchmark_schema.cpp
    png_wic.cpp
    raster_wic.cpp
    tga.cpp
    webp.cpp)

if(OZVIEWER_ENABLE_EDITING)
    list(APPEND OZVIEWER_CORE_SOURCES
        crop_interaction.cpp
        edit_state.cpp
        edit_pipeline.cpp
        save_pipeline.cpp)
endif()

add_library(ozviewer_core STATIC ${OZVIEWER_CORE_SOURCES})
target_include_directories(ozviewer_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(ozviewer_core PUBLIC cxx_std_23)
if(OZVIEWER_ENABLE_EDITING)
    target_compile_definitions(ozviewer_core PUBLIC OZVIEWER_ENABLE_EDITING=1)
else()
    target_compile_definitions(ozviewer_core PUBLIC OZVIEWER_ENABLE_EDITING=0)
endif()
target_link_libraries(ozviewer_core PUBLIC oz_benchmark_markers oz_collector ozbench_lib shlwapi psapi bcrypt)
target_link_libraries(ozviewer_core PUBLIC windowscodecs ole32)

# libjpeg-turbo is required. When vcpkg is present the CONFIG target exists;
# otherwise the target is unavailable and the viewer build is blocked (the
# dependency-free core still builds and is tested). The static-MD triplet
# provides libjpeg-turbo::turbojpeg-static instead of ::turbojpeg.
find_package(libjpeg-turbo CONFIG QUIET)
if(TARGET libjpeg-turbo::turbojpeg)
    target_link_libraries(ozviewer_core PUBLIC libjpeg-turbo::turbojpeg)
    target_compile_definitions(ozviewer_core PUBLIC OZVIEWER_HAVE_TURBOJPEG=1)
elseif(TARGET libjpeg-turbo::turbojpeg-static)
    target_link_libraries(ozviewer_core PUBLIC libjpeg-turbo::turbojpeg-static)
    target_compile_definitions(ozviewer_core PUBLIC OZVIEWER_HAVE_TURBOJPEG=1)
else()
    target_compile_definitions(ozviewer_core PUBLIC OZVIEWER_HAVE_TURBOJPEG=0)
endif()

# Static WebP decode-only: WebP::webpdecoder + WebP::webpdemux.
# WebP save support (WebP::webp static encoder) is added when available and editing is enabled.
if(OZVIEWER_ENABLE_WEBP)
    find_package(WebP CONFIG QUIET)
    if(TARGET WebP::webpdecoder AND TARGET WebP::webpdemux)
        target_link_libraries(ozviewer_core PUBLIC WebP::webpdecoder WebP::webpdemux)
        target_compile_definitions(ozviewer_core PUBLIC OZVIEWER_HAVE_WEBP=1)
        if(OZVIEWER_ENABLE_EDITING AND OZVIEWER_ENABLE_WEBP_ENCODER AND TARGET WebP::webp)
            target_link_libraries(ozviewer_core PUBLIC WebP::webp)
            target_compile_definitions(ozviewer_core PUBLIC OZVIEWER_HAVE_WEBP_ENCODER=1)
        else()
            target_compile_definitions(ozviewer_core PUBLIC OZVIEWER_HAVE_WEBP_ENCODER=0)
        endif()
    elseif(VCPKG_TARGET_TRIPLET MATCHES "^oz-x64-windows-static-md$")
        message(FATAL_ERROR "OZVIEWER_ENABLE_WEBP=ON but WebP::webpdecoder/WebP::webpdemux not found. "
            "Configure the supported release with the pinned build/vcpkg toolchain, "
            "overlay triplet triplets/oz-x64-windows-static-md.cmake "
            "(-DVCPKG_TARGET_TRIPLET=oz-x64-windows-static-md "
            "-DVCPKG_OVERLAY_TRIPLETS=<repo>/triplets), or configure with -DOZVIEWER_ENABLE_WEBP=OFF.")
    else()
        message(STATUS "WebP decoder/demux not found; building without WebP support "
                       "(configure the oz-x64-windows-static-md release for WebP).")
        target_compile_definitions(ozviewer_core PUBLIC OZVIEWER_HAVE_WEBP=0)
        target_compile_definitions(ozviewer_core PUBLIC OZVIEWER_HAVE_WEBP_ENCODER=0)
    endif()
else()
    target_compile_definitions(ozviewer_core PUBLIC OZVIEWER_HAVE_WEBP=0)
    target_compile_definitions(ozviewer_core PUBLIC OZVIEWER_HAVE_WEBP_ENCODER=0)
endif()

# Static AVIF decode-only: libavif (dav1d decoder). Never encoder,
# DLL, delay-load, or LoadLibrary.
if(OZVIEWER_ENABLE_AVIF)
    find_package(libavif CONFIG QUIET)
    if(TARGET avif)
        target_link_libraries(ozviewer_core PUBLIC avif)
        target_compile_definitions(ozviewer_core PUBLIC OZVIEWER_HAVE_AVIF=1)
    elseif(VCPKG_TARGET_TRIPLET MATCHES "^oz-x64-windows-static-md$")
        message(FATAL_ERROR "OZVIEWER_ENABLE_AVIF=ON but target 'avif' not found. "
            "Configure the supported release with the pinned build/vcpkg toolchain, "
            "overlay triplet triplets/oz-x64-windows-static-md.cmake "
            "(-DVCPKG_TARGET_TRIPLET=oz-x64-windows-static-md "
            "-DVCPKG_OVERLAY_TRIPLETS=<repo>/triplets), or configure with -DOZVIEWER_ENABLE_AVIF=OFF.")
    else()
        message(STATUS "libavif not found; building without AVIF support "
                       "(configure the oz-x64-windows-static-md release for AVIF).")
        target_compile_definitions(ozviewer_core PUBLIC OZVIEWER_HAVE_AVIF=0)
    endif()
else()
    target_compile_definitions(ozviewer_core PUBLIC OZVIEWER_HAVE_AVIF=0)
endif()

# Dynamic HEIC decode: dynamically loaded libheif/libde265 DLLs.
# Requires libheif headers from vcpkg when enabled. Links NO import library!
# Missing DLLs at runtime is NOT a build failure.
if(OZVIEWER_ENABLE_HEIC)
    find_package(libheif CONFIG QUIET)
    if(TARGET heif)
        # Get interface include directories from heif target, but do NOT link it.
        get_target_property(HEIF_INCLUDES heif INTERFACE_INCLUDE_DIRECTORIES)
        if(HEIF_INCLUDES)
            target_include_directories(ozviewer_core PRIVATE ${HEIF_INCLUDES})
        endif()
        target_compile_definitions(ozviewer_core PUBLIC OZVIEWER_HAVE_HEIC=1)
    elseif(VCPKG_TARGET_TRIPLET MATCHES "^oz-x64-windows-static-md$")
        message(FATAL_ERROR "OZVIEWER_ENABLE_HEIC=ON but target 'heif' not found. "
            "Configure the supported release with the pinned build/vcpkg toolchain, "
            "overlay triplet triplets/oz-x64-windows-static-md.cmake "
            "(-DVCPKG_TARGET_TRIPLET=oz-x64-windows-static-md "
            "-DVCPKG_OVERLAY_TRIPLETS=<repo>/triplets), or configure with -DOZVIEWER_ENABLE_HEIC=OFF.")
    else()
        message(STATUS "libheif not found; building without HEIC support "
                       "(configure the oz-x64-windows-static-md release for HEIC).")
        target_compile_definitions(ozviewer_core PUBLIC OZVIEWER_HAVE_HEIC=0)
    endif()
else()
    target_compile_definitions(ozviewer_core PUBLIC OZVIEWER_HAVE_HEIC=0)
endif()

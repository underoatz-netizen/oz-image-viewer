cmake_minimum_required(VERSION 3.24)

project(ozbench
    VERSION 0.1.1
    DESCRIPTION "PikaView (formerly OZ Image Viewer) benchmark harness and native Windows image viewer"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global C++23 / MSVC settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /EHsc)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# ---------------------------------------------------------------------------
# Optional pinned nlohmann-json (vcpkg)
#
# The runner ships with a small internal JSON module (src/runner/json.cpp) so
# the harness builds without vcpkg. When a vcpkg toolchain is available, pass
# -DOZBENCH_USE_NLOHMANN=ON to use the pinned nlohmann-json instead; the
# runner code only ever touches JSON through the accessor layer in
# oz/runner/json.hpp, so both providers are interchangeable.
# ---------------------------------------------------------------------------
option(OZBENCH_USE_NLOHMANN "Use pinned nlohmann-json from vcpkg for the runner" OFF)
if(OZBENCH_USE_NLOHMANN)
    find_package(nlohmann_json CONFIG REQUIRED)
endif()

# Private bounded print-dialog diagnostics. Release builds must keep this OFF;
# the recovered print dialog/controller sources and release-provenance scripts
# expect this cache entry to exist explicitly.
option(OZVIEWER_PRINT_DIAGNOSTICS "Enable bounded stderr diagnostics for print dialog lifecycle" OFF)
if(OZVIEWER_PRINT_DIAGNOSTICS)
    add_compile_definitions(OZVIEWER_PRINT_DIAGNOSTICS)
endif()

add_subdirectory(src/markers)
add_subdirectory(src/runner)
add_subdirectory(src/collector)
add_subdirectory(src/fixture)
add_subdirectory(src/control_window)

# Stage 1: testable viewer core (always builds; JPEG decode requires vcpkg's
# libjpeg-turbo) and the WIN32 viewer executable (only when TurboJPEG is found).
add_subdirectory(src/viewer_core)
add_subdirectory(src/viewer)

enable_testing()
add_subdirectory(tests)
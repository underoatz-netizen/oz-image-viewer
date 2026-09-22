# oz_collector - external_visible_frame reference-benchmark collector.
#
# Measures ExternalFirstImageVisible via DXGI Desktop Duplication with a fixed
# client-area probe (see docs/reference-benchmark.md). Depends on the runner
# library for JSON/UTF helpers; links D3D11/DXGI for duplication, bcrypt for
# manifest sealing.
add_library(oz_collector STATIC
    probe.cpp
    manifest.cpp
    capture.cpp)
target_include_directories(oz_collector PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(oz_collector PUBLIC ozbench_lib)
target_compile_features(oz_collector PUBLIC cxx_std_23)

if(WIN32)
    target_link_libraries(oz_collector PUBLIC d3d11 dxgi dxguid user32 bcrypt shcore)
endif()
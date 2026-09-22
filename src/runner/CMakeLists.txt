add_library(ozbench_lib STATIC
    json.cpp
    plan.cpp
    record.cpp
    process.cpp
    collect.cpp
    summarize.cpp
    import.cpp
    run.cpp)
target_include_directories(ozbench_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ozbench_lib PUBLIC oz_benchmark_markers)
target_compile_features(ozbench_lib PUBLIC cxx_std_23)

if(WIN32)
    # CNG (bcrypt) for subject executable SHA-256 provenance.
    target_link_libraries(ozbench_lib PUBLIC bcrypt)
endif()

if(OZBENCH_USE_NLOHMANN)
    target_link_libraries(ozbench_lib PUBLIC nlohmann_json::nlohmann_json)
    target_compile_definitions(ozbench_lib PUBLIC OZBENCH_HAVE_NLOHMANN=1)
endif()

add_executable(ozbench main.cpp)
target_link_libraries(ozbench PRIVATE ozbench_lib oz_collector)
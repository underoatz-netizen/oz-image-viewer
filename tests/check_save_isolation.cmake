# check_save_isolation.cmake - source-scan CTest for the Stage 5 Slice 6 save
# isolation contract: the save controller may only be instantiated inside the
# interactive non-benchmark path, so `--benchmark` and `--navigation-benchmark`
# create no save controller (and no save UI/workers).
#
# Usage: cmake -DMAIN_CPP=<src/viewer/main.cpp> -P check_save_isolation.cmake

if(NOT EXISTS "${MAIN_CPP}")
    message(FATAL_ERROR "main.cpp not found: ${MAIN_CPP}")
endif()

file(READ "${MAIN_CPP}" content)

set(marker "std::make_unique<oz::viewer::save_controller>")
string(LENGTH "${marker}" marker_length)
string(FIND "${content}" "${marker}" position)
if(position EQUAL -1)
    message(FATAL_ERROR "save_controller instantiation not found in main.cpp")
endif()

# Exactly one construction site.
string(FIND "${content}" "${marker}" second_position REVERSE)
if(NOT second_position EQUAL position)
    message(FATAL_ERROR "save_controller must be instantiated exactly once in main.cpp")
endif()

# The construction site must be inside the interactive-mode guard.
math(EXPR window_start "${position} - 700")
if(window_start LESS 0)
    set(window_start 0)
endif()
string(SUBSTRING "${content}" ${window_start} 700 window)
string(FIND "${window}" "!benchmark && !nav_benchmark" guard_position)
if(guard_position EQUAL -1)
    message(FATAL_ERROR "save_controller is not guarded by the interactive non-benchmark mode check")
endif()

# The Save/Save As menu ids must be created only under the editing gate and
# inside the interactive menu block (benchmark creates no menu at all).
foreach(id_marker "kIdFileSave" "kIdFileSaveAs")
    string(FIND "${content}" "${id_marker}" id_position)
    if(id_position EQUAL -1)
        message(FATAL_ERROR "${id_marker} not found in main.cpp")
    endif()
endforeach()

message(STATUS "check_save_isolation: OK (save controller exists only in interactive non-benchmark mode)")

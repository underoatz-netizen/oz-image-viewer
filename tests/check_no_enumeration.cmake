# check_no_enumeration.cmake - source-scan CTest that rejects folder-enumeration
# APIs in the Stage 1 viewer sources. Stage 1 must not enumerate directories,
# derive sibling paths, or use shell folder APIs before FirstPresent.
#
# Usage: cmake -DSRC_DIR=<dir> -P check_no_enumeration.cmake
# Exits non-zero if any forbidden pattern is found.

set(FORBIDDEN
    "FindFirstFile"
    "FindNextFile"
    "FindFirstFileEx"
    "directory_iterator"
    "recursive_directory_iterator"
    "SHGetFolderPath"
    "SHGetKnownFolderPath"
    "SHBrowseForFolder"
    "IFileDialog"
    "GetCurrentDirectory"
    "GetModuleFileName"
    "GetFullPathName"
    "PathFindFileName"
    "PathRemoveFileSpec"
    "PathCchRemoveFileSpec"
    "wcsrchr"          # wildcard/sibling derivation via last-backslash
    "GetFileAttributes"
)

file(GLOB_RECURSE SOURCES "${SRC_DIR}/*.cpp" "${SRC_DIR}/*.hpp" "${SRC_DIR}/*.h")
set(fail 0)
foreach(src ${SOURCES})
    file(READ "${src}" content)
    foreach(pat ${FORBIDDEN})
        string(FIND "${content}" "${pat}" pos)
        if(NOT pos EQUAL -1)
            message(FATAL_ERROR "Forbidden folder-enumeration/sibling API '${pat}' in ${src}")
        endif()
    endforeach()
endforeach()
message(STATUS "check_no_enumeration: OK (no folder-enumeration APIs in viewer sources)")
# check_navigation_containment.cmake - verifies that directory enumeration
# and sibling derivation are strictly confined to the dedicated navigation component
# (ozviewer_core/navigation.cpp) and forbidden anywhere in viewer UI/startup sources.
#
# Usage: cmake -DVIEWER_DIR=<dir> -DNAV_FILE=<file> -P check_navigation_containment.cmake

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
    "wcsrchr"
    "GetFileAttributes"
)

# 1. Check viewer sources: none of the forbidden enumeration APIs may appear.
file(GLOB_RECURSE SOURCES "${VIEWER_DIR}/*.cpp" "${VIEWER_DIR}/*.hpp" "${VIEWER_DIR}/*.h")
foreach(src ${SOURCES})
    file(READ "${src}" content)
    foreach(pat ${FORBIDDEN})
        string(FIND "${content}" "${pat}" pos)
        if(NOT pos EQUAL -1)
            message(FATAL_ERROR "Forbidden folder-enumeration/sibling API '${pat}' in ${src}")
        endif()
    endforeach()
endforeach()

# 2. Check dedicated navigation component: verify FindFirstFileW is contained here.
if(NOT EXISTS "${NAV_FILE}")
    message(FATAL_ERROR "Navigation component file '${NAV_FILE}' not found")
endif()
file(READ "${NAV_FILE}" nav_content)
string(FIND "${nav_content}" "FindFirstFileW" nav_pos)
if(nav_pos EQUAL -1)
    message(FATAL_ERROR "Expected FindFirstFileW in navigation component ${NAV_FILE}")
endif()

message(STATUS "check_navigation_containment: OK (enumeration strictly isolated to navigation component)")

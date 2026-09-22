# oz-x64-windows-static-md - project overlay triplet for the supported
# WebP/AVIF/HEIC release. x64, dynamic CRT (/MD), static libraries by default.
# Per-port conditional: libheif and libde265 get dynamic library linkage
# to allow standalone DLL bundling and lazy runtime loading without LGPL static link.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

if(PORT STREQUAL "libheif" OR PORT STREQUAL "libde265")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()


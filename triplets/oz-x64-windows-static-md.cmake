# oz-x64-windows-static-md - project overlay triplet for the supported
# WebP/AVIF/HEIC release. x64, dynamic CRT (/MD), static libraries by default.
# Per-port conditional: libheif and libde265 get dynamic library linkage
# to allow standalone DLL bundling and lazy runtime loading without LGPL static link.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_PLATFORM_TOOLSET v143)
set(VCPKG_C_FLAGS "/experimental:deterministic")
set(VCPKG_CXX_FLAGS "/experimental:deterministic")

if(PORT STREQUAL "libheif")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
    set(VCPKG_LINKER_FLAGS "/Brepro /PDBALTPATH:heif.pdb")
elseif(PORT STREQUAL "libde265")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
    set(VCPKG_LINKER_FLAGS "/Brepro /PDBALTPATH:libde265.pdb")
endif()


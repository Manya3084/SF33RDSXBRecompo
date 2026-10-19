# Cross-compiling for 64-bit Windows with MinGW-w64:
#     cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake \
#           -DSF_WIN_SYSROOT=/path/to/root/mingw64 -DSF_DECOMP_DIR=...
# SF_WIN_SYSROOT holds the Windows builds of SDL3, OpenAL Soft, libarchive,
# zlib and the Vulkan loader (MSYS2's mingw-w64-x86_64 packages, unpacked).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(SF_WIN_SYSROOT "" CACHE PATH "the Windows libraries (an unpacked MSYS2 mingw64 tree)")
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32 ${SF_WIN_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# pkg-config (libarchive) from the same tree: its .pc files say /mingw64
if(SF_WIN_SYSROOT)
    get_filename_component(_sf_win_root "${SF_WIN_SYSROOT}" DIRECTORY)
    set(ENV{PKG_CONFIG_LIBDIR} "${SF_WIN_SYSROOT}/lib/pkgconfig")
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "${_sf_win_root}")
endif()

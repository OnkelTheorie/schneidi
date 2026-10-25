# Cross build for 64-bit ARM Linux (e.g. Raspberry Pi with Debian trixie) on an x86-64 host.
# The sysroot is a copy of the target's /usr/include, /usr/lib/aarch64-linux-gnu, /usr/lib/gcc/aarch64-linux-gnu
# and /usr/lib/qt6 (all -dev packages installed there). Qt host tools (moc, rcc, uic) come from the host,
# which must have the same Qt version.
#   SCHNEIDI_SYSROOT=<dir> cmake -S . -B build-aarch64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-clang.cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT DEFINED ENV{SCHNEIDI_SYSROOT})
    message(FATAL_ERROR "Set SCHNEIDI_SYSROOT to the target sysroot")
endif()
set(CMAKE_SYSROOT $ENV{SCHNEIDI_SYSROOT})
set(_triple aarch64-linux-gnu)

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET ${_triple})
set(CMAKE_CXX_COMPILER_TARGET ${_triple})
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(QT_HOST_PATH /usr CACHE PATH "Host Qt for moc/rcc/uic")
set(QT_HOST_PATH_CMAKE_DIR /usr/lib/x86_64-linux-gnu/cmake CACHE PATH "Host Qt CMake packages (Debian layout)")

set(ENV{PKG_CONFIG_DIR} "")
set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_SYSROOT}/usr/lib/${_triple}/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} ${CMAKE_SYSROOT})

cmake_minimum_required(VERSION 3.14)
project(obmafs3 C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------- Fetch xxHash ----------
include(FetchContent)
FetchContent_Declare(
    xxHash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG        v0.8.2
    SOURCE_SUBDIR  cmake_unofficial
)
FetchContent_MakeAvailable(xxHash)

# ---------- Fetch zstd ----------
FetchContent_Declare(
    zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG        v1.5.6
    SOURCE_SUBDIR  build/cmake
)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC   ON  CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zstd)

# ---------- Force optimization for dependencies in Debug builds ----------
# ZSTD and xxHash inner loops are extremely sensitive to -O0; compiling them
# without optimisation makes compression 3-4× slower while providing minimal
# debugging value (we never need to step through third-party library code).
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(libzstd_static PRIVATE -O2)
    target_compile_options(xxhash         PRIVATE -O2)
endif()

# ---------- Find FUSE3 (system) ----------
find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE3 REQUIRED fuse3>=3.0)

# ---------- Subdirectories ----------
add_subdirectory(src)

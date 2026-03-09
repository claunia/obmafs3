# LZMA SDK 26.00 — minimal static library build for OBMAFS3
#
# Builds only the LZMA1 encoder/decoder with the C-optimized match finder.
# On ARM64, uses the hand-tuned assembly decoder (LzmaDecOpt.S).
# The LZMA SDK is placed in the public domain by Igor Pavlov.

set(LZMA_C_DIR ${CMAKE_CURRENT_SOURCE_DIR}/C)

# --- Core sources (always needed) ---
set(LZMA_SOURCES
    ${LZMA_C_DIR}/Alloc.c
    ${LZMA_C_DIR}/CpuArch.c
    ${LZMA_C_DIR}/LzmaDec.c
    ${LZMA_C_DIR}/LzmaEnc.c
    ${LZMA_C_DIR}/LzmaLib.c
    ${LZMA_C_DIR}/LzFind.c
    ${LZMA_C_DIR}/LzFindOpt.c
    ${LZMA_C_DIR}/LzFindMt.c
    ${LZMA_C_DIR}/Threads.c
)

set(LZMA_COMPILE_DEFS "")

# --- CPU-optimized decoder (platform-specific) ---
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    # ARM64: GAS assembly decoder — significantly faster than C
    enable_language(ASM)
    list(APPEND LZMA_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/Asm/arm64/LzmaDecOpt.S
    )
    list(APPEND LZMA_COMPILE_DEFS Z7_LZMA_DEC_OPT)
    message(STATUS "LZMA SDK: ARM64 optimized decoder enabled")
else()
    message(STATUS "LZMA SDK: Using C decoder for ${CMAKE_SYSTEM_PROCESSOR}")
endif()

add_library(lzma_static STATIC ${LZMA_SOURCES})

target_include_directories(lzma_static PUBLIC ${LZMA_C_DIR})

target_compile_definitions(lzma_static PRIVATE ${LZMA_COMPILE_DEFS})

# Suppress warnings from third-party SDK code
target_compile_options(lzma_static PRIVATE -w)

# Keep optimized even in debug builds — same as zstd/xxhash.
# LZMA inner loops are extremely branch-heavy and 10x+ slower at -O0.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(lzma_static PRIVATE -O2)
endif()

# pthreads required by Threads.c / LzFindMt.c
find_package(Threads REQUIRED)
target_link_libraries(lzma_static PRIVATE Threads::Threads)

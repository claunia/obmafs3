add_library(obmafs STATIC
    superblock.c
    btree.c
    bitmap.c
    block.c
    checksum.c
    io.c
)

target_include_directories(obmafs PUBLIC
    ${CMAKE_SOURCE_DIR}/src/include
)

target_link_libraries(obmafs PUBLIC
    xxhash
    libzstd_static
)

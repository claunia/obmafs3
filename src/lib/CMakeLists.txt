add_library(obmafs STATIC
    superblock.c
    btree.c
    catalog.c
    inode.c
    media_tag.c
    cd_btree.c
    metadata.c
    bitmap.c
    block.c
    checksum.c
    dedup.c
    ecc_cd.c
    io.c
)

target_include_directories(obmafs PUBLIC
    ${CMAKE_SOURCE_DIR}/src/include
)

find_package(Threads REQUIRED)

target_link_libraries(obmafs PUBLIC
    xxhash
    libzstd_static
    Threads::Threads
)

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
    dedup_cache.c
    dedup_tree.c
    dedup_write.c
    dedup_read.c
    dedup_persist.c
    dedup_housekeeping.c
    ecc_cd.c
    refcount.c
    sector_tag.c
    io.c
    aes128.c
    sha1.c
    nintendo.c
    nintendo_read.c
    junk_map.c
)

target_include_directories(obmafs PUBLIC
    ${CMAKE_SOURCE_DIR}/src/include
)

find_package(Threads REQUIRED)

target_link_libraries(obmafs PUBLIC
    xxhash
    libzstd_static
    lzma_static
    Threads::Threads
)

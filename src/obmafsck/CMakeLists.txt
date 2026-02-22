add_executable(obmafsck
    main.c
    fsck_util.c
    fsck_superblock.c
    fsck_progress.c
    fsck_btree.c
    fsck_collect.c
    fsck_scrub.c
    fsck_hash.c
    fsck_dedup.c
    fsck_metadata.c
    fsck_crosscheck.c
    fsck_extents.c
    fsck_defrag.c
)
target_link_libraries(obmafsck obmafs)

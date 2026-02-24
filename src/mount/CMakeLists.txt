add_executable(mount.obmafs
    main.c
    fuse_ops.c
    fuse_read.c
    fuse_write.c
    fuse_clone.c
    fuse_dir.c
    fuse_attr.c
    fuse_xattr.c
    fuse_ioctl.c
)

target_include_directories(mount.obmafs PRIVATE
    ${FUSE3_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(mount.obmafs
    obmafs
    ${FUSE3_LIBRARIES}
)

target_compile_options(mount.obmafs PRIVATE
    ${FUSE3_CFLAGS_OTHER}
)

if(HAVE_FUSE_STATX)
    target_compile_definitions(mount.obmafs PRIVATE HAVE_FUSE_STATX)
endif()

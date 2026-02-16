add_executable(mount.obmafs
    main.c
    fuse_ops.c
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

add_executable(import-ngcw
    main.c
    disc.c
    partition.c
    import.c
    metadata.c
    junk_map.c
)

target_include_directories(import-ngcw PRIVATE
    ${CMAKE_SOURCE_DIR}/src/include
    ${CMAKE_SOURCE_DIR}/src/lib
)

target_link_libraries(import-ngcw
    obmafs
)

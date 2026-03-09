add_executable(import-ps3
    main.c
    disc.c
    ird.c
    iso9660.c
    sfo.c
    import.c
    metadata.c
)

target_include_directories(import-ps3 PRIVATE
    ${CMAKE_SOURCE_DIR}/src/include
    ${CMAKE_SOURCE_DIR}/src/lib
)

target_link_libraries(import-ps3
    obmafs
    z
)

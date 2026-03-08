add_executable(import-wiiu
    main.c
    disc.c
    import.c
    metadata.c
)

target_include_directories(import-wiiu PRIVATE
    ${CMAKE_SOURCE_DIR}/src/include
    ${CMAKE_SOURCE_DIR}/src/lib
)

target_link_libraries(import-wiiu
    obmafs
)

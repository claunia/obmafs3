find_package(PkgConfig REQUIRED)
pkg_check_modules(NCURSES REQUIRED ncursesw)

add_executable(defrag
    main.c
    defrag_tui.c
    defrag_map.c
)

target_include_directories(defrag PRIVATE
    ${CMAKE_SOURCE_DIR}/src/include
    ${NCURSES_INCLUDE_DIRS}
)

target_link_libraries(defrag
    obmafs
    ${NCURSES_LIBRARIES}
)

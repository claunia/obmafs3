add_executable(import-aif main.c)

target_include_directories(import-aif PRIVATE ${CMAKE_SOURCE_DIR}/src/include)

target_link_libraries(import-aif
    aaruformat
)

add_executable(obmafs-query main.c)

target_include_directories(obmafs-query PRIVATE ${CMAKE_SOURCE_DIR}/src/include)
target_link_libraries(obmafs-query readline)

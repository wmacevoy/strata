# Vendored SQLite 3.47.2 amalgamation — static library
cmake_minimum_required(VERSION 3.16)

add_library(sqlite3_static STATIC sqlite3.c)
target_include_directories(sqlite3_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(sqlite3_static PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_ENABLE_FTS5
    SQLITE_ENABLE_JSON1
)
target_compile_options(sqlite3_static PRIVATE -w)
if(NOT APPLE)
    target_link_libraries(sqlite3_static pthread dl m)
endif()

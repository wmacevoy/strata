cmake_minimum_required(VERSION 3.16)
project(strata C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# macOS Homebrew paths
if(APPLE)
    execute_process(
        COMMAND brew --prefix
        OUTPUT_VARIABLE HOMEBREW_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    list(APPEND CMAKE_PREFIX_PATH
        "${HOMEBREW_PREFIX}/opt/sqlite"
        "${HOMEBREW_PREFIX}/opt/zeromq"
        "${HOMEBREW_PREFIX}/opt/wasm-micro-runtime"
    )
endif()

# SQLite
find_path(SQLITE3_INCLUDE_DIR sqlite3.h)
find_library(SQLITE3_LIBRARY sqlite3)
if(NOT SQLITE3_INCLUDE_DIR OR NOT SQLITE3_LIBRARY)
    message(FATAL_ERROR "SQLite3 not found")
endif()

# ZeroMQ
find_path(ZMQ_INCLUDE_DIR zmq.h)
find_library(ZMQ_LIBRARY zmq)
if(NOT ZMQ_INCLUDE_DIR OR NOT ZMQ_LIBRARY)
    message(FATAL_ERROR "ZeroMQ not found")
endif()

# WAMR
find_path(WAMR_INCLUDE_DIR wasm_export.h)
find_library(WAMR_LIBRARY iwasm)
if(NOT WAMR_INCLUDE_DIR OR NOT WAMR_LIBRARY)
    message(FATAL_ERROR "WAMR (wasm-micro-runtime) not found")
endif()

# OpenSSL (needed on Linux for SHA-256; macOS uses CommonCrypto)
if(NOT APPLE)
    find_package(OpenSSL REQUIRED)
endif()

# QuickJS (vendored)
add_library(quickjs
    vendor/quickjs/quickjs.c
    vendor/quickjs/cutils.c
    vendor/quickjs/libbf.c
    vendor/quickjs/libregexp.c
    vendor/quickjs/libunicode.c
)
target_include_directories(quickjs PUBLIC vendor/quickjs)
target_compile_definitions(quickjs PRIVATE
    CONFIG_VERSION="2024-01-13"
    _GNU_SOURCE
    CONFIG_BIGNUM
)
# Suppress warnings in vendored code
target_compile_options(quickjs PRIVATE -w)

# Strata library
add_library(strata
    src/schema.c
    src/store.c
    src/store_sqlite.c
    src/context.c
    src/change.c
    src/den.c
    src/blob.c
    src/village.c
)
target_compile_definitions(strata PRIVATE VILLAGE_NO_MAIN)
target_include_directories(strata PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/vendor/quickjs
    ${SQLITE3_INCLUDE_DIR}
    ${ZMQ_INCLUDE_DIR}
    ${WAMR_INCLUDE_DIR}
)
target_link_libraries(strata quickjs ${SQLITE3_LIBRARY} ${ZMQ_LIBRARY} ${WAMR_LIBRARY})
if(NOT APPLE)
    target_link_libraries(strata OpenSSL::Crypto)
endif()

# Store service
add_library(store_service_lib OBJECT src/store_service.c)
target_include_directories(store_service_lib PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${SQLITE3_INCLUDE_DIR}
    ${ZMQ_INCLUDE_DIR}
)
target_compile_definitions(store_service_lib PRIVATE STORE_SERVICE_NO_MAIN)

add_executable(store_service src/store_service.c)
target_include_directories(store_service PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${SQLITE3_INCLUDE_DIR}
    ${ZMQ_INCLUDE_DIR}
)
target_link_libraries(store_service strata)

# Tests
enable_testing()

add_executable(test_store test/test_store.c)
target_link_libraries(test_store strata)
add_test(NAME test_store COMMAND test_store)

add_executable(test_change test/test_change.c)
target_link_libraries(test_change strata)
add_test(NAME test_change COMMAND test_change)

add_executable(test_den test/test_den.c)
target_link_libraries(test_den strata)
add_test(NAME test_den COMMAND test_den WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_board test/test_board.c $<TARGET_OBJECTS:store_service_lib>)
target_link_libraries(test_board strata)
add_test(NAME test_board COMMAND test_board WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_blob test/test_blob.c)
target_link_libraries(test_blob strata)
add_test(NAME test_blob COMMAND test_blob)

add_executable(test_village test/test_village.c $<TARGET_OBJECTS:store_service_lib>)
target_link_libraries(test_village strata)
add_test(NAME test_village COMMAND test_village WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_claud_homestead test/test_claud_homestead.c $<TARGET_OBJECTS:store_service_lib>)
target_link_libraries(test_claud_homestead strata)
add_test(NAME test_claud_homestead COMMAND test_claud_homestead WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_collaboration test/test_collaboration.c $<TARGET_OBJECTS:store_service_lib>)
target_link_libraries(test_collaboration strata)
add_test(NAME test_collaboration COMMAND test_collaboration WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Code-smith vocation service
add_library(code_smith_lib OBJECT src/code_smith.c)
target_include_directories(code_smith_lib PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${ZMQ_INCLUDE_DIR}
)
target_compile_definitions(code_smith_lib PRIVATE CODE_SMITH_NO_MAIN)

add_executable(code_smith src/code_smith.c)
target_include_directories(code_smith PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${ZMQ_INCLUDE_DIR}
)
target_link_libraries(code_smith ${ZMQ_LIBRARY})

# Cobbler vocation (optional — requires clang with WASM target)
find_program(WASM_CLANG clang PATHS /opt/homebrew/opt/llvm/bin NO_DEFAULT_PATH)
if(NOT WASM_CLANG)
    find_program(WASM_CLANG clang)
endif()
if(WASM_CLANG)
    execute_process(COMMAND ${WASM_CLANG} --target=wasm32 -x c -c -o /dev/null /dev/null
                    RESULT_VARIABLE WASM_CLANG_RC ERROR_QUIET)
    if(NOT WASM_CLANG_RC EQUAL 0)
        set(WASM_CLANG "")
    endif()
endif()

if(WASM_CLANG)
    message(STATUS "Cobbler enabled: ${WASM_CLANG}")
    add_library(cobbler_lib OBJECT src/cobbler.c)
    target_include_directories(cobbler_lib PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${ZMQ_INCLUDE_DIR}
    )
    target_compile_definitions(cobbler_lib PRIVATE COBBLER_NO_MAIN)

    add_executable(cobbler src/cobbler.c)
    target_include_directories(cobbler PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${ZMQ_INCLUDE_DIR}
    )
    target_link_libraries(cobbler ${ZMQ_LIBRARY})
else()
    message(STATUS "Cobbler disabled: no clang with WASM support found")
endif()

# Village daemon
add_executable(village_daemon src/village.c)
target_include_directories(village_daemon PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${ZMQ_INCLUDE_DIR}
)
target_link_libraries(village_daemon strata)

# CLI tools
add_executable(strata_cli src/cli_strata.c)
target_link_libraries(strata_cli strata)
set_target_properties(strata_cli PROPERTIES OUTPUT_NAME "strata")

add_executable(strata_den_cli src/cli_strata_den.c)
target_link_libraries(strata_den_cli strata)
set_target_properties(strata_den_cli PROPERTIES OUTPUT_NAME "strata-den")

add_executable(strata_homestead_cli src/cli_strata_homestead.c $<TARGET_OBJECTS:store_service_lib>)
target_link_libraries(strata_homestead_cli strata)
set_target_properties(strata_homestead_cli PROPERTIES OUTPUT_NAME "strata-homestead")

add_executable(strata_human_cli src/cli_human_den.c)
target_link_libraries(strata_human_cli strata)
set_target_properties(strata_human_cli PROPERTIES OUTPUT_NAME "strata-human")

# Warren's Village
if(WASM_CLANG)
    add_executable(warren_village src/warren_village.c $<TARGET_OBJECTS:store_service_lib> $<TARGET_OBJECTS:code_smith_lib> $<TARGET_OBJECTS:cobbler_lib>)
    target_compile_definitions(warren_village PRIVATE HAVE_COBBLER)
else()
    add_executable(warren_village src/warren_village.c $<TARGET_OBJECTS:store_service_lib> $<TARGET_OBJECTS:code_smith_lib>)
endif()
target_link_libraries(warren_village strata)

# Cobbler test (conditional)
if(WASM_CLANG)
    add_executable(test_cobbler test/test_cobbler.c $<TARGET_OBJECTS:cobbler_lib>)
    target_include_directories(test_cobbler PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${ZMQ_INCLUDE_DIR}
    )
    target_link_libraries(test_cobbler ${ZMQ_LIBRARY})
    add_test(NAME test_cobbler COMMAND test_cobbler WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# Examples
add_executable(listener examples/listener.c)
target_link_libraries(listener strata)

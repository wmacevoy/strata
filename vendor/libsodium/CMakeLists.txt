# Minimal CMake wrapper for vendored libsodium 1.0.20
# Builds a static library with all source files.

cmake_minimum_required(VERSION 3.16)

file(GLOB_RECURSE SODIUM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.c")

# Include x86_64 assembly for optimized curve25519/salsa20
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    enable_language(ASM)
    file(GLOB_RECURSE SODIUM_ASM "${CMAKE_CURRENT_SOURCE_DIR}/*.S")
    list(APPEND SODIUM_SOURCES ${SODIUM_ASM})
endif()

add_library(sodium_static STATIC ${SODIUM_SOURCES})

target_include_directories(sodium_static PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_include_directories(sodium_static PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/sodium
)

target_compile_definitions(sodium_static PUBLIC SODIUM_STATIC)
target_compile_definitions(sodium_static PRIVATE
    CONFIGURED=1
    NATIVE_LITTLE_ENDIAN
)

# 64-bit GCC/Clang have __int128 support
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_compile_definitions(sodium_static PRIVATE HAVE_TI_MODE)
endif()

# Inline asm and CPUID for GCC/Clang
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_definitions(sodium_static PRIVATE HAVE_INLINE_ASM)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|i[3-6]86")
        target_compile_definitions(sodium_static PRIVATE HAVE_CPUID HAVE_AVX_ASM)
    endif()
endif()

# Suppress all warnings in vendored code
target_compile_options(sodium_static PRIVATE -w)

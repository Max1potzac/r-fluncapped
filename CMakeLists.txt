cmake_minimum_required(VERSION 3.24)
project(r-fluncapped LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RFU_BUILD_TESTS "Build the test suite" ON)

include(cmake/Dependencies.cmake)

add_library(rfu_core STATIC
    src/core/guest_memory.cpp
    src/core/guest_heap.cpp
    src/core/module.cpp
    src/core/linker.cpp
    src/core/log.cpp
    src/core/vfs.cpp
    src/cpu/cpu.cpp
    src/jni/jni.cpp
    src/shims/registry.cpp
    src/shims/printf.cpp
    src/shims/libc_memory.cpp
    src/shims/libc_string.cpp
    src/shims/libc_stdio.cpp
    src/shims/libc_math.cpp
    src/shims/libc_misc.cpp
    src/shims/libc_pthread.cpp
    src/shims/libdl.cpp
    src/shims/libz.cpp
    src/runtime.cpp
)
target_include_directories(rfu_core PUBLIC src)
find_package(ZLIB REQUIRED)
target_link_libraries(rfu_core PUBLIC dynarmic fmt::fmt ZLIB::ZLIB)
if (MSVC)
    # C4100: unused parameter, which every shim that ignores its CallContext has.
    target_compile_options(rfu_core PRIVATE /W4 /permissive- /utf-8 /wd4100)
    target_compile_definitions(rfu_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(rfu_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_executable(rfu src/main.cpp)
target_link_libraries(rfu PRIVATE rfu_core)

if (RFU_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

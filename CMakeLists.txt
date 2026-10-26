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
    src/core/hooks.cpp
    src/core/patches.cpp
    src/core/rtti.cpp
    src/core/trace.cpp
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
    src/shims/libc_net.cpp
    src/shims/libc_pthread.cpp
    src/shims/libdl.cpp
    src/shims/libz.cpp
    src/runtime.cpp
    src/game/game_keys.cpp
    src/game/hover.cpp
    # SDL-free parts of src/platform, here so the tests reach them
    src/platform/keybinds.cpp
    src/platform/png.cpp
)
target_include_directories(rfu_core PUBLIC src)
find_package(ZLIB REQUIRED)
target_link_libraries(rfu_core PUBLIC dynarmic fmt::fmt ZLIB::ZLIB)
if (WIN32)
    target_link_libraries(rfu_core PUBLIC ws2_32)
endif()
if (MSVC)
    # C4100: unused parameter, which every shim that ignores its CallContext has.
    target_compile_options(rfu_core PRIVATE /W4 /permissive- /utf-8 /wd4100)
    target_compile_definitions(rfu_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(rfu_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# The window, OpenGL ES, audio and the game's Java side: everything that needs SDL2.
find_package(SDL2 CONFIG REQUIRED)
add_library(rfu_app STATIC
    src/platform/etc.cpp
    src/platform/gles.cpp
    src/platform/http.cpp
    src/platform/opensles.cpp
    src/platform/window.cpp
    src/android/app.cpp
    src/android/java_http.cpp
)
target_link_libraries(rfu_app PUBLIC rfu_core $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>)
# The game's own (Java-side) web requests: WinHTTP on Windows, libcurl elsewhere.
if (WIN32)
    target_link_libraries(rfu_app PUBLIC winhttp)
else()
    find_package(CURL)
    if (CURL_FOUND)
        target_link_libraries(rfu_app PUBLIC CURL::libcurl)
        target_compile_definitions(rfu_app PRIVATE RFU_HAVE_CURL)
    else()
        message(WARNING "libcurl not found: the game's own web requests will fail (its CDN client still works)")
    endif()
endif()
if (MSVC)
    target_compile_options(rfu_app PRIVATE /W4 /permissive- /utf-8 /wd4100)
else()
    target_compile_options(rfu_app PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_executable(rfu src/main.cpp)
target_link_libraries(rfu PRIVATE rfu_app)

if (RFU_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

add_executable(rfu_tests runtime_tests.cpp)
target_link_libraries(rfu_tests PRIVATE rfu_core)

# The guest test library is arm64 Android code, so it needs clang and lld (any recent LLVM
# can target aarch64-linux-android without the NDK). Without them only host tests run.
find_program(RFU_GUEST_CLANG NAMES clang clang-18 clang-17 clang-16)
find_program(RFU_GUEST_LLD NAMES ld.lld lld)
if (RFU_GUEST_CLANG AND RFU_GUEST_LLD)
    set(guest_dir ${CMAKE_CURRENT_BINARY_DIR}/guest)
    add_custom_command(
        OUTPUT ${guest_dir}/libtest.so
        COMMAND ${CMAKE_COMMAND} -E make_directory ${guest_dir}
        COMMAND ${RFU_GUEST_CLANG} --target=aarch64-linux-android21 -O2 -fPIC -shared -nostdlib
                -ffreestanding -fstack-protector-all -fuse-ld=lld -Wl,-soname,libtest.so
                -o ${guest_dir}/libtest.so ${CMAKE_CURRENT_SOURCE_DIR}/guest/testlib.c
        DEPENDS guest/testlib.c
        COMMENT "Building the arm64 guest test library")
    add_custom_target(rfu_guest_lib ALL DEPENDS ${guest_dir}/libtest.so)
    add_dependencies(rfu_tests rfu_guest_lib)
    add_test(NAME runtime COMMAND rfu_tests ${guest_dir})
else()
    message(STATUS "clang/lld not found: guest tests disabled")
    add_test(NAME runtime COMMAND rfu_tests)
endif()

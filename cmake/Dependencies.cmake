# Third-party dependencies.
#
# dynarmic (ARM64 -> x86-64 recompiler) is fetched at a pinned commit together with the
# externals it bundles. It needs Boost headers (icl, variant): on Windows install them with
# vcpkg (see vcpkg.json), on Linux with your package manager (libboost-dev). zlib comes from
# the same places (vcpkg, zlib1g-dev).

include(FetchContent)

set(DYNARMIC_FRONTENDS "A64" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(dynarmic
    GIT_REPOSITORY https://github.com/azahar-emu/dynarmic.git
    GIT_TAG        a46601580d5512d324104f985b5f0209dc980ddc
    GIT_SUBMODULES externals/fmt externals/mcl externals/robin-map externals/xbyak externals/zydis
    GIT_SUBMODULES_RECURSE ON
)
FetchContent_MakeAvailable(dynarmic)

# =============================================================================
# Third-party dependencies: all managed by FetchContent
#
# The 8 dependencies: CLI11 / spdlog / DiligentEngine / glfw / googletest / grpc /
# expected / nlohmann_json. imgui is compiled from the sources under DiligentTools/ThirdParty but is
# declared with FetchContent as well, so it can reuse the same local cache.
#
# Default behaviour: reuse an existing checkout under FILEBROWSER_VENDOR_ROOT through
# FETCHCONTENT_SOURCE_DIR_<UPPER>, with no network access. Remove those cache
# variables, or use -DFILEBROWSER_USE_LOCAL_VENDOR=OFF, to fall back to a git
# clone of the repositories/tags declared below.
#
# Cross-compiling: the host protoc / grpc_cpp_plugin are prepared by cmake/HostTools.cmake before
# gRPC is added, so no -D flags are needed on the command line.
# =============================================================================

include(FetchContent)
include(HostTools)

# Diligent / imgui only need to be "fetched"; we do not want FetchContent to run
# add_subdirectory for them (the targets are assembled by cmake/Diligent.cmake and
# cmake/Imgui.cmake), so FetchContent_Populate is used here. CMake 4.x warns about
# CMP0169, which is explicitly kept at OLD behaviour.
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

# Dependencies such as zlib (2.4.4) and c-ares (3.1) declare a
# cmake_minimum_required below 3.5; CMake 4.x needs an explicit policy floor,
# otherwise configuration fails.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Warning codes reported by the vendor sources themselves (our own code reports none):
#   C5287 different enum types compared - gRPC's generated upb headers
#   C4141 'inline' used more than once - protobuf headers
#   C4624 destructor implicitly defined as deleted - promise/detail/seq_state.h
#   C4715 not all control paths return a value - promise/party.h
#   C4116 unnamed type definition in parentheses - upb
#   C4805 unsafe mix of int and bool - promise_based_filter.h
#   C4312 reinterpret_cast to a larger pointer - chttp2_transport.cc
# These are vendor-side issues (fixing them would mean patching the fetched sources),
# so they are silenced for the dependencies only; our own targets keep the full warning level.
# Keep this as one quoted string, not a list: a list would expand with ';' separators and the
# VS generator would then pass the whole thing to CL.exe as a single argument (error D8021).
set(FB_VENDOR_WARNING_FLAGS "")
if(MSVC)
    set(FB_VENDOR_WARNING_FLAGS "/wd5287 /wd4141 /wd4624 /wd4715 /wd4116 /wd4805 /wd4312")
endif()

# CMAKE_CXX_FLAGS / CMAKE_C_FLAGS are inherited downwards by add_subdirectory, so adding the
# flags only for the duration of FetchContent_MakeAvailable scopes them to that dependency and
# restores them afterwards.
function(fb_make_available_quiet name)
    set(saved_cxx_flags "${CMAKE_CXX_FLAGS}")
    set(saved_c_flags "${CMAKE_C_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${FB_VENDOR_WARNING_FLAGS}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${FB_VENDOR_WARNING_FLAGS}")
    FetchContent_MakeAvailable(${name})
    set(CMAKE_CXX_FLAGS "${saved_cxx_flags}")
    set(CMAKE_C_FLAGS "${saved_c_flags}")
endfunction()

# Root of the local third-party source cache: no hard-coded absolute path. When
# empty it resolves to Vendor one level above the project directory; can be
# overridden with -DFILEBROWSER_VENDOR_ROOT=<dir>.
set(FILEBROWSER_VENDOR_ROOT "" CACHE PATH
    "Root of the local third-party source cache; empty means <project dir>/../Vendor")
option(FILEBROWSER_USE_LOCAL_VENDOR
    "Prefer an existing checkout under FILEBROWSER_VENDOR_ROOT instead of downloading" ON)

if(FILEBROWSER_VENDOR_ROOT STREQUAL "")
    set(FB_VENDOR_ROOT "${CMAKE_SOURCE_DIR}/../Vendor")
else()
    set(FB_VENDOR_ROOT "${FILEBROWSER_VENDOR_ROOT}")
endif()

# Declare a dependency: if the local cache is valid, set FETCHCONTENT_SOURCE_DIR_<UPPER>;
# otherwise fetch it from URL/TAG. out_dir returns the source directory actually used.
function(fb_prepare name url tag subdir out_dir)
    string(TOUPPER "${name}" upper_name)
    set(resolved "")
    if(FILEBROWSER_USE_LOCAL_VENDOR)
        set(local_dir "${FB_VENDOR_ROOT}/${subdir}")
        if(IS_DIRECTORY "${local_dir}")
            # An uninitialized submodule is an empty directory and is not a valid cache
            file(GLOB probe LIST_DIRECTORIES false "${local_dir}/*")
            if(probe)
                set(resolved "${local_dir}")
                set(FETCHCONTENT_SOURCE_DIR_${upper_name} "${local_dir}" PARENT_SCOPE)
            endif()
        endif()
    endif()
    FetchContent_Declare(${name}
        GIT_REPOSITORY ${url}
        GIT_TAG ${tag}
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
        ${ARGN}
    )
    if(NOT resolved)
        set(resolved "${FETCHCONTENT_BASE_DIR}/${name}-src")
    endif()
    set(${out_dir} "${resolved}" PARENT_SCOPE)
endfunction()

# gRPC's provider=module dependencies are source directories inside the repository
# (not CMake packages). When something is missing, print an actionable fix instead of
# failing later at link time.
function(fb_check dep_dir what)
    if(NOT EXISTS "${dep_dir}/${what}")
        message(FATAL_ERROR
            "FetchContent dependency is missing ${what}:\n  ${dep_dir}/${what}\n"
            "  With local reuse (FILEBROWSER_USE_LOCAL_VENDOR=ON), run:\n"
            "    git -C \"${dep_dir}\" submodule update --init --depth 1\n"
            "  Or switch to an online fetch:\n"
            "    cmake --preset <preset> -DFILEBROWSER_USE_LOCAL_VENDOR=OFF")
    endif()
endfunction()

# Windows system libraries the renderer and the glfw backend need
function(fb_system_links target)
    if(WIN32)
        target_link_libraries(${target} PRIVATE
            User32 Gdi32 Shell32 Dxgi Comdlg32 d3d11 D3DCompiler OpenGL32)
    endif()
endfunction()

# ---------------------------------------------------------------- CLI11
fb_prepare(cli11 https://github.com/CLIUtils/CLI11.git v2.7.2 CLI11 cli11_dir)
set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLI11_INSTALL OFF CACHE BOOL "" FORCE)
message(STATUS "[FetchContent] cli11        <- ${cli11_dir}")
fb_make_available_quiet(cli11)

# ---------------------------------------------------------------- spdlog
fb_prepare(spdlog https://github.com/gabime/spdlog.git v1.15.1 spdlog spdlog_dir)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
# fmt Unicode support is turned off: this project formats narrow (char) strings only
add_compile_definitions(FMT_UNICODE=0)
message(STATUS "[FetchContent] spdlog       <- ${spdlog_dir}")
fb_make_available_quiet(spdlog)

# ---------------------------------------------------------------- googletest
# Only the test targets use gtest, and those are Windows-only (see the root CMakeLists), so a cross-build
# does not fetch or build it at all.
if(FILEBROWSER_BUILD_TESTS)
    fb_prepare(googletest https://github.com/google/googletest.git v1.15.2 googletest googletest_dir)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)   # dynamic CRT, as everywhere else in this project
    message(STATUS "[FetchContent] googletest   <- ${googletest_dir}")
    fb_make_available_quiet(googletest)
endif()

# ---------------------------------------------------------------- tl::expected
fb_prepare(expected https://github.com/TartanLlama/expected.git v1.1.0 expected expected_dir)
set(EXPECTED_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(EXPECTED_BUILD_PACKAGE OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
message(STATUS "[FetchContent] expected     <- ${expected_dir}")
fb_make_available_quiet(expected)

# ---------------------------------------------------------------- nlohmann_json
# Only the client uses it: the state file for a resumable transfer (client/src/TransferState.cpp) is read with this
# library rather than with a parser written here. Header only, so the target carries the include directory and
# nothing of it is compiled.
if(WIN32 AND FILEBROWSER_BUILD_CLIENT)
    fb_prepare(nlohmann_json https://github.com/nlohmann/json.git v3.11.3 nlohmann_json nlohmann_json_dir)
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    set(JSON_Install OFF CACHE BOOL "" FORCE)
    message(STATUS "[FetchContent] nlohmann_json <- ${nlohmann_json_dir}")
    fb_make_available_quiet(nlohmann_json)
endif()

# ---------------------------------------------------------------- glfw
if(FILEBROWSER_BUILD_CLIENT)
    fb_prepare(glfw https://github.com/glfw/glfw.git 3.4.0 glfw glfw_dir)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    # glfw's CMake does not propagate GLFW_EXPOSE_NATIVE_WIN32, so the imgui target defines
    # it as PUBLIC for the client (see cmake/Imgui.cmake)
    message(STATUS "[FetchContent] glfw         <- ${glfw_dir}")
    fb_make_available_quiet(glfw)
endif()

# ---------------------------------------------------------------- DiligentEngine
# Note: this only declares and fetches the sources; Diligent's own top-level
# CMakeLists is not used. DiligentCore/Graphics/ShaderTools unconditionally calls
# find_package(Python3 REQUIRED) (to run BuildTools/File2Include/script.py and generate
# HLSLDefinitions_inc.fxh). So we assemble our own targets from only the modules the
# client uses: see cmake/Diligent.cmake (file2string reproduced in pure CMake, no Python).
fb_prepare(diligent_engine
    https://github.com/DiligentGraphics/DiligentEngine.git master DiligentEngine diligent_engine_dir
    GIT_SUBMODULES "DiligentCore")
message(STATUS "[FetchContent] diligent     <- ${diligent_engine_dir}")

# ---------------------------------------------------------------- imgui (sources only; targets assembled by cmake/Imgui.cmake)
fb_prepare(imgui https://github.com/ocornut/imgui.git v1.89.2
    DiligentEngine/DiligentTools/ThirdParty/imgui imgui_dir)

if(FILEBROWSER_BUILD_CLIENT)
    FetchContent_Populate(diligent_engine)
    FetchContent_Populate(imgui)

    set(FB_DILIGENT_SOURCE_DIR "${diligent_engine_SOURCE_DIR}")
    set(FB_IMGUI_SOURCE_DIR "${imgui_SOURCE_DIR}")

    include(${CMAKE_SOURCE_DIR}/cmake/Diligent.cmake)
endif()   # FILEBROWSER_BUILD_CLIENT: Diligent / imgui are only used by the client

# ---------------------------------------------------------------- gRPC
# All providers stay "module": the dependency source directories are decided by the
# FetchContent declarations above and turned into targets by gRPC's own cmake/*.cmake
# (add_subdirectory).
fb_prepare(grpc https://github.com/grpc/grpc.git v1.70.0 grpc grpc_dir
    GIT_SUBMODULES
    "third_party/abseil-cpp third_party/protobuf third_party/re2 third_party/cares/cares third_party/zlib third_party/boringssl-with-bazel third_party/utf8_range third_party/upb third_party/address_sorting third_party/xxhash")

# gRPC's provider=module dependencies are source directories under the repository's
# third_party/ (not CMake packages). List every missing one at once with an
# actionable fix instead of failing one by one.
set(fb_grpc_missing "")
foreach(grpc_dep IN ITEMS abseil-cpp protobuf re2 cares/cares zlib boringssl-with-bazel)
    if(NOT EXISTS "${grpc_dir}/third_party/${grpc_dep}/CMakeLists.txt")
        list(APPEND fb_grpc_missing "third_party/${grpc_dep}")
    endif()
endforeach()
foreach(grpc_dep IN ITEMS utf8_range/utf8_range.c upb/upb/base/status.c address_sorting/address_sorting.c xxhash/xxhash.h)
    if(NOT EXISTS "${grpc_dir}/third_party/${grpc_dep}")
        list(APPEND fb_grpc_missing "third_party/${grpc_dep}")
    endif()
endforeach()
if(fb_grpc_missing)
    message(FATAL_ERROR
        "gRPC's third_party sources are incomplete:\n  ${fb_grpc_missing}\n"
        "  With local reuse (FILEBROWSER_USE_LOCAL_VENDOR=ON), run:\n"
        "    git -C \"${grpc_dir}\" submodule update --init --depth 1\n"
        "  Or switch to an online fetch:\n"
        "    cmake --preset <preset> -DFILEBROWSER_USE_LOCAL_VENDOR=OFF")
endif()

# BoringSSL's Windows x64 assembly sources are NASM syntax, and no real NASM is installed here:
# CMake's ASM_NASM detection falls back to yasm, and the one found on PATH is the Android NDK's
# yasm 1.3, which cannot parse the AVX10.2 files (aes-gcm-avx10-x86_64-win.asm: "instruction
# expected after label" for the {k1}{z} operand decoration, "undefined symbol zmm0"). The other
# .asm files assemble fine, so only this one is the problem; keeping the C fallbacks is enough.
# Set this to OFF to build the assembly with a NASM >= 2.14 (then also point
# CMAKE_ASM_NASM_COMPILER at that NASM so the NDK yasm is not picked up again).
option(FILEBROWSER_BORINGSSL_NO_ASM "Skip BoringSSL's NASM assembly sources (use the C fallbacks)" ON)
if(FILEBROWSER_BORINGSSL_NO_ASM)
    set(OPENSSL_NO_ASM ON)
endif()

foreach(grpc_dep IN ITEMS ABSL CPP PROTOBUF RE2 CARES ZLIB SSL)
    set(gRPC_${grpc_dep}_PROVIDER "module" CACHE STRING "gRPC dependency provider")
endforeach()
set(gRPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(gRPC_INSTALL OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_CODEGEN ON CACHE BOOL "" FORCE)          # grpc_cpp_plugin is needed to generate .grpc.pb.*
set(gRPC_BUILD_GRPC_CPP_PLUGIN ON CACHE BOOL "" FORCE)
# Only the libraries + protoc are used: turn off install / tests in every subproject.
# 1) protobuf's install(EXPORT protobuf-targets) fails because abseil is not in any
#    export set (ABSL_ENABLE_INSTALL is only ON when gRPC_INSTALL is ON), so install is disabled;
# 2) protobuf / re2 / benchmark / utf8_range build tests by default and each brings its
#    own copy of googletest, which conflicts with this project's GTest targets
#    (one copy under grpc/third_party/googletest, another under boringssl);
# 3) the runtime is the dynamic CRT everywhere; protobuf defaults to changing
#    CMAKE_MSVC_RUNTIME_LIBRARY to /MT, which clashes with this project.
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_CONFORMANCE OFF CACHE BOOL "" FORCE)
set(protobuf_MSVC_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
set(utf8_range_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(utf8_range_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(RE2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CARES_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CARES_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
# c-ares gates its install rules behind CARES_INSTALL; zlib uses the generic SKIP_INSTALL_ALL
# variable instead of its own option. re2 and tl-expected install unconditionally, so this project's
# own files are separated by install component (see the install rules in the root CMakeLists).
set(CARES_INSTALL OFF CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)

# Cross-compiling: fills the cache variables gRPC's find_program() and cmake/Proto.cmake use by
# building the two tools in a native tree first. On a native build this does nothing.
fb_host_tools("${grpc_dir}")

message(STATUS "[FetchContent] grpc         <- ${grpc_dir}")
fb_make_available_quiet(grpc)

# Used by cmake/Proto.cmake: protoc / grpc_cpp_plugin (gRPC creates no named aliases,
# these are the actual target names)
set(FB_GRPC_LIBRARY "grpc++")
set(FB_GRPC_CPP_PLUGIN_TARGET "grpc_cpp_plugin")

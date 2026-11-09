# =============================================================================
# Host codegen tools for cross-compiling
#
# protoc and grpc_cpp_plugin have to run on the machine doing the build, so the binaries gRPC
# builds for the target platform cannot be used. gRPC reacts to that by calling find_program() when
# CMAKE_CROSSCOMPILING is ON (cmake/protobuf.cmake, CMakeLists.txt), and cmake/Proto.cmake does the
# same. This builds the two tools once in a separate native tree, from the same source cache, and
# points those variables at the host executables.
#
# A native build cannot be nested inside the cross-compiled one, so this runs cmake twice at
# configure time. Nothing happens on a native build, and the work is skipped when the executables
# are already there. Only the two targets are built: Ninja pulls in just their own dependencies
# (libprotoc, utf8_range, grpc_plugin_support), not c-ares or BoringSSL.
#
# That native tree needs a native compiler: fb_host_tool_compiler() keeps it away from the cross
# toolchain's own one, and C++17 has to be passed to it explicitly.
# =============================================================================

set(FILEBROWSER_HOST_TOOLS_DIR "" CACHE PATH
    "Native build tree for protoc / grpc_cpp_plugin; empty means <build dir>/host-tools")
set(FILEBROWSER_HOST_TOOLS_GENERATOR "Ninja" CACHE STRING
    "Generator for that native tree (single-config, so the executables sit directly in it)")
set(FILEBROWSER_HOST_TOOLS_COMPILER "" CACHE FILEPATH
    "Host compiler for that native tree; empty means CMake's own detection")

# Resolves a host executable. The extension belongs to the host, not to the platform being
# cross-compiled, so CMAKE_EXECUTABLE_SUFFIX cannot be used here, and the file is not at the root
# of the tree either: protobuf puts protoc under third_party/protobuf/, and a multi-config generator
# puts it under <config>/. A recursive glob covers both.
function(fb_host_tool_exe dir name out_dir)
    file(GLOB_RECURSE fb_host_tool_matches LIST_DIRECTORIES false "${dir}/${name}.exe" "${dir}/${name}")
    if(fb_host_tool_matches)
        set(${out_dir} "${fb_host_tool_matches}" PARENT_SCOPE)
    else()
        set(${out_dir} "" PARENT_SCOPE)
    endif()
endfunction()

# Picks the compiler for the native tree. CMake's own detection searches PATH, and a cross toolchain
# compiler is usually on PATH too - that one cannot build host binaries, so the same compiler name is
# looked for in the PATH entries outside the toolchain directory. Empty result means CMake detects it
# itself. FILEBROWSER_HOST_TOOLS_COMPILER overrides everything.
function(fb_host_tool_compiler lang out_path)
    if(NOT FILEBROWSER_HOST_TOOLS_COMPILER STREQUAL "")
        set(${out_path} "${FILEBROWSER_HOST_TOOLS_COMPILER}" PARENT_SCOPE)
        return()
    endif()

    if(NOT CMAKE_${lang}_COMPILER)
        set(${out_path} "" PARENT_SCOPE)
        return()
    endif()

    get_filename_component(toolchain_dir "${CMAKE_${lang}_COMPILER}" DIRECTORY)
    get_filename_component(toolchain_name "${CMAKE_${lang}_COMPILER}" NAME)

    # PATH separates with ';' on a Windows host and with ':' on a Unix host. CMAKE_HOST_WIN32 is the
    # right test here: WIN32 describes the target, which is Android in a cross build.
    if(CMAKE_HOST_WIN32)
        set(fb_path_entries $ENV{PATH})
    else()
        string(REPLACE ":" ";" fb_path_entries "$ENV{PATH}")
    endif()

    foreach(entry IN LISTS fb_path_entries)
        string(REPLACE "\"" "" entry "${entry}")
        # Skip the toolchain's own bin directory, and any other NDK toolchain bin directory: a clang
        # from there is the cross compiler even when a different NDK version is the one in use.
        if(entry STREQUAL "" OR entry STREQUAL toolchain_dir OR entry MATCHES "toolchains[/\\\\]llvm")
            continue()
        endif()
        foreach(candidate IN ITEMS "${entry}/${toolchain_name}.exe" "${entry}/${toolchain_name}")
            if(EXISTS "${candidate}")
                set(${out_path} "${candidate}" PARENT_SCOPE)
                return()
            endif()
        endforeach()
    endforeach()

    set(${out_path} "" PARENT_SCOPE)
endfunction()

# grpc_source_dir is the source directory gRPC was prepared from, so the host tree uses the same
# checkout as the main build instead of fetching it a second time.
function(fb_host_tools grpc_source_dir)
    if(NOT CMAKE_CROSSCOMPILING)
        return()
    endif()

    set(host_dir "${FILEBROWSER_HOST_TOOLS_DIR}")
    if(host_dir STREQUAL "")
        set(host_dir "${CMAKE_BINARY_DIR}/host-tools")
    endif()

    fb_host_tool_exe("${host_dir}" protoc host_protoc)
    fb_host_tool_exe("${host_dir}" grpc_cpp_plugin host_plugin)

    if(NOT host_protoc OR NOT host_plugin)
        message(STATUS "[host tools] building protoc / grpc_cpp_plugin for the host in ${host_dir}")
        # The same "no tests, no install" settings as the main build, so the native tree does not
        # pull in the subprojects' test binaries or write install rules.
        # CMAKE_CXX_STANDARD has to be passed explicitly: a sub-build starts from CMake's default,
        # and abseil then keeps its own string_view instead of std::string_view. protobuf's
        # io::Printer::PrintImpl is mangled with absl::string_view, so the plugin - built as C++17
        # like the rest of this project - links against a different name and fails.
        # The compiler comes from fb_host_tool_compiler(), which avoids the cross toolchain's own one.
        set(fb_policy_floor "3.5")
        if(NOT CMAKE_POLICY_VERSION_MINIMUM STREQUAL "")
            set(fb_policy_floor "${CMAKE_POLICY_VERSION_MINIMUM}")
        endif()
        set(host_args
            -S "${grpc_source_dir}" -B "${host_dir}"
            -G "${FILEBROWSER_HOST_TOOLS_GENERATOR}"
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_C_STANDARD=11
            -DCMAKE_CXX_STANDARD=17
            -DCMAKE_POLICY_VERSION_MINIMUM=${fb_policy_floor}
            -DCMAKE_EXPORT_COMPILE_COMMANDS=OFF
            -DgRPC_BUILD_TESTS=OFF -DgRPC_INSTALL=OFF
            -DgRPC_BUILD_CODEGEN=ON -DgRPC_BUILD_GRPC_CPP_PLUGIN=ON
            -Dprotobuf_INSTALL=OFF -Dprotobuf_BUILD_TESTS=OFF
            -Dutf8_range_ENABLE_INSTALL=OFF -Dutf8_range_ENABLE_TESTS=OFF
            -DABSL_ENABLE_INSTALL=OFF -DABSL_BUILD_TESTING=OFF
            -DRE2_BUILD_TESTING=OFF
            -DCARES_BUILD_TESTS=OFF -DCARES_BUILD_TOOLS=OFF
            -DSKIP_INSTALL_ALL=ON
        )
        fb_host_tool_compiler(C host_c)
        fb_host_tool_compiler(CXX host_cxx)
        if(host_c OR host_cxx)
            # No quotes around the value: an argument that starts with -D keeps any quote that is not
            # at its very beginning, so "-DVAR=path with spaces" would reach the sub-build with literal
            # quotes. Each list element is one argument, so the spaces survive without them.
            if(host_c)
                list(APPEND host_args -DCMAKE_C_COMPILER=${host_c})
            endif()
            if(host_cxx)
                list(APPEND host_args -DCMAKE_CXX_COMPILER=${host_cxx})
            endif()
            message(STATUS "[host tools] compiler: ${host_cxx}")
        endif()
        execute_process(
            COMMAND ${CMAKE_COMMAND} ${host_args}
            RESULT_VARIABLE host_configure_rc
        )
        if(NOT host_configure_rc EQUAL 0)
            message(FATAL_ERROR
                "Host tools tree failed to configure: ${host_dir}\n"
                "  This native tree picks its own compiler, and a compiler from the cross toolchain can be found first.\n"
                "  Delete ${host_dir} and re-run with -DFILEBROWSER_HOST_TOOLS_COMPILER=<a native compiler>.")
        endif()

        set(host_config_args)
        if(FILEBROWSER_HOST_TOOLS_GENERATOR MATCHES "Visual Studio")
            # A multi-config generator puts the executables under <generator>/<config>/ and needs
            # --config on the build command.
            list(APPEND host_config_args --config Release)
        endif()
        execute_process(
            COMMAND ${CMAKE_COMMAND} --build "${host_dir}" --target protoc grpc_cpp_plugin ${host_config_args}
            RESULT_VARIABLE host_build_rc
        )
        if(NOT host_build_rc EQUAL 0)
            message(FATAL_ERROR "Host tools build failed: ${host_dir}")
        endif()
        fb_host_tool_exe("${host_dir}" protoc host_protoc)
        fb_host_tool_exe("${host_dir}" grpc_cpp_plugin host_plugin)
        if(NOT host_protoc OR NOT host_plugin)
            message(FATAL_ERROR "Host tools build produced neither protoc nor grpc_cpp_plugin in ${host_dir}")
        endif()
    endif()

    # gRPC's find_program() names and the names cmake/Proto.cmake uses, both filled from the
    # host tree. find_program returns a cache entry that is already a full path, so setting these
    # before add_subdirectory(grpc) is what keeps the cross build from looking for a system protoc.
    set(_gRPC_PROTOBUF_PROTOC_EXECUTABLE "${host_protoc}" CACHE FILEPATH "Host protoc" FORCE)
    set(_gRPC_CPP_PLUGIN "${host_plugin}" CACHE FILEPATH "Host grpc_cpp_plugin" FORCE)
    set(FB_HOST_PROTOC "${host_protoc}" CACHE FILEPATH "Host protoc" FORCE)
    set(FB_HOST_GRPC_CPP_PLUGIN "${host_plugin}" CACHE FILEPATH "Host grpc_cpp_plugin" FORCE)
    message(STATUS "[host tools] protoc: ${host_protoc}")
    message(STATUS "[host tools] grpc_cpp_plugin: ${host_plugin}")
endfunction()

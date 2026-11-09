# =============================================================================
# protos/FileBrowser.proto -> FileBrowser.pb.* / FileBrowser.grpc.pb.*
#
# Prefer the protoc / grpc_cpp_plugin targets produced by the FetchContent'd gRPC.
# When cross-compiling (Android) we cannot run target-platform executables, so the
# fallback order is:
#   1) the cache variables cmake/HostTools.cmake filled from a native host tree
#   2) protoc / grpc_cpp_plugin already installed on the host (find_program)
#   3) files already present in build/generated (reused if any)
# =============================================================================

set(FB_PROTO_DIR "${CMAKE_SOURCE_DIR}/protos")
set(FB_PROTO_FILE "${FB_PROTO_DIR}/FileBrowser.proto")
set(FB_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")

set(FB_PROTO_OUTPUTS
    "${FB_GENERATED_DIR}/FileBrowser.pb.cc"
    "${FB_GENERATED_DIR}/FileBrowser.pb.h"
    "${FB_GENERATED_DIR}/FileBrowser.grpc.pb.cc"
    "${FB_GENERATED_DIR}/FileBrowser.grpc.pb.h")

if(TARGET protoc AND TARGET grpc_cpp_plugin AND NOT CMAKE_CROSSCOMPILING)
    file(MAKE_DIRECTORY "${FB_GENERATED_DIR}")
    add_custom_command(
        OUTPUT ${FB_PROTO_OUTPUTS}
        COMMAND $<TARGET_FILE:protoc>
            --cpp_out=${FB_GENERATED_DIR}
            --plugin=protoc-gen-grpc=$<TARGET_FILE:grpc_cpp_plugin>
            --grpc_out=${FB_GENERATED_DIR}
            -I "${FB_PROTO_DIR}"
            "${FB_PROTO_FILE}"
        DEPENDS "${FB_PROTO_FILE}" grpc_cpp_plugin
        COMMENT "protoc: FileBrowser.proto -> ${FB_GENERATED_DIR}"
        VERBATIM
    )
    set(FB_PROTO_SOURCES "${FB_GENERATED_DIR}/FileBrowser.pb.cc"
                         "${FB_GENERATED_DIR}/FileBrowser.grpc.pb.cc")
else()
    find_program(FB_HOST_PROTOC protoc)
    find_program(FB_HOST_GRPC_CPP_PLUGIN grpc_cpp_plugin)
    if(FB_HOST_PROTOC AND FB_HOST_GRPC_CPP_PLUGIN)
        file(MAKE_DIRECTORY "${FB_GENERATED_DIR}")
        # No quotes here: Ninja runs this through cmd.exe when an argument needs escaping, and inside
        # that wrapper an escaped quote reaches protoc as a literal quote, so the plugin path is then
        # read as "\"D:/...\"" and the plugin fails to load. gRPC's own codegen commands do the same thing.
        add_custom_command(
            OUTPUT ${FB_PROTO_OUTPUTS}
            COMMAND "${FB_HOST_PROTOC}"
                --cpp_out=${FB_GENERATED_DIR}
                --plugin=protoc-gen-grpc=${FB_HOST_GRPC_CPP_PLUGIN}
                --grpc_out=${FB_GENERATED_DIR}
                -I "${FB_PROTO_DIR}"
                "${FB_PROTO_FILE}"
            DEPENDS "${FB_PROTO_FILE}"
            COMMENT "protoc (host): FileBrowser.proto -> ${FB_GENERATED_DIR}"
            VERBATIM
        )
        set(FB_PROTO_SOURCES "${FB_GENERATED_DIR}/FileBrowser.pb.cc"
                             "${FB_GENERATED_DIR}/FileBrowser.grpc.pb.cc")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/build/generated/FileBrowser.pb.cc")
        # Cross-compiling with no host protoc: reuse the files already present in the build tree
        set(FB_GENERATED_DIR "${CMAKE_SOURCE_DIR}/build/generated")
        set(FB_PROTO_SOURCES "${FB_GENERATED_DIR}/FileBrowser.pb.cc"
                             "${FB_GENERATED_DIR}/FileBrowser.grpc.pb.cc")
        set_source_files_properties(${FB_PROTO_SOURCES} PROPERTIES GENERATED TRUE)
        message(WARNING "protoc not found, reusing already generated files: ${FB_GENERATED_DIR}")
    else()
        message(FATAL_ERROR
            "Cannot generate protobuf/gRPC code: neither protoc nor grpc_cpp_plugin is available.\n"
            "  Install protoc and grpc_cpp_plugin on the host first, or generate build/generated beforehand.")
    endif()
endif()

add_library(filebrowser_proto STATIC ${FB_PROTO_SOURCES})
target_include_directories(filebrowser_proto PUBLIC "${FB_GENERATED_DIR}")
# gRPC creates no named aliases: grpc++ / gpr / address_sorting / utf8_range_lib are the real target names
if(TARGET protobuf::libprotobuf)
    target_link_libraries(filebrowser_proto PUBLIC protobuf::libprotobuf)
elseif(TARGET libprotobuf)
    target_link_libraries(filebrowser_proto PUBLIC libprotobuf)
endif()
target_link_libraries(filebrowser_proto PUBLIC grpc++)
if(TARGET absl::str_format)
    target_link_libraries(filebrowser_proto PUBLIC absl::str_format)
endif()

# group by on-disk directory structure in VS (cmake/SourceGroups.cmake):
# generated files live in the build tree, so they get their own group
fb_source_group(filebrowser_proto ROOT "${FB_GENERATED_DIR}" PREFIX "generated")

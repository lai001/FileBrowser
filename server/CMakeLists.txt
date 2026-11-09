# Server.cpp is built as a library so the smoke test (server/test) can link the same
# implementation without compiling it twice.
add_library(fb_server STATIC
    src/Server.cpp
    src/Server.h
    src/DirectoryScan.cpp
    src/DirectoryScan.h
    src/Shutdown.cpp
    src/Shutdown.h
)
target_include_directories(fb_server PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# Shutdown.cpp runs the pipe reader on a background thread
find_package(Threads REQUIRED)
target_link_libraries(fb_server PUBLIC
    fb_misc
    spdlog::spdlog
    filebrowser_proto
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

# group by on-disk directory structure in VS (cmake/SourceGroups.cmake): src
fb_source_group(fb_server)

add_executable(FileBrowserServer
    src/main.cpp
)
# main.cpp parses the command line with CLI11, the same library CollectFiles uses
target_link_libraries(FileBrowserServer PRIVATE
    fb_server
    CLI11::CLI11
)
# main.cpp calls InitProtoReflectionServerBuilderPlugin() on every platform, so the reflection
# library is linked wherever it exists; it is only missing when gRPC's codegen is turned off.
if(TARGET grpc++_reflection)
    target_link_libraries(FileBrowserServer PRIVATE grpc++_reflection)
endif()

fb_system_links(FileBrowserServer)

# group by on-disk directory structure in VS (cmake/SourceGroups.cmake): src
fb_source_group(FileBrowserServer)

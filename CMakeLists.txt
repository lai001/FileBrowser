cmake_minimum_required(VERSION 3.21)
project(FileBrowser LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Dynamic CRT (MD / MDd): the MSVC default, pinned here because protobuf would otherwise
# switch the runtime to /MT (see cmake/ThirdParty.cmake)
if(MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# Ninja already starts one compiler process per source file, but MSBuild runs a single
# cl.exe per project, so ask for multi-process compilation on the VS generator only
# (otherwise Ninja would spawn cores x cores processes).
if(MSVC AND CMAKE_GENERATOR MATCHES "Visual Studio")
    add_compile_options(/MP)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(SourceGroups)
include(GNUInstallDirs)

option(FILEBROWSER_BUILD_CLIENT "Build the GLFW + Diligent client (Windows only)" ON)
option(FILEBROWSER_BUILD_TESTS "Build the gtest tests (Windows only)" ON)
option(FILEBROWSER_CRT_CHECK_ALWAYS_HEAP
    "Validate every debug CRT heap block on each allocation/free (MSVC debug CRT only; every allocation scans the whole heap, so it is slow)"
    OFF)

include(ThirdParty)
include(Proto)
if(WIN32 AND FILEBROWSER_BUILD_CLIENT)
    include(Imgui)
endif()

enable_testing()

# The client and the tests are Windows only; an Android cross-build contains just the
# server and CollectFiles.
add_subdirectory(misc)
add_subdirectory(server)
add_subdirectory(collect_files)
if(WIN32 AND FILEBROWSER_BUILD_TESTS)
    add_subdirectory(server/test)
endif()

if(WIN32 AND FILEBROWSER_BUILD_CLIENT)
    add_subdirectory(client)
    if(FILEBROWSER_BUILD_TESTS)
        add_subdirectory(client/test)
    endif()
endif()

# Install rules for this project's own outputs, put in a component named after the project.
# Note: the FetchContent subprojects write their own install rules and those are not component
# guarded (c-ares / re2 / tl-expected have no option that turns them off, see cmake/ThirdParty.cmake),
# so cmake --install also copies their headers, libraries and CMake config files; build everything
# first, otherwise those rules fail on files that were never built.
set(FB_INSTALL_COMPONENT "FileBrowser")

set(fb_install_targets "")
foreach(fb_target IN ITEMS FileBrowserServer CollectFiles FileBrowserClient)
    if(TARGET ${fb_target})
        list(APPEND fb_install_targets ${fb_target})
    endif()
endforeach()

if(fb_install_targets)
    install(TARGETS ${fb_install_targets}
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        COMPONENT "${FB_INSTALL_COMPONENT}"
    )
    message(STATUS "install component ${FB_INSTALL_COMPONENT}: ${fb_install_targets} -> ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}")
endif()

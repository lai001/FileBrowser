# =============================================================================
# Source grouping in IDEs (Visual Studio): show files following the real
# on-disk directory structure instead of CMake's default "Header Files" /
# "Source Files" (which groups by file extension).
#
# Implementation: source_group(TREE <root> FILES <files>) (CMake >= 3.8), which
# derives the groups from each file's path relative to <root> (written to
# *.vcxproj.filters).
#
# Two things to keep in mind:
#   1) source_group rules are directory-scoped: they only affect targets created
#      in the same directory. So fb_source_group() must be called from the
#      CMakeLists.txt where the target is defined (calling it from a function is
#      fine - a function inherits the caller's directory scope). Targets created
#      inside third-party directories (pulled in by FetchContent /
#      add_subdirectory) are outside our scope and cannot be regrouped from here.
#   2) TREE requires every file to be under <root> (paths must start with <root>),
#      otherwise the command fails, so files outside <root> (generated files in
#      the build tree, vendor sources) get their own group named after their
#      directory.
#
# Only IDE generators consume source groups; Ninja ignores them, so calling this
# has no side effects there. (The SOURCE_GROUP source-file property was added in
# CMake 4.5; this file does not set it, and setting it would do nothing on CMake 4.3.)
# =============================================================================

# fb_source_group(<target> [ROOT <dir>] [PREFIX <group>])
#   ROOT   defaults to the directory of the CMakeLists.txt that calls it
#   PREFIX optional top-level group name for files located directly in ROOT
function(fb_source_group target)
    cmake_parse_arguments(FBSG "" "ROOT;PREFIX" "" ${ARGN})

    get_target_property(sources ${target} SOURCES)
    if(NOT sources)
        return()
    endif()

    set(root "${CMAKE_CURRENT_SOURCE_DIR}")
    if(FBSG_ROOT)
        set(root "${FBSG_ROOT}")
    endif()

    set(in_root "")
    set(outside "")
    foreach(f ${sources})
        if(NOT IS_ABSOLUTE "${f}")
            set(f "${CMAKE_CURRENT_SOURCE_DIR}/${f}")
        endif()
        file(RELATIVE_PATH rel "${root}" "${f}")
        if(rel MATCHES "^\\.\\.")
            list(APPEND outside "${f}")
        else()
            list(APPEND in_root "${f}")
        endif()
    endforeach()

    if(in_root)
        if(FBSG_PREFIX)
            source_group(TREE "${root}" PREFIX "${FBSG_PREFIX}" FILES ${in_root})
        else()
            source_group(TREE "${root}" FILES ${in_root})
        endif()
    endif()
    foreach(f ${outside})
        get_filename_component(group "${f}" DIRECTORY)
        get_filename_component(group "${group}" NAME)
        if(group STREQUAL "")
            set(group "generated")
        endif()
        source_group("${group}" FILES ${f})
    endforeach()
endfunction()

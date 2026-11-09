# =============================================================================
# file2string - pure-CMake equivalent of BuildTools/File2Include/script.py
#
# Diligent uses it to turn HLSLDefinitions.fxh into a C++ string constant
# (HLSLDefinitions_inc.fxh). Among the modules this project assembles this is the only
# place that needs Python; Diligent's own CMakeLists also calls find_package(Python3
# REQUIRED) in HLSL2GLSLConverterLib, RenderStateNotation and DiligentFX/Hydrogent, which
# are not built here. Reproducing the conversion in CMake means no Python
# interpreter is required after switching to CMake.
#
# Usage:
#   include(cmake/File2String.cmake)
#   fb_file2string(<src> <dst>)
# It can also be run directly as a script (the cmake executable is enough):
#   cmake -DFB_F2S_SRC=<src> -DFB_F2S_DST=<dst> -P cmake/File2String.cmake
# CMake 4.x does not populate ARGV0/ARGV1 for "cmake -P script <args>", so the -D variables are the
# reliable way to pass the paths; ARGV is still accepted for older CMake versions.
#
# script.py conversion rules (per line): escape ' " \ , wrap the whole line as
# "...\n", strip trailing whitespace (rstrip).
# =============================================================================

function(fb_file2string src_file dst_file)
    if(NOT EXISTS "${src_file}")
        message(FATAL_ERROR "file2string: source file does not exist: ${src_file}")
    endif()

    file(READ "${src_file}" _content)

    # Escape special characters first, then wrap each line as a string constant
    string(REPLACE "\\" "\\\\" _content "${_content}")
    string(REPLACE "\"" "\\\"" _content "${_content}")
    string(REPLACE "'" "\\'" _content "${_content}")
    string(REPLACE "\r\n" "\n" _content "${_content}")

    # Per line: strip trailing whitespace (python's rstrip), wrap as "...\n"
    set(_out "")
    while(TRUE)
        string(FIND "${_content}" "\n" _nl)
        if(_nl EQUAL -1)
            if(NOT _content STREQUAL "")
                string(REGEX REPLACE "[ \t]+$" "" _line "${_content}")
                string(APPEND _out "\"${_line}\\n\"\n")
            endif()
            break()
        endif()
        string(SUBSTRING "${_content}" 0 ${_nl} _line)
        string(REGEX REPLACE "[ \t]+$" "" _line "${_line}")
        string(APPEND _out "\"${_line}\\n\"\n")
        math(EXPR _next "${_nl} + 1")
        string(SUBSTRING "${_content}" ${_next} -1 _content)
    endwhile()

    get_filename_component(_dst_dir "${dst_file}" DIRECTORY)
    if(_dst_dir)
        file(MAKE_DIRECTORY "${_dst_dir}")
    endif()
    file(WRITE "${dst_file}" "${_out}")
    message(STATUS "[file2string] ${src_file} -> ${dst_file}")
endfunction()

# Run as a script: cmake -DFB_F2S_SRC=<src> -DFB_F2S_DST=<dst> -P File2String.cmake
if(CMAKE_SCRIPT_MODE_FILE)
    if(ARGV_COUNT GREATER 1)
        fb_file2string("${ARGV0}" "${ARGV1}")
    elseif(FB_F2S_SRC AND FB_F2S_DST)
        fb_file2string("${FB_F2S_SRC}" "${FB_F2S_DST}")
    else()
        message(FATAL_ERROR "Usage: cmake -DFB_F2S_SRC=<src> -DFB_F2S_DST=<dst> -P File2String.cmake")
    endif()
endif()

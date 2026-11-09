# =============================================================================
# DiligentEngine: only the modules the client uses
#
# Why we cannot simply add_subdirectory(DiligentEngine):
#   DiligentCore/Graphics/CMakeLists.txt unconditionally adds ShaderTools, and
#   ShaderTools/CMakeLists.txt unconditionally calls find_package(Python3 REQUIRED)
#   to run BuildTools/File2Include/script.py and generate HLSLDefinitions_inc.fxh.
#
# So this file assembles its own targets from the modules the client actually uses,
# and the codegen step is reproduced in pure CMake (see cmake/File2String.cmake).
#
# Modules used:
#   Primitives / PlatformInterface (+Basic +Win32) / Common / GraphicsAccessories /
#   GraphicsEngine / GraphicsEngineD3DBase / GraphicsEngineD3D11 / ShaderTools
#   (GraphicsTools is not needed: the Imgui bridge only uses GraphicsAccessories,
#    imgui rendering does not reference it)
# =============================================================================

set(FB_DILIGENT_CORE_DIR "${FB_DILIGENT_SOURCE_DIR}/DiligentCore")

# Only the D3D11 backend is enabled (the RENDER_DEVICE_TYPE_D3D11 branches in
# client/src/Renderer.cpp and RendererInitializer.cpp; every other backend is
# guarded by #if *_SUPPORTED)
set(PLATFORM_WIN32             TRUE  CACHE INTERNAL "")
set(ARCH                       64    CACHE INTERNAL "")
set(WINDOWS_SDK_VERSION        "10.0")
set(D3D11_SUPPORTED            TRUE  CACHE INTERNAL "")
set(D3D12_SUPPORTED            FALSE CACHE INTERNAL "")
set(GL_SUPPORTED               FALSE CACHE INTERNAL "")
set(GLES_SUPPORTED             FALSE CACHE INTERNAL "")
set(VULKAN_SUPPORTED           FALSE CACHE INTERNAL "")
set(METAL_SUPPORTED            FALSE CACHE INTERNAL "")
set(WEBGPU_SUPPORTED           FALSE CACHE INTERNAL "")
set(ARCHIVER_SUPPORTED         FALSE CACHE INTERNAL "")
set(RENDER_STATE_CACHE_SUPPORTED FALSE CACHE INTERNAL "")
set(DILIGENT_USE_SPIRV_TOOLCHAIN OFF CACHE INTERNAL "")
set(DILIGENT_NO_HLSL           OFF  CACHE INTERNAL "")
set(DILIGENT_INSTALL_CORE      OFF  CACHE INTERNAL "")
set(DILIGENT_INSTALL_TOOLS     OFF  CACHE INTERNAL "")
set(DILIGENT_NO_FORMAT_VALIDATION ON CACHE INTERNAL "")

set(DEBUG_CONFIGURATIONS   Debug)
set(RELEASE_CONFIGURATIONS Release RelWithDebInfo MinSizeRel)

# Diligent's module CMakeLists rely on these definitions/helper functions
# (originally provided by the DiligentCore top-level CMakeLists)
include("${FB_DILIGENT_CORE_DIR}/BuildTools/CMake/BuildUtils.cmake")

add_library(Diligent-PublicBuildSettings INTERFACE)
target_compile_definitions(Diligent-PublicBuildSettings INTERFACE
    PLATFORM_WIN32=1
    D3D11_SUPPORTED=$<BOOL:${D3D11_SUPPORTED}>
    D3D12_SUPPORTED=$<BOOL:${D3D12_SUPPORTED}>
    GL_SUPPORTED=$<BOOL:${GL_SUPPORTED}>
    GLES_SUPPORTED=$<BOOL:${GLES_SUPPORTED}>
    VULKAN_SUPPORTED=$<BOOL:${VULKAN_SUPPORTED}>
    METAL_SUPPORTED=$<BOOL:${METAL_SUPPORTED}>
    WEBGPU_SUPPORTED=$<BOOL:${WEBGPU_SUPPORTED}>
    "$<$<CONFIG:Debug>:DILIGENT_DEVELOPMENT;DILIGENT_DEBUG>"
)

add_library(Diligent-BuildSettings INTERFACE)
target_compile_definitions(Diligent-BuildSettings INTERFACE
    "$<$<CONFIG:Debug>:_DEBUG;DEBUG>"
    "$<$<CONFIG:Release>:NDEBUG>"
    "$<$<CONFIG:RelWithDebInfo>:NDEBUG>"
    "$<$<CONFIG:MinSizeRel>:NDEBUG>"
)
target_link_libraries(Diligent-BuildSettings INTERFACE Diligent-PublicBuildSettings)

# Originally created by DiligentCore/Graphics/CMakeLists.txt (we do not add the
# whole Graphics directory). Order matters: Primitives must be built first, then
# attached to GraphicsEngineInterface.
add_library(Diligent-GraphicsEngineInterface INTERFACE)
target_include_directories(Diligent-GraphicsEngineInterface INTERFACE
    "${FB_DILIGENT_CORE_DIR}/Graphics/GraphicsEngine/interface"
)

function(fb_diligent_module subdir)
    # EXCLUDE_FROM_ALL: these libraries are built only as link dependencies of the
    # client, not as part of "build everything" (GraphicsEngineD3D11's CMakeLists
    # also creates a -shared target, which we do not need)
    add_subdirectory("${FB_DILIGENT_CORE_DIR}/${subdir}"
        "${CMAKE_BINARY_DIR}/diligent_engine/${subdir}" EXCLUDE_FROM_ALL)
endfunction()

fb_check("${FB_DILIGENT_SOURCE_DIR}" "DiligentCore/Primitives/interface/BasicTypes.h")
fb_check("${FB_DILIGENT_SOURCE_DIR}" "DiligentCore/ThirdParty/GPUOpenShaderUtils/DXBCChecksum.h")
fb_check("${FB_DILIGENT_SOURCE_DIR}" "DiligentCore/ThirdParty/DirectXShaderCompiler/dxc/dxcapi.h")

fb_diligent_module(Primitives)
target_link_libraries(Diligent-GraphicsEngineInterface INTERFACE
    Diligent-Primitives
    Diligent-PublicBuildSettings
)

fb_diligent_module(Platforms)     # Diligent-PlatformInterface + Diligent-BasicPlatform + Diligent-TargetPlatform
fb_diligent_module(Common)
fb_diligent_module(Graphics/GraphicsEngine)
fb_diligent_module(Graphics/GraphicsAccessories)

# -----------------------------------------------------------------------------
# Diligent-ShaderTools: hand-written equivalent of what ShaderTools/CMakeLists.txt
# produces on Win32 + D3D11 (GL/Vulkan/Metal disabled), with the Python file2string
# replaced by pure CMake. GraphicsEngineD3DBase needs it (ShaderD3DBase.cpp ->
# HLSLUtils.hpp).
# -----------------------------------------------------------------------------
set(FB_SHADER_TOOLS_DIR "${FB_DILIGENT_CORE_DIR}/Graphics/ShaderTools")

include("${CMAKE_SOURCE_DIR}/cmake/File2String.cmake")
set(FB_HLSL_DEFINITIONS_INC "${CMAKE_BINARY_DIR}/diligent_engine/GeneratedShaders/HLSLDefinitions_inc.fxh")
add_custom_command(OUTPUT "${FB_HLSL_DEFINITIONS_INC}"
    # CMake 4.x does not fill ARGV0/ARGV1 for "cmake -P script <args>", so the paths are passed as
    # -D definitions (see cmake/File2String.cmake).
    COMMAND ${CMAKE_COMMAND}
            -DFB_F2S_SRC="${FB_SHADER_TOOLS_DIR}/include/HLSLDefinitions.fxh"
            -DFB_F2S_DST="${FB_HLSL_DEFINITIONS_INC}"
            -P "${CMAKE_SOURCE_DIR}/cmake/File2String.cmake"
    MAIN_DEPENDENCY "${FB_SHADER_TOOLS_DIR}/include/HLSLDefinitions.fxh"
    COMMENT "Processing HLSLDefinitions.fxh (pure CMake, no Python)"
)

set_source_files_properties("${FB_HLSL_DEFINITIONS_INC}" PROPERTIES GENERATED TRUE)

add_library(Diligent-ShaderTools STATIC
    "${FB_SHADER_TOOLS_DIR}/src/ShaderToolsCommon.cpp"
    "${FB_SHADER_TOOLS_DIR}/src/GLSLParsingTools.cpp"
    "${FB_SHADER_TOOLS_DIR}/src/HLSLParsingTools.cpp"
    "${FB_SHADER_TOOLS_DIR}/src/HLSLTokenizer.cpp"
    "${FB_SHADER_TOOLS_DIR}/src/HLSLUtils.cpp"            # ENABLE_HLSL (D3D11)
    "${FB_SHADER_TOOLS_DIR}/src/DXBCUtils.cpp"
    "${FB_SHADER_TOOLS_DIR}/src/DXCompiler.cpp"
    "${FB_SHADER_TOOLS_DIR}/src/DXCompilerLibrary.cpp"
    "${FB_SHADER_TOOLS_DIR}/src/DXCompilerLibraryWin32.cpp"
    "${FB_SHADER_TOOLS_DIR}/include/ShaderToolsCommon.hpp"
    "${FB_SHADER_TOOLS_DIR}/include/GLSLParsingTools.hpp"
    "${FB_SHADER_TOOLS_DIR}/include/HLSLParsingTools.hpp"
    "${FB_SHADER_TOOLS_DIR}/include/HLSLTokenizer.hpp"
    "${FB_SHADER_TOOLS_DIR}/include/HLSLUtils.hpp"
    "${FB_SHADER_TOOLS_DIR}/include/HLSLKeywords.h"
    "${FB_SHADER_TOOLS_DIR}/include/DXBCUtils.hpp"
    "${FB_SHADER_TOOLS_DIR}/include/ResourceBindingMap.hpp"
    "${FB_SHADER_TOOLS_DIR}/include/DXCompiler.hpp"
    "${FB_SHADER_TOOLS_DIR}/include/DXCompilerLibrary.hpp"
    "${FB_SHADER_TOOLS_DIR}/include/HLSLDefinitions.fxh"
    "${FB_HLSL_DEFINITIONS_INC}"
    "${FB_DILIGENT_CORE_DIR}/ThirdParty/GPUOpenShaderUtils/DXBCChecksum.cpp"
    "${FB_DILIGENT_CORE_DIR}/ThirdParty/GPUOpenShaderUtils/DXBCChecksum.h"
)
# HLSLUtils.cpp does #include "HLSLDefinitions_inc.fxh", so the generated directory
# must be an include directory; ShaderTools' interface include dir must be PUBLIC
# (GraphicsEngineD3DBase reaches it through ShaderToolsCommon.hpp)
# The DirectXShaderCompiler include dir must be PUBLIC as well: ShaderD3DBase.cpp includes
# "dxc/dxcapi.h" (upstream adds it PUBLIC because DXC_SUPPORTED is TRUE on Win32/MSVC, see
# ShaderTools/CMakeLists.txt); the header is the vendored copy in ThirdParty, no Windows SDK
# DXC component is needed to compile.
target_include_directories(Diligent-ShaderTools
    PUBLIC
        "${FB_SHADER_TOOLS_DIR}/include"
        "${FB_DILIGENT_CORE_DIR}/ThirdParty/DirectXShaderCompiler"
    PRIVATE
        "${FB_DILIGENT_CORE_DIR}/Graphics/GraphicsEngine/include"
        "${CMAKE_BINARY_DIR}/diligent_engine/GeneratedShaders"
)
target_link_libraries(Diligent-ShaderTools
    PRIVATE
        Diligent-BuildSettings
        Diligent-GraphicsAccessories
        Diligent-Common
    PUBLIC
        Diligent-GraphicsEngineInterface
)
# No Diligent-HLSL2GLSLConverterLib (GL/Vulkan disabled), same behaviour as Diligent's CMakeLists
target_compile_definitions(Diligent-ShaderTools PRIVATE DILIGENT_NO_HLSL=1)
set_common_target_properties(Diligent-ShaderTools 17)

# group by on-disk directory structure in VS (cmake/SourceGroups.cmake):
# ShaderTools/src, ShaderTools/include
fb_source_group(Diligent-ShaderTools ROOT "${FB_SHADER_TOOLS_DIR}" PREFIX "ShaderTools")

fb_diligent_module(Graphics/GraphicsEngineD3DBase)
fb_diligent_module(Graphics/GraphicsEngineD3D11)

# System libraries for the D3D11 backend (d3d11 / dxgi / d3dcompiler) are handled by fb_system_links()
message(STATUS "[Diligent] building only the modules the client needs (no Python): ${FB_DILIGENT_SOURCE_DIR}")

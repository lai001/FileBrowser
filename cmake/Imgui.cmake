# =============================================================================
# imgui targets. DiligentTools/CMakeLists.txt would pull in the whole tool set
# (NativeApp, AssetLoader, ...), so here we build just the two static libraries.
# =============================================================================

add_library(fb_imgui STATIC
    "${FB_IMGUI_SOURCE_DIR}/imgui.cpp"
    "${FB_IMGUI_SOURCE_DIR}/imgui_draw.cpp"
    "${FB_IMGUI_SOURCE_DIR}/imgui_tables.cpp"
    "${FB_IMGUI_SOURCE_DIR}/imgui_widgets.cpp"
    "${FB_IMGUI_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
    "${FB_IMGUI_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
)
target_include_directories(fb_imgui PUBLIC "${FB_IMGUI_SOURCE_DIR}")
target_link_libraries(fb_imgui PUBLIC glfw)
# glfw's CMake does not propagate GLFW_EXPOSE_NATIVE_WIN32, so it is defined here as
# PUBLIC for the client
target_compile_definitions(fb_imgui PUBLIC GLFW_EXPOSE_NATIVE_WIN32)

# group by on-disk directory structure in VS (cmake/SourceGroups.cmake)
fb_source_group(fb_imgui ROOT "${FB_IMGUI_SOURCE_DIR}" PREFIX "imgui")

set(FB_IMGUI_DILIGENT_DIR "${FB_DILIGENT_SOURCE_DIR}/DiligentTools/Imgui")

# ImGuiImplWin32.cpp is compiled into this library but never called by the client: the app pumps
# events through GLFW, so it uses ImGuiImplDiligent with the GLFW platform backend (see
# client/src/Renderer.cpp and client/bin/main.cpp).
add_library(fb_imgui_diligent STATIC
    "${FB_IMGUI_DILIGENT_DIR}/src/ImGuiUtils.cpp"
    "${FB_IMGUI_DILIGENT_DIR}/src/ImGuiDiligentRenderer.cpp"
    "${FB_IMGUI_DILIGENT_DIR}/src/ImGuiImplDiligent.cpp"
    "${FB_IMGUI_DILIGENT_DIR}/src/ImGuiImplWin32.cpp"
)
# The client code includes headers as "DiligentTools/Imgui/interface/..." and "DiligentCore/..."
target_include_directories(fb_imgui_diligent PUBLIC
    "${diligent_engine_SOURCE_DIR}"
    "${FB_IMGUI_DILIGENT_DIR}/interface"
    # ImGuiDiligentRenderer.cpp / ImGuiImplDiligent.cpp include "MapHelper.hpp", which lives in
    # DiligentCore/Graphics/GraphicsTools/interface; the library itself does not have to be linked.
    "${FB_DILIGENT_SOURCE_DIR}/DiligentCore/Graphics/GraphicsTools/interface"
)
# Diligent-GraphicsTools is not linked: the Imgui bridge only needs GraphicsAccessories
# (imgui rendering does not reference GraphicsTools)
target_link_libraries(fb_imgui_diligent PUBLIC
    fb_imgui
    Diligent-BuildSettings
    Diligent-Common
    Diligent-PlatformInterface
    Diligent-GraphicsEngineInterface
    Diligent-GraphicsAccessories
    Diligent-TargetPlatform
)

# group by on-disk directory structure in VS (cmake/SourceGroups.cmake): Imgui/src
fb_source_group(fb_imgui_diligent ROOT "${FB_IMGUI_DILIGENT_DIR}" PREFIX "Imgui")

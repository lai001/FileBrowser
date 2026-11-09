# Client core (sources shared by FileBrowserClientTest and FileBrowserClient)
add_library(fb_client_core STATIC
    src/Client.cpp
    src/Client.h
    src/CrashDump.cpp
    src/CrashDump.h
    src/DirectoryTree.cpp
    src/DirectoryTree.h
    src/FileBrowserUIController.cpp
    src/FileBrowserUIController.h
    src/FileBrowserUIModel.cpp
    src/FileBrowserUIModel.h
    src/FileBrowserUIView.cpp
    src/FileBrowserUIView.h
    src/ImguiRenderer.cpp
    src/ImguiRenderer.h
    src/Renderer.cpp
    src/Renderer.h
    src/RendererInitializer.cpp
    src/RendererInitializer.h
    src/RenderTarget.cpp
    src/RenderTarget.h
    src/ScanQueue.cpp
    src/ScanQueue.h
    src/ServerLauncher.cpp
    src/ServerLauncher.h
    src/TransferQueue.cpp
    src/TransferQueue.h
    src/TransferState.cpp
    src/TransferState.h
    src/Transfers.cpp
    src/Transfers.h
    src/UiPaths.cpp
    src/UiPaths.h
    src/Viewport.cpp
    src/Viewport.h
)

target_include_directories(fb_client_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

# The scan runs on a worker thread (src/ScanQueue.cpp). Threads::Threads is linked on every platform, as in
# server/CMakeLists.txt: on Windows it adds nothing, on the other platforms it is the system thread library.
find_package(Threads REQUIRED)
target_link_libraries(fb_client_core PRIVATE Threads::Threads)

# The folder picker in src/FileBrowserUIView.cpp (pickFolder) calls CoInitializeEx and CoCreateInstance,
# which are in ole32. fb_system_links does not list it: it covers User32, Gdi32, Shell32, Dxgi, Comdlg32, d3d11,
# D3DCompiler and OpenGL32.
# Comdlg32 alone does not give the modern IFileOpenDialog object.
if(WIN32)
    target_link_libraries(fb_client_core PRIVATE ole32)
endif()

# Dbghelp is not in fb_system_links either: src/CrashDump.cpp calls MiniDumpWriteDump. PUBLIC because the test
# target links fb_client_core, and that call is what its tests check.
if(WIN32)
    target_link_libraries(fb_client_core PUBLIC Dbghelp)
endif()

# group by on-disk directory structure in VS (cmake/SourceGroups.cmake): src / bin
fb_source_group(fb_client_core)

# The client sources include Diligent headers as "DiligentTools/Imgui/interface/..."
# and "DiligentCore/..."; those include dirs come from fb_imgui_diligent (PUBLIC),
# rooted at the Diligent directory resolved by FetchContent.
target_link_libraries(fb_client_core PUBLIC
    filebrowser_proto
    fb_misc
    spdlog::spdlog
    tl::expected
    nlohmann_json::nlohmann_json
    glfw
    fb_imgui
    fb_imgui_diligent
    Diligent-BuildSettings
    Diligent-Common
    Diligent-PlatformInterface
    Diligent-Primitives
    Diligent-GraphicsEngineInterface
    Diligent-GraphicsAccessories
    Diligent-TargetPlatform
)

# Only the D3D11 backend is enabled (cmake/ThirdParty.cmake); the static backend
# library has to be linked separately.
if(TARGET Diligent-GraphicsEngine)
    target_link_libraries(fb_client_core PUBLIC Diligent-GraphicsEngine)
endif()
if(TARGET Diligent-GraphicsEngineD3D11-static)
    target_link_libraries(fb_client_core PUBLIC Diligent-GraphicsEngineD3D11-static)
endif()

# src/Transfers.cpp hashes entry content with absl::ExtendCrc32c, so the state can decide from a recorded hash. The
# target comes from the abseil-cpp grpc brings in; the guard is the same as cmake/Proto.cmake uses for
# absl::str_format. xmake.lua already lists absl_crc32c.
if(TARGET absl::crc32c)
    target_link_libraries(fb_client_core PUBLIC absl::crc32c)
endif()

fb_system_links(fb_client_core)

add_executable(FileBrowserClient bin/main.cpp)
fb_source_group(FileBrowserClient)
target_link_libraries(FileBrowserClient PRIVATE fb_client_core)
fb_system_links(FileBrowserClient)

# FB_CRT_CHECK_ALWAYS_HEAP gates the _CrtSetDbgFlag(_CRTDBG_CHECK_ALWAYS_DF) call in bin/main.cpp.
# It only has an effect with the MSVC debug CRT (/MDd); off by default because every allocation
# then scans the whole heap. Enable with -DFILEBROWSER_CRT_CHECK_ALWAYS_HEAP=ON when hunting for a
# bad free or a buffer overrun: the error is reported at the moment it happens instead of during
# CRT teardown at process exit.
if(FILEBROWSER_CRT_CHECK_ALWAYS_HEAP)
    target_compile_definitions(FileBrowserClient PRIVATE FB_CRT_CHECK_ALWAYS_HEAP)
    message(STATUS "[client] FB_CRT_CHECK_ALWAYS_HEAP is ON for FileBrowserClient")
endif()

#if _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <shlobj.h>
#endif
#include <iostream>
#include <locale>
#include <string>
#include <vector>
#include "spdlog/spdlog.h"
#include "GLFW/glfw3.h"
#include "GLFW/glfw3native.h"
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#include "CrashDump.h"
#include "DiligentCore/Platforms/interface/NativeWindow.h"
#include "Renderer.h"
#include "Viewport.h"
#include "Client.h"
#include "FileBrowserUIController.h"

constexpr static const int MainWindowID = 0;

bool loadSystemFont(const float dpiScale)
{
#if _WIN32
    PWSTR pathToFonts;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Fonts, 0, nullptr, &pathToFonts)))
    {
        const std::filesystem::path fontsPath(pathToFonts);
        CoTaskMemFree(pathToFonts);
        ImGuiIO &io = ImGui::GetIO();
        const std::filesystem::path fontPath = fontsPath / "msyh.ttc";
        const std::string fontPathStr = fontPath.string();
        spdlog::trace("Loading font, {}", fontPathStr);
        ImFont *customFont = io.Fonts->AddFontFromFileTTF(fontPathStr.c_str(), 16.0f * dpiScale, nullptr,
                                                          io.Fonts->GetGlyphRangesChineseFull());
        if (nullptr == customFont)
        {
            return false;
        }
        io.FontDefault = customFont;
        return true;
    }
#endif
    return false;
}

int dumpGlfwError()
{
    const char *description;
    const int errorCode = glfwGetError(&description);
    if (errorCode != GLFW_NO_ERROR)
    {
        spdlog::error("glfw error: {}, {}", errorCode, description);
    }
    return errorCode;
}

// Files dropped from Explorer arrive for the window as a whole. imgui cannot take a drop from the operating
// system: that needs its docking branch, and this is the plain 1.89.2 branch, where ImGuiPlatformIO has no
// AddDropRange. So the paths come from GLFW, which calls DragAcceptFiles on the window and turns WM_DROPFILES
// into this callback. It fires inside glfwPollEvents(), on the same thread that renders, so the controller can
// be called directly - there is no second thread to hand the paths over to, and no mutex to take.
static void onDroppedFiles(GLFWwindow *window, int pathCount, const char **paths)
{
    // A C callback cannot capture anything, so the controller is reached through the window's user pointer.
    FileBrowserUIController *controller = static_cast<FileBrowserUIController *>(glfwGetWindowUserPointer(window));
    if (controller == nullptr)
    {
        return;
    }

    // GLFW gives the paths already decoded to UTF-8, which is how the controller holds every path.
    std::vector<std::string> utf8Paths;
    for (int i = 0; i < pathCount; ++i)
    {
        utf8Paths.push_back(paths[i]);
    }

    controller->handleFileDrop(utf8Paths);
}

int run(int argc, char **argv)
{
    // std::setlocale(LC_ALL, "en_US.UTF-8");
    spdlog::set_level(spdlog::level::trace);
    if (glfwInit() == GLFW_FALSE)
    {
        return dumpGlfwError();
    }
    GLFWmonitor *monitor = glfwGetPrimaryMonitor();
    if (nullptr == monitor)
    {
        return 1;
    }

    float scaleX = 1.0f;
    float scaleY = 1.0f;
    glfwGetMonitorContentScale(monitor, &scaleX, &scaleY);
    const unsigned int windowWidth = 1024 * scaleX;
    const unsigned int windowHeight = 764 * scaleY;

    GLFWwindow *window = glfwCreateWindow(windowWidth, windowHeight, "File Broswer", nullptr, nullptr);
    if (window == nullptr)
    {
        return dumpGlfwError();
    }
    tl::expected<std::unique_ptr<Renderer>, std::string> expectedRenderer = Renderer::create();
    if (!expectedRenderer)
    {
        spdlog::error("Create renderer fail, {}", expectedRenderer.error());
        return 1;
    }
    std::unique_ptr<Renderer> renderer = std::move(expectedRenderer.value());

    renderer->addWindow(MainWindowID, window);
    std::unique_ptr<Viewport> viewport =
        std::unique_ptr<Viewport>(renderer->createViewportWithID(MainWindowID, windowWidth, windowHeight));
    if (!viewport)
    {
        spdlog::error("Create viewport fail");
        return 1;
    }
    // This app pumps events through GLFW, so GLFW is the platform (input) backend. It must be
    // initialized on the ImGui context created by Diligent's renderer backend above.
    if (!ImGui_ImplGlfw_InitForOther(window, true))
    {
        spdlog::error("ImGui GLFW backend init fail");
        return 1;
    }

    const bool isLoadResult = loadSystemFont(scaleX);
    if (!isLoadResult)
    {
        spdlog::warn("Failed to load custom font.");
    }
    viewport->updateFontsTexture();
    viewport->setContentScale(scaleX);

    std::unique_ptr<FileBrowserUIController> fileBrowserUIController = std::make_unique<FileBrowserUIController>();

    // How the drop callback reaches the controller. The controller is destroyed before the window (see the
    // shutdown order below), so the pointer cannot be used after it is gone.
    glfwSetWindowUserPointer(window, fileBrowserUIController.get());
    glfwSetDropCallback(window, onDroppedFiles);

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();
        int currentWindowWidth;
        int currentWindowHeight;
        glfwGetWindowSize(window, &currentWindowWidth, &currentWindowHeight);
        // An iconified window reports 0x0. A frame cannot run without a display size: the frame scope is only
        // opened when the size is non-zero (see ImguiRenderer::newFrame()), and both Begin() and EndFrame() assert
        // on that scope, so the whole frame is skipped here, not just the drawing.
        if (currentWindowWidth <= 0 || currentWindowHeight <= 0)
        {
            continue;
        }
        viewport->setNewSize(currentWindowWidth, currentWindowHeight);
        renderer->getRenderTarget(MainWindowID)->setNewSize(currentWindowWidth, currentWindowHeight);
        renderer->setCurrentRenderTarget(*renderer->getRenderTarget(MainWindowID));
        // The ImGui window is sized to io.DisplaySize every frame (see FileBrowserUIView::render()) and draws its
        // own background, so there is no area around a panel left for this colour: it shows only through the 0.94
        // alpha of the dark theme's WindowBg, which keeps that background from reading as pure black.
        const float clearColor[4] = {0.25f, 0.25f, 0.25f, 1.0f};
        renderer->setClearColor(*renderer->getRenderTarget(MainWindowID), clearColor);
        // The platform backend must run before ImGui::NewFrame(): ImGui::NewFrame() does not call
        // it, so this is what sets io.DisplaySize / io.DeltaTime and feeds the events collected by
        // glfwPollEvents() above.
        ImGui_ImplGlfw_NewFrame();
        viewport->newFrame();

        fileBrowserUIController->render();

        renderer->renderViewport(*viewport);
        renderer->present(*renderer->getRenderTarget(MainWindowID));
    }

    // Release the ImGui backend, the swap chain and the render device while the native
    // window is still alive: the DXGI swap chain must not outlive the window it was
    // created for, and every D3D11 object must be released before the device itself.
    // Shut down the platform backend before the render backend destroys the ImGui context.
    ImGui_ImplGlfw_Shutdown();
    fileBrowserUIController.reset();
    viewport.reset();
    renderer.reset();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}

int main(int argc, char **argv)
{
    // Where a crash is recorded: a dump in a folder of its own next to the exe, the same place the state file
    // lives. Installed before anything else so the handlers cover the whole run, including the setup below.
    crash::install(crash::moduleDirectory() / "crashes");
#ifdef FB_CRT_CHECK_ALWAYS_HEAP
#if defined(_MSC_VER)
    // Controlled by -DFILEBROWSER_CRT_CHECK_ALWAYS_HEAP=ON (see client/CMakeLists.txt). Checking
    // every debug-heap block on every allocation/free reports a bad write or double free at the
    // moment it happens, instead of only when CRT teardown touches the damaged memory at exit.
    _CrtSetDbgFlag(_CRTDBG_CHECK_ALWAYS_DF);
#endif
#endif
    const int code = run(argc, argv);
#ifdef EXITCHECK
    std::cin >> std::string();
#endif
    return code;
}

#if _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <shlobj.h>
#endif
#include <iostream>
#include <locale>
#include "spdlog/spdlog.h"
#include "GLFW/glfw3.h"
#include "GLFW/glfw3native.h"
#include "backends/imgui_impl_glfw.h"
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
    const bool isLoadResult = loadSystemFont(scaleX);
    if (!isLoadResult)
    {
        spdlog::warn("Failed to load custom font.");
    }
    viewport->updateFontsTexture();
    viewport->setContentScale(scaleX);
    glfwSetWindowFocusCallback(window, ImGui_ImplGlfw_WindowFocusCallback);
    glfwSetCursorEnterCallback(window, ImGui_ImplGlfw_CursorEnterCallback);
    glfwSetCursorPosCallback(window, ImGui_ImplGlfw_CursorPosCallback);
    glfwSetMouseButtonCallback(window, ImGui_ImplGlfw_MouseButtonCallback);
    glfwSetScrollCallback(window, ImGui_ImplGlfw_ScrollCallback);
    glfwSetKeyCallback(window, ImGui_ImplGlfw_KeyCallback);
    glfwSetCharCallback(window, ImGui_ImplGlfw_CharCallback);
    glfwSetMonitorCallback(ImGui_ImplGlfw_MonitorCallback);

    std::unique_ptr<FileBrowserUIController> fileBrowserUIController = std::make_unique<FileBrowserUIController>();

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        renderer->setCurrentRenderTarget(*renderer->getRenderTarget(MainWindowID));
        const float clearColor[4] = {1.0f, 0.0f, 0.0f, 1.0f};
        renderer->setClearColor(*renderer->getRenderTarget(MainWindowID), clearColor);
        viewport->newFrame();

        fileBrowserUIController->render();

        renderer->renderViewport(*viewport);
        renderer->present(*renderer->getRenderTarget(MainWindowID));
    }

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}

int main(int argc, char **argv)
{
    const int code = run(argc, argv);
#ifdef EXITCHECK
    std::cin >> std::string();
#endif
    return code;
}

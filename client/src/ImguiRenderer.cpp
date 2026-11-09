#include "ImguiRenderer.h"
#include "DiligentCore/Graphics/GraphicsEngine/interface/GraphicsTypes.h"
#include "imgui.h"
#include "spdlog/spdlog.h"

ImguiRenderer::ImguiRenderer(std::shared_ptr<Diligent::ImGuiImplDiligent> imGuiImplDiligent,
                             unsigned int renderSurfaceWidth, unsigned int renderSurfaceHeight)
    : imGuiImplDiligent(imGuiImplDiligent), renderSurfaceWidth(renderSurfaceWidth),
      renderSurfaceHeight(renderSurfaceHeight)
{
}

ImguiRenderer::~ImguiRenderer()
{
    // Diligent's ImGui renderer destructor is empty, so its device objects (pipeline state - which
    // holds the shaders and the input layout - vertex/index buffers, constant buffer, font texture
    // view and the shader resource binding) have to be released explicitly here; otherwise they are
    // still attached to the device when the device is released.
    if (imGuiImplDiligent)
    {
        imGuiImplDiligent->InvalidateDeviceObjects();
    }
}

void ImguiRenderer::newFrame()
{
    if (renderSurfaceWidth != 0 && renderSurfaceHeight != 0 && imGuiImplDiligent)
    {
        imGuiImplDiligent->NewFrame(renderSurfaceWidth, renderSurfaceHeight, Diligent::SURFACE_TRANSFORM_IDENTITY);
    }
}

void ImguiRenderer::endFrame()
{
    if (imGuiImplDiligent)
    {
        imGuiImplDiligent->EndFrame();
    }
}

void ImguiRenderer::render(Diligent::IDeviceContext &deviceContext)
{
    if (imGuiImplDiligent)
    {
        imGuiImplDiligent->Render(&deviceContext);
    }
}

void ImguiRenderer::updateFontsTexture()
{
    if (imGuiImplDiligent)
    {
        imGuiImplDiligent->UpdateFontsTexture();
    }
}

void ImguiRenderer::setContentScale(const float scale)
{
    if (imGuiImplDiligent)
    {
        ImGuiIO &io = ImGui::GetIO();
        // io.DisplayFramebufferScale = ImVec2(scale, scale);
        // ImGuiIO &io = ImGui::GetIO();
        // io.FontGlobalScale = scale;
        ImGui::GetStyle().ScaleAllSizes(scale);
    }
}

void ImguiRenderer::setNewSize(const int width, const int height)
{
    renderSurfaceWidth = width;
    renderSurfaceHeight = height;
}

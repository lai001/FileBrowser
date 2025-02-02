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

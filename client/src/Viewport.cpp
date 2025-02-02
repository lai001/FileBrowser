#include "Viewport.h"

Viewport::Viewport(std::unique_ptr<ImguiRenderer> imguiRenderer) : imguiRenderer(std::move(imguiRenderer))
{
}

Viewport::~Viewport()
{
}

void Viewport::newFrame()
{
    if (imguiRenderer)
    {
        imguiRenderer->newFrame();
    }
}

void Viewport::endFrame()
{
    if (imguiRenderer)
    {
        imguiRenderer->endFrame();
    }
}

void Viewport::render(Diligent::IDeviceContext &deviceContext)
{
    if (imguiRenderer)
    {
        imguiRenderer->render(deviceContext);
    }
}

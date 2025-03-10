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

void Viewport::updateFontsTexture()
{
    if (imguiRenderer)
    {
        imguiRenderer->updateFontsTexture();
    }
}

void Viewport::setContentScale(const float scale)
{
    if (imguiRenderer)
    {
        imguiRenderer->setContentScale(scale);
    }
}

void Viewport::setNewSize(const int width, const int height)
{
    if (imguiRenderer)
    {
        imguiRenderer->setNewSize(width, height);
    }
}

#include "RenderTarget.h"

RenderTarget::RenderTarget(Diligent::RefCntAutoPtr<Diligent::ISwapChain> swapChain,
                           Diligent::Win32NativeWindow nativeWindow)
    : swapChain(swapChain), nativeWindow(nativeWindow)
{
}

RenderTarget::~RenderTarget()
{
}

Diligent::RefCntAutoPtr<Diligent::ISwapChain> RenderTarget::getSwapChain() const
{
    return swapChain;
}

const Diligent::Win32NativeWindow &RenderTarget::getNativeWindow() const
{
    return nativeWindow;
}

void RenderTarget::setNewSize(const int width, const int height)
{
    if (swapChain->GetDesc().Width != width || swapChain->GetDesc().Height != height)
    {
        swapChain->Resize(width, height);
    }
}

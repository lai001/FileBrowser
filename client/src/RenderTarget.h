#ifndef SRC_RENDER_TARGET_H
#define SRC_RENDER_TARGET_H

#include "GLFW/glfw3.h"
#include "GLFW/glfw3native.h"
#include "DiligentCore/Common/interface/RefCntAutoPtr.hpp"
#include "DiligentCore/Graphics/GraphicsEngine/interface/SwapChain.h"

class RenderTarget
{
  public:
    RenderTarget(Diligent::RefCntAutoPtr<Diligent::ISwapChain> swapChain, Diligent::Win32NativeWindow nativeWindow);

    ~RenderTarget();

    Diligent::RefCntAutoPtr<Diligent::ISwapChain> getSwapChain() const;

    const Diligent::Win32NativeWindow &getNativeWindow() const;

  private:
    RenderTarget(const RenderTarget &) = delete;

    RenderTarget &operator=(const RenderTarget &) = delete;

    Diligent::RefCntAutoPtr<Diligent::ISwapChain> swapChain;
#if PLATFORM_WIN32
    Diligent::Win32NativeWindow nativeWindow;
#endif
};

#endif

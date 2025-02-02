#ifndef SRC_RENDERER_INITIALIZER_H
#define SRC_RENDERER_INITIALIZER_H

#include "DiligentCore/Common/interface/RefCntAutoPtr.hpp"
#include "DiligentCore/Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "DiligentCore/Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "DiligentCore/Graphics/GraphicsEngine/interface/SwapChain.h"
#include "DiligentCore/Graphics/GraphicsEngine/interface/GraphicsTypes.h"

class RendererInitializer
{
  private:
    RendererInitializer();

    bool init(const Diligent::RENDER_DEVICE_TYPE deviceType);

  public:
    static RendererInitializer *create(const Diligent::RENDER_DEVICE_TYPE deviceType);

    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device;

    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> immediateContext;

    Diligent::IEngineFactory *engineFactory;
};

#endif
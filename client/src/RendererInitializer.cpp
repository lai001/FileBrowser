#include "RendererInitializer.h"
#include "spdlog/spdlog.h"
#include "DiligentCore/Platforms/interface/NativeWindow.h"
#if D3D12_SUPPORTED
#include "DiligentCore/Graphics/GraphicsEngineD3D12/interface/EngineFactoryD3D12.h"
#endif
#if D3D11_SUPPORTED
#include "DiligentCore/Graphics/GraphicsEngineD3D11/interface/EngineFactoryD3D11.h"
#endif
#if GL_SUPPORTED
#include "DiligentCore/Graphics/GraphicsEngineOpenGL/interface/EngineFactoryOpenGL.h"
#endif
#if VULKAN_SUPPORTED
#include "DiligentCore/Graphics/GraphicsEngineVulkan/interface/EngineFactoryVk.h"
#endif

using namespace Diligent;

RendererInitializer::RendererInitializer() : engineFactory(nullptr)
{
}

RendererInitializer *RendererInitializer::create(const Diligent::RENDER_DEVICE_TYPE deviceType)
{
    RendererInitializer *rendererInitializer = new RendererInitializer();
    if (rendererInitializer->init(deviceType))
    {
        return rendererInitializer;
    }
    delete rendererInitializer;
    return nullptr;
}

bool RendererInitializer::init(const Diligent::RENDER_DEVICE_TYPE deviceType)
{
    SwapChainDesc swapChainDesc;
    switch (deviceType)
    {
#if D3D11_SUPPORTED
    case RENDER_DEVICE_TYPE_D3D11: {
        EngineD3D11CreateInfo EngineCI;
#if ENGINE_DLL
        auto *GetEngineFactoryD3D11 = LoadGraphicsEngineD3D11();
#endif
        Diligent::IEngineFactoryD3D11 *factoryD3D11 = GetEngineFactoryD3D11();
        factoryD3D11->CreateDeviceAndContextsD3D11(EngineCI, &device, &immediateContext);
        engineFactory = factoryD3D11;
    }
    break;
#endif

#if GL_SUPPORTED
    case RENDER_DEVICE_TYPE_GL: {
#if ENGINE_DLL
        auto GetEngineFactoryOpenGL = LoadGraphicsEngineOpenGL();
#endif
        Diligent::IEngineFactoryOpenGL *factoryOpenGL = GetEngineFactoryOpenGL();
        EngineGLCreateInfo engineGLCreateInfo;
        factoryOpenGL->CreateDeviceAndSwapChainGL(engineGLCreateInfo, &device, &immediateContext, swapChainDesc,
                                                  nullptr);
        engineFactory = factoryOpenGL;
    }
    break;
#endif

#if D3D12_SUPPORTED
    case RENDER_DEVICE_TYPE_D3D12: {
#if ENGINE_DLL
        auto *GetEngineFactoryD3D12 = LoadGraphicsEngineD3D12();
#endif
        Diligent::IEngineFactoryD3D12 *factoryD3D12 = GetEngineFactoryD3D12();
        EngineD3D12CreateInfo engineD3D12CreateInfo;
        factoryD3D12->CreateDeviceAndContextsD3D12(engineD3D12CreateInfo, &device, &immediateContext);
        engineFactory = factoryD3D12;
    }
    break;
#endif

#if VULKAN_SUPPORTED
    case RENDER_DEVICE_TYPE_VULKAN: {
#if ENGINE_DLL
        auto GetEngineFactoryVk = LoadGraphicsEngineVk();
#endif
        EngineVkCreateInfo engineCI;
        Diligent::IEngineFactoryVk *factoryVk = GetEngineFactoryVk();
        factoryVk->CreateDeviceAndContextsVk(engineCI, &device, &immediateContext);
        engineFactory = factoryVk;
    }
    break;
#endif

    default:
        spdlog::error("Unknown/unsupported device type");
        return false;
        break;
    }

    if (device == nullptr || immediateContext == nullptr)
    {
        return false;
    }

    return true;
}
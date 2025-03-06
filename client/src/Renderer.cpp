#include "Renderer.h"
#include <memory>
#include "spdlog/spdlog.h"
#include "imgui.h"
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
#if PLATFORM_WIN32
#include "D:/Workspace/Vendor/DiligentEngine/DiligentTools/Imgui/interface/ImGuiImplWin32.hpp"
#endif
#include "RendererInitializer.h"

Renderer::Renderer() : engineFactory(nullptr), deviceType(Diligent::RENDER_DEVICE_TYPE::RENDER_DEVICE_TYPE_UNDEFINED)
{
}

Renderer::~Renderer()
{
}

tl::expected<std::unique_ptr<Renderer>, std::string> Renderer::create()
{
    const Diligent::RENDER_DEVICE_TYPE deviceType = Diligent::RENDER_DEVICE_TYPE::RENDER_DEVICE_TYPE_D3D11;
    tl::expected<RendererInitializer, std::string> rendererInitializer = RendererInitializer::create(deviceType);
    return rendererInitializer.map([deviceType](RendererInitializer &rendererInitializer) {
        Renderer *renderer = new Renderer();
        renderer->deviceType = deviceType;
        renderer->device = rendererInitializer.device;
        renderer->immediateContext = rendererInitializer.immediateContext;
        renderer->engineFactory = rendererInitializer.engineFactory;
        return std::unique_ptr<Renderer>(renderer);
    });
}

bool Renderer::addWindow(const int id, GLFWwindow *window)
{
    if (engineFactory == nullptr)
    {
        return false;
    }
    if (window == nullptr)
    {
        return false;
    }
#if PLATFORM_WIN32
    Diligent::Win32NativeWindow nativeWindow{glfwGetWin32Window(window)};
#else
#error No support on this platform
#endif

    switch (deviceType)
    {
#if D3D11_SUPPORTED
    case Diligent::RENDER_DEVICE_TYPE_D3D11: {
        Diligent::IEngineFactoryD3D11 *factoryD3D11 = reinterpret_cast<Diligent::IEngineFactoryD3D11 *>(engineFactory);
        if (factoryD3D11)
        {
            Diligent::RefCntAutoPtr<Diligent::ISwapChain> swapChain;
            Diligent::SwapChainDesc swapChainDesc;
            factoryD3D11->CreateSwapChainD3D11(device, immediateContext, swapChainDesc, Diligent::FullScreenModeDesc{},
                                               nativeWindow, &swapChain);
            renderTargets[id] = std::make_unique<RenderTarget>(swapChain, nativeWindow);
            return true;
        }
    }
    break;
#endif
#if D3D12_SUPPORTED
    case Diligent::RENDER_DEVICE_TYPE_D3D12: {
        Diligent::IEngineFactoryD3D12 *factoryD3D12 = reinterpret_cast<Diligent::IEngineFactoryD3D12 *>(engineFactory);
        if (factoryD3D12)
        {
            Diligent::RefCntAutoPtr<Diligent::ISwapChain> swapChain;
            Diligent::SwapChainDesc swapChainDesc;
            factoryD3D12->CreateSwapChainD3D12(device, immediateContext, swapChainDesc, Diligent::FullScreenModeDesc{},
                                               nativeWindow, &swapChain);
            renderTargets[id] = std::make_unique<RenderTarget>(swapChain, nativeWindow);
            return true;
        }
    }
    break;
#endif
#if GL_SUPPORTED
    case Diligent::RENDER_DEVICE_TYPE_GL: {
        Diligent::IEngineFactoryOpenGL *factoryOpenGL =
            reinterpret_cast<Diligent::IEngineFactoryOpenGL *>(engineFactory);
        if (factoryOpenGL)
        {
        }
    }
    break;
#endif
#if VULKAN_SUPPORTED
    case Diligent::RENDER_DEVICE_TYPE_VULKAN: {
        Diligent::IEngineFactoryVk *factoryVk = reinterpret_cast<Diligent::IEngineFactoryVk *>(engineFactory);
        if (factoryVk)
        {
            Diligent::RefCntAutoPtr<Diligent::ISwapChain> swapChain;
            Diligent::SwapChainDesc swapChainDesc;
            factoryVk->CreateSwapChainVk(device, immediateContext, swapChainDesc, nativeWindow, &swapChain);
            renderTargets[id] = std::make_unique<RenderTarget>(swapChain, nativeWindow);
            return true;
        }
    }
    break;
#endif
    default:
        break;
    }
    return false;
}

ImguiRenderer *Renderer::createImguiRenderer(RenderTarget &renderTarget, unsigned int renderSurfaceWidth,
                                             unsigned int renderSurfaceHeight)
{
    if (!renderTarget.getSwapChain())
    {
        return nullptr;
    }
    Diligent::SwapChainDesc swapChainDesc = renderTarget.getSwapChain()->GetDesc();
    const Diligent::Win32NativeWindow &nativeWindow = renderTarget.getNativeWindow();
    if (nativeWindow.hWnd == nullptr)
    {
        return nullptr;
    }
    HWND hwnd = reinterpret_cast<HWND>(nativeWindow.hWnd);
    std::shared_ptr<Diligent::ImGuiImplWin32> imGuiImplWin32 =
        std::move(Diligent::ImGuiImplWin32::Create(Diligent::ImGuiDiligentCreateInfo{device, swapChainDesc}, hwnd));
    ImguiRenderer *imguiRenderer = new ImguiRenderer(imGuiImplWin32, renderSurfaceWidth, renderSurfaceHeight);
    return imguiRenderer;
}

Viewport *Renderer::createViewport(RenderTarget &renderTarget, unsigned int renderSurfaceWidth,
                                   unsigned int renderSurfaceHeight)
{
    ImguiRenderer *imguiRenderer = createImguiRenderer(renderTarget, renderSurfaceWidth, renderSurfaceHeight);
    if (!imguiRenderer)
    {
        return nullptr;
    }
    Viewport *viewport = new Viewport(std::unique_ptr<ImguiRenderer>(imguiRenderer));
    return viewport;
}

Viewport *Renderer::createViewportWithID(const int id, unsigned int renderSurfaceWidth,
                                         unsigned int renderSurfaceHeight)
{
    RenderTarget *renderTarget = getRenderTarget(id);
    if (renderTarget == nullptr)
    {
        return nullptr;
    }
    return createViewport(*renderTarget, renderSurfaceWidth, renderSurfaceHeight);
}

RenderTarget *Renderer::getRenderTarget(const int id) const
{
    if (renderTargets.find(id) == renderTargets.end())
    {
        return nullptr;
    }
    return renderTargets.at(id).get();
}

void Renderer::renderViewport(Viewport &viewport)
{
    viewport.render(*immediateContext);
}

void Renderer::present(RenderTarget &renderTarget)
{
    renderTarget.getSwapChain()->Present();
}

void Renderer::setCurrentRenderTarget(RenderTarget &renderTarget)
{
    Diligent::ITextureView *backBufferRTV = renderTarget.getSwapChain()->GetCurrentBackBufferRTV();
    Diligent::ITextureView *depthBufferDSV = renderTarget.getSwapChain()->GetDepthBufferDSV();
    immediateContext->SetRenderTargets(1, &backBufferRTV, depthBufferDSV,
                                       Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

void Renderer::setClearColor(RenderTarget &renderTarget, const float clearColor[4])
{
    Diligent::ITextureView *backBufferRTV = renderTarget.getSwapChain()->GetCurrentBackBufferRTV();
    immediateContext->ClearRenderTarget(backBufferRTV, clearColor, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}
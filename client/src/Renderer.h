#ifndef SRC_RENDERER_H
#define SRC_RENDERER_H
#include <vector>
#include <unordered_map>
#include "GLFW/glfw3.h"
#include "GLFW/glfw3native.h"
#include "DiligentCore/Common/interface/RefCntAutoPtr.hpp"
#include "DiligentCore/Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "DiligentCore/Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "DiligentCore/Graphics/GraphicsEngine/interface/SwapChain.h"
#include "tl/expected.hpp"
#include "RenderTarget.h"
#include "ImguiRenderer.h"
#include "Viewport.h"

class Renderer
{
  public:
    Renderer(const Renderer &) = delete;

    Renderer &operator=(const Renderer &) = delete;

    ~Renderer();

    static tl::expected<std::unique_ptr<Renderer>, std::string> create();

    bool addWindow(const int id, GLFWwindow *window);

    RenderTarget *getRenderTarget(const int id) const;

    Viewport *createViewportWithID(const int id, unsigned int renderSurfaceWidth, unsigned int renderSurfaceHeight);

    void renderViewport(Viewport &viewport);

    void setCurrentRenderTarget(RenderTarget &renderTarget);

    void setClearColor(RenderTarget &renderTarget, const float clearColor[4]);

    void present(RenderTarget &renderTarget);

  private:
    Renderer();

    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device;

    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> immediateContext;

    Diligent::IEngineFactory *engineFactory;

    std::unordered_map<int, std::unique_ptr<RenderTarget>> renderTargets;

    Diligent::RENDER_DEVICE_TYPE deviceType;

    ImguiRenderer *createImguiRenderer(RenderTarget &renderTarget, unsigned int renderSurfaceWidth,
                                       unsigned int renderSurfaceHeight);

    Viewport *createViewport(RenderTarget &renderTarget, unsigned int renderSurfaceWidth,
                             unsigned int renderSurfaceHeight);
};

#endif
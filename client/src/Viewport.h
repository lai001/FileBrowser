#ifndef SRC_VIEWPORT_H
#define SRC_VIEWPORT_H
#include <memory>
#include "ImguiRenderer.h"

class Viewport
{
  public:
    Viewport(std::unique_ptr<ImguiRenderer> imguiRenderer);

    ~Viewport();

    void newFrame();

    void endFrame();

    void render(Diligent::IDeviceContext &deviceContext);

  private:
    std::unique_ptr<ImguiRenderer> imguiRenderer;
};

#endif
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

    void updateFontsTexture();

    void setContentScale(const float scale);

    void setNewSize(const int width, const int height);

  private:
    std::unique_ptr<ImguiRenderer> imguiRenderer;
};

#endif
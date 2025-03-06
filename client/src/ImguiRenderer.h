#ifndef SRC_IMGUI_RENDERER_H
#define SRC_IMGUI_RENDERER_H
#include <memory>
#include "DiligentTools/Imgui/interface/ImGuiImplDiligent.hpp"

class ImguiRenderer
{
  public:
    ImguiRenderer(std::shared_ptr<Diligent::ImGuiImplDiligent> imGuiImplDiligent, unsigned int renderSurfaceWidth,
                  unsigned int renderSurfaceHeight);

    ~ImguiRenderer();

    void newFrame();

    void endFrame();

    void render(Diligent::IDeviceContext &deviceContext);

    void updateFontsTexture();

    void setContentScale (const float scale);

  private:
    std::shared_ptr<Diligent::ImGuiImplDiligent> imGuiImplDiligent;

    unsigned int renderSurfaceWidth;

    unsigned int renderSurfaceHeight;
};

#endif
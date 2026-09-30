#include "CameraController.h"

namespace origin
{

glm::vec2 CameraController::ToNdc(float windowX, float windowY) const
{
    int w = 0;
    int h = 0;
    SDL_GetWindowSize(m_window, &w, &h);
    if (w <= 0 || h <= 0)
    {
        return glm::vec2(0.0f);
    }

    // Y 取反：窗口坐标原点在左上、向下为正，而我们要的 NDC 是向上为正，
    // 这样向上拖动才会让物体向上翻。
    return glm::vec2(2.0f * windowX / static_cast<float>(w) - 1.0f,
                     1.0f - 2.0f * windowY / static_cast<float>(h));
}

glm::vec2 CameraController::TouchToNdc(float normalizedX, float normalizedY)
{
    return glm::vec2(2.0f * normalizedX - 1.0f, 1.0f - 2.0f * normalizedY);
}

}  // namespace origin

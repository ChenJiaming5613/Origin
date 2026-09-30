#pragma once

#include "Camera.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>

#include <glm/glm.hpp>

namespace origin
{

// 相机控制器基类：把输入事件翻译成对 Camera 位姿的修改。
//
// 与 Camera 的分工：
//   Camera     —— 只有位姿与投影，不知道输入的存在
//   Controller —— 拥有自己的交互参数化（轨道 / FPS / 飞行 ...），
//                 每次改动后把结果写回 Camera 的 position + orientation
//
// 基类只放两样真正通用的东西：坐标换算与灵敏度。手势语义完全交给子类 ——
// 轨道相机的"拖动=绕目标转"和 FPS 相机的"拖动=转头"没有共同抽象可言。
class CameraController
{
public:
    CameraController(Camera& camera, SDL_Window* window) : m_camera(camera), m_window(window) {}

    virtual ~CameraController() = default;

    // 持有引用，拷贝没有意义
    CameraController(const CameraController&)            = delete;
    CameraController& operator=(const CameraController&) = delete;

    // 返回 true 表示事件已被相机消费。
    //
    // 调用方必须先把事件交给 ImGui，并在 ImGui 想要捕获输入时跳过本函数 ——
    // 否则拖动面板上的滑条会同时把相机转起来。
    virtual bool HandleEvent(const SDL_Event& event) = 0;

    // 焦点丢失 / 进入后台时调用，清掉所有按下状态。
    // 不做这件事的话，在拖动中切后台再回来，控制器会以为按键仍然按着，
    // 下一次移动就会产生一次巨大的跳变。
    virtual void ResetInputState() = 0;

    // ---- 灵敏度 -------------------------------------------------------------
    // 刻意用 public 数据成员：ImGui 面板要直接把地址传给 SliderFloat，
    // 包一层 getter/setter 只会让面板代码变啰嗦而没有任何收益。
    float rotateSpeed = 1.0f;
    float zoomSpeed   = 1.0f;
    float panSpeed    = 1.0f;
    bool  invertY     = false;

protected:
    // 窗口坐标（像素）-> [-1,1] 的 NDC，Y 向上为正。
    //
    // ⚠️ 用 SDL_GetWindowSize（逻辑坐标）而不是 SDL_GetWindowSizeInPixels：
    // 鼠标事件给的就是逻辑坐标，高 DPI 下两者不相等，混用会让灵敏度偏掉
    // （在 2x 缩放的屏幕上拖动量会差一倍）。
    glm::vec2 ToNdc(float windowX, float windowY) const;

    // 触摸事件的 x/y 已经归一化到 0..1，不需要查窗口尺寸，直接线性映射。
    // 独立成函数是为了让"两条输入路径用的是同一套 NDC 约定"这件事显式可见。
    static glm::vec2 TouchToNdc(float normalizedX, float normalizedY);

    Camera&     m_camera;
    SDL_Window* m_window = nullptr;
};

}  // namespace origin

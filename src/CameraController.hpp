#pragma once

#include "Camera.hpp"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>

namespace origin {

// Arcball 输入控制器。把 SDL 事件翻译成 Camera 的操作。
//
// 手势映射（两个平台的语义刻意保持一致）：
//
//   Windows / 鼠标            Android / 触摸
//   ------------------------  ---------------------------
//   左键拖动    -> 旋转        单指拖动   -> 旋转
//   右/中键拖动 -> 平移        双指平移   -> 平移
//   滚轮        -> 缩放        双指捏合   -> 缩放
//
// 关于 arcball 本身：把屏幕点投影到一个覆盖视口的虚拟单位球上，取拖动前后
// 两个球面向量之间的最短旋转。相比 yaw/pitch 欧拉角方案的好处是斜向拖动不失真、
// 没有万向锁、可以越过天顶继续转。球外的点投影到球的边缘（Shoemake 的经典做法），
// 于是在画面边缘拖动会得到绕视线方向的滚转，而不是突然失去响应。
class CameraController {
public:
    CameraController(Camera& camera, SDL_Window* window) : camera_(camera), window_(window) {}

    // 返回 true 表示事件已被相机消费。
    //
    // 调用方必须先把事件交给 ImGui，并在 ImGui 想要捕获输入时跳过本函数 ——
    // 否则拖动面板上的滑条会同时把相机转起来。
    bool handleEvent(const SDL_Event& event);

    // 焦点丢失 / 进入后台时调用，清掉所有按下状态。
    // 不做这件事的话，在拖动中切后台再回来，控制器会以为按键仍然按着，
    // 下一次鼠标移动就会产生一次巨大的跳变旋转。
    void resetInputState();

    // 可在面板里调的灵敏度
    float rotateSpeed = 1.0f;
    float zoomSpeed   = 1.0f;
    float panSpeed    = 1.0f;
    bool  invertY     = false;

private:
    // 把窗口坐标（像素）映射到 [-1,1] 的 NDC，Y 向上为正。
    glm::vec2 toNdc(float windowX, float windowY) const;

    // 屏幕点 -> 虚拟球面向量
    static glm::vec3 projectToSphere(const glm::vec2& ndc);

    // 两个屏幕点之间的 arcball 旋转增量
    glm::quat arcballDelta(const glm::vec2& fromNdc, const glm::vec2& toNdc) const;

    void applyRotate(const glm::vec2& fromNdc, const glm::vec2& toNdc);

    Camera&     camera_;
    SDL_Window* window_ = nullptr;

    // ---- 鼠标状态 ----------------------------------------------------------
    bool      mouseRotating_ = false;
    bool      mousePanning_  = false;
    glm::vec2 lastMouseNdc_{0.0f, 0.0f};

    // ---- 触摸状态 ----------------------------------------------------------
    // 只跟踪前两根手指：再多的手指对 arcball 没有额外语义，
    // 而且用固定数组可以完全避免每帧在事件回调里做堆分配。
    struct Finger {
        SDL_FingerID id     = 0;
        bool         active = false;
        glm::vec2    ndc{0.0f, 0.0f};
    };
    std::array<Finger, 2> fingers_{};

    // 双指手势的上一帧量，用于算捏合比例与平移增量
    float     lastPinchDistance_ = 0.0f;
    glm::vec2 lastPinchCenter_{0.0f, 0.0f};
    bool      pinchActive_       = false;

    int activeFingerCount() const;
    int findFinger(SDL_FingerID id) const;

    void onFingerDown(const SDL_TouchFingerEvent& e);
    void onFingerUp(const SDL_TouchFingerEvent& e);
    void onFingerMotion(const SDL_TouchFingerEvent& e);
    void beginPinch();
};

}  // namespace origin

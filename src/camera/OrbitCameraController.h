#pragma once

#include "CameraController.h"

#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>

namespace origin
{

// Arcball（轨道）控制器。相机绕一个目标点转，距离可缩放，目标点可平移。
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
//
// 轨道状态（target / distance / 累积旋转）存在**控制器**里，不在 Camera 里。
// Camera 只拿到换算后的 position + orientation。这样同一个 Camera 可以被
// 不同控制器接管，也可以被动画系统直接喂位姿。
class OrbitCameraController : public CameraController
{
public:
    OrbitCameraController(Camera& camera, SDL_Window* window);

    bool HandleEvent(const SDL_Event& event) override;
    void ResetInputState() override;

    // ---- 轨道操作（也可被面板/渲染器直接调用）-------------------------------

    // 左乘一个旋转增量。左乘而非右乘：增量是在**当前**屏幕空间里量出来的，
    // 必须作用在已累积的朝向之外侧，否则拖动方向会随已有旋转而漂移。
    void Orbit(const glm::quat& delta);

    // 相对缩放。用乘法而不是加减：等量的手势在远近处产生的观感变化才一致
    // （加法在靠近目标时会显得过于灵敏）。
    void Zoom(float factor);

    // 沿相机的右/上方向平移目标点。dx/dy 是 NDC 位移量，
    // 内部按 Camera::HalfExtentAtDistance 换算成世界单位 ——
    // 于是拉远后同样的拖动会移动更多，与观感一致，且换正交相机也自动正确。
    void Pan(float dxNdc, float dyNdc);

    void Reset();

    // ---- 轨道状态 -----------------------------------------------------------

    void SetTarget(const glm::vec3& target);
    void SetDistance(float distance);
    void SetDistanceLimits(float minDistance, float maxDistance);

    const glm::vec3& Target() const { return m_target; }
    float            Distance() const { return m_distance; }
    const glm::quat& Rotation() const { return m_rotation; }

    // 让相机正好框住一个包围球（glTF 加载完按模型尺寸初始化时用）。
    // 同时设好距离上下限，避免滚轮一格就穿进模型内部或飞到天外。
    void FrameBounds(const glm::vec3& center, float radius);

private:
    // 把 target / distance / m_rotation 换算成 Camera 的 position + orientation。
    // 任何改动轨道状态的地方都必须在末尾调它，否则相机与控制器状态会脱节。
    void SyncCamera();

    // 屏幕点 -> 虚拟球面向量
    static glm::vec3 ProjectToSphere(const glm::vec2& ndc);

    // 两个屏幕点之间的 arcball 旋转增量
    glm::quat ArcballDelta(const glm::vec2& fromNdc, const glm::vec2& toNdc) const;

    void ApplyRotate(const glm::vec2& fromNdc, const glm::vec2& toNdc);

    // ---- 轨道状态 -----------------------------------------------------------
    // m_rotation 与 Camera::Orientation 同语义（world -> view）。
    glm::quat m_rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 m_target   = glm::vec3(0.0f);
    float     m_distance = 3.0f;

    float m_minDistance = 0.5f;
    float m_maxDistance = 50.0f;

    // ---- 鼠标状态 -----------------------------------------------------------
    bool      m_mouseRotating = false;
    bool      m_mousePanning  = false;
    glm::vec2 m_lastDragNdc   = glm::vec2(0.0f);

    // ---- 触摸状态 -----------------------------------------------------------
    // 只跟踪前两根手指：再多的手指对 arcball 没有额外语义，
    // 而且用固定数组可以完全避免在事件回调里做堆分配。
    struct Finger
    {
        SDL_FingerID id     = 0;
        bool         active = false;
        glm::vec2    ndc    = glm::vec2(0.0f);
    };
    std::array<Finger, 2> m_fingers{};

    // 双指手势的上一帧量，用于算捏合比例与平移增量
    float     m_lastPinchDistance = 0.0f;
    glm::vec2 m_lastPinchCenter   = glm::vec2(0.0f);
    bool      m_pinchActive       = false;

    int  ActiveFingerCount() const;
    int  FindFinger(SDL_FingerID id) const;
    void OnFingerDown(const SDL_TouchFingerEvent& e);
    void OnFingerUp(const SDL_TouchFingerEvent& e);
    void OnFingerMotion(const SDL_TouchFingerEvent& e);
    void BeginPinch();
};

}  // namespace origin

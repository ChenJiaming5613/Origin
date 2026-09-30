#include "OrbitCameraController.h"

#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_touch.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace origin
{
namespace
{

// 缩放灵敏度：滚轮一格对应的距离比例。
// 用比例而非固定步长，保证远近处的手感一致。
constexpr float kWheelZoomStep = 0.12f;

// 低于这个位移就不产生旋转。抖动过滤，同时避免在 arcball 里对零向量做 normalize。
constexpr float kMinDragNdc = 1e-5f;

}  // namespace

OrbitCameraController::OrbitCameraController(Camera& camera, SDL_Window* window)
    : CameraController(camera, window)
{
    // 构造即同步一次：否则在第一个输入事件到来之前，Camera 还停留在
    // 它自己的默认位姿上，与控制器的 target/distance 不一致 ——
    // 表现为启动后第一帧的画面位置不对，动一下鼠标才"跳"到正确位置。
    SyncCamera();
}

// =============================================================================
// 轨道状态 -> 相机位姿
// =============================================================================

void OrbitCameraController::SyncCamera()
{
    // 相机位置 = 目标点沿"视线反方向"退 distance。
    //
    // m_rotation 是 world -> view，所以用它的共轭把视图空间的 +Z
    // （即视线的反方向）变换回世界空间。用共轭而不是 inverse()：
    // 单位四元数下两者相等，但共轭不需要除以模。
    const glm::quat inv = glm::conjugate(m_rotation);

    m_camera.SetOrientation(m_rotation);
    m_camera.SetPosition(m_target + inv * glm::vec3(0.0f, 0.0f, m_distance));

    // 等价性说明（相对旧实现）：
    //   旧版 view = T(0,0,-d) * R(q) * T(-target)
    //   现版 view = R(q) * T(-position)，position = target + q⁻¹·(0,0,d)
    // 展开后两者逐项相同，所以画面表现完全一致 —— 只是状态的归属换了地方。
}

void OrbitCameraController::Orbit(const glm::quat& delta)
{
    m_rotation = glm::normalize(delta * m_rotation);
    SyncCamera();
}

void OrbitCameraController::Zoom(float factor)
{
    if (!(factor > 0.0f))
    {
        // 防御 0 / 负数 / NaN：一旦让 m_distance 变成 0 或 NaN，
        // view 矩阵就全废了，而且之后每一帧都是废的。
        // 写成 !(x > 0) 而不是 x <= 0 是为了同时拦住 NaN。
        return;
    }
    SetDistance(m_distance * factor);
}

void OrbitCameraController::Pan(float dxNdc, float dyNdc)
{
    // 平移量按"目标平面上的可见范围"缩放，于是「抓住画面拖动」的手感
    // 在任何距离下都一致。
    //
    // 这里是新抽象最直接的收益：换算量向 Camera 询问，而不是自己算
    // distance * tan(fovY/2) —— 控制器因此完全不需要知道相机是透视还是正交，
    // 将来接上正交相机时这段代码一行都不用改。
    const float halfHeight = m_camera.HalfExtentAtDistance(m_distance);
    const float worldX     = dxNdc * halfHeight * m_camera.Aspect();
    const float worldY     = dyNdc * halfHeight;

    // 直接问相机要世界空间的右/上方向。因为每次改动后都 SyncCamera 过，
    // 相机的朝向必然与 m_rotation 一致，不存在读到过期值的问题。
    const glm::vec3 right = m_camera.Right();
    const glm::vec3 up    = m_camera.Up();

    // 减号：拖动是「抓着场景走」，目标点要朝相反方向移动，
    // 画面里的物体才会跟着手指同向移动。
    m_target -= right * worldX + up * worldY;
    SyncCamera();
}

void OrbitCameraController::Reset()
{
    m_rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    m_target   = glm::vec3(0.0f);
    m_distance = std::clamp(3.0f, m_minDistance, m_maxDistance);
    SyncCamera();
}

void OrbitCameraController::SetTarget(const glm::vec3& target)
{
    m_target = target;
    SyncCamera();
}

void OrbitCameraController::SetDistance(float distance)
{
    m_distance = std::clamp(distance, m_minDistance, m_maxDistance);
    SyncCamera();
}

void OrbitCameraController::SetDistanceLimits(float minDistance, float maxDistance)
{
    m_minDistance = std::max(0.01f, minDistance);
    m_maxDistance = std::max(m_minDistance + 0.01f, maxDistance);
    SetDistance(m_distance);  // 夹一次，顺带 SyncCamera
}

void OrbitCameraController::FrameBounds(const glm::vec3& center, float radius)
{
    const float r = std::max(radius, 1e-3f);

    // 距离取"让包围球正好内切于竖直视野"再留 1.6 倍余量。
    //
    // 用 HalfExtentAtDistance 反解而不是写死 r / tan(fov/2)：
    // 该函数在距离 1 处返回的就是 tan(fovY/2)，于是 r / 它 即为所需距离。
    // 正交相机下它是常量，反解出的距离与 r 无关 —— 正是正交投影该有的行为。
    const float halfExtentAtUnit = m_camera.HalfExtentAtDistance(1.0f);
    const float fitDistance      = (halfExtentAtUnit > 1e-6f) ? (r / halfExtentAtUnit) : (r * 3.0f);

    m_target = center;
    SetDistanceLimits(r * 0.1f, r * 50.0f);
    SetDistance(fitDistance * 1.6f);
}

// =============================================================================
// arcball 数学
// =============================================================================

glm::vec3 OrbitCameraController::ProjectToSphere(const glm::vec2& ndc)
{
    const float d2 = ndc.x * ndc.x + ndc.y * ndc.y;
    if (d2 <= 1.0f)
    {
        // 球内：直接抬到球面上
        return glm::vec3(ndc.x, ndc.y, std::sqrt(1.0f - d2));
    }
    // 球外：投到赤道圈上（Shoemake 的做法）。
    // 好处是视口角落依然可用，并且得到的是绕视线方向的滚转，
    // 而不是像「夹取到球心」那样在边缘突然没有响应。
    const float len = std::sqrt(d2);
    return glm::vec3(ndc.x / len, ndc.y / len, 0.0f);
}

glm::quat OrbitCameraController::ArcballDelta(const glm::vec2& fromNdc,
                                              const glm::vec2& toNdc) const
{
    const glm::vec3 a = ProjectToSphere(fromNdc);
    const glm::vec3 b = ProjectToSphere(toNdc);

    glm::vec3 axis = glm::cross(a, b);
    if (glm::dot(axis, axis) < 1e-12f)
    {
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);  // 同向或反向，无有效旋转轴
    }

    const float cosAngle = glm::clamp(glm::dot(a, b), -1.0f, 1.0f);
    const float angle    = std::acos(cosAngle) * rotateSpeed;
    return glm::angleAxis(angle, glm::normalize(axis));
}

void OrbitCameraController::ApplyRotate(const glm::vec2& fromNdc, const glm::vec2& toNdc)
{
    glm::vec2 from = fromNdc;
    glm::vec2 to   = toNdc;
    if (invertY)
    {
        from.y = -from.y;
        to.y   = -to.y;
    }
    if (glm::dot(to - from, to - from) < kMinDragNdc * kMinDragNdc)
    {
        return;
    }
    Orbit(ArcballDelta(from, to));
}

// =============================================================================
// 事件分发
// =============================================================================

bool OrbitCameraController::HandleEvent(const SDL_Event& event)
{
    switch (event.type)
    {
        // ---- 鼠标 ----------------------------------------------------------
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        {
            // 关键一处：SDL 默认开启 SDL_HINT_TOUCH_MOUSE_EVENTS，
            // 触摸会**额外**合成一份鼠标事件。若不在这里滤掉，
            // Android 上单指拖动会被「触摸路径」和「鼠标路径」各处理一次，
            // 旋转速度翻倍，而且双指手势会和合成的鼠标拖动打架。
            if (event.button.which == SDL_TOUCH_MOUSEID)
            {
                return false;
            }
            const bool down = (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            if (event.button.button == SDL_BUTTON_LEFT)
            {
                m_mouseRotating = down;
            }
            else if (event.button.button == SDL_BUTTON_RIGHT ||
                     event.button.button == SDL_BUTTON_MIDDLE)
            {
                m_mousePanning = down;
            }
            else
            {
                return false;
            }
            if (down)
            {
                m_lastDragNdc = ToNdc(event.button.x, event.button.y);
            }
            return true;
        }

        case SDL_EVENT_MOUSE_MOTION:
        {
            if (event.motion.which == SDL_TOUCH_MOUSEID)
            {
                return false;
            }
            if (!m_mouseRotating && !m_mousePanning)
            {
                return false;
            }
            const glm::vec2 ndc = ToNdc(event.motion.x, event.motion.y);
            if (m_mouseRotating)
            {
                ApplyRotate(m_lastDragNdc, ndc);
            }
            else
            {
                const glm::vec2 d = ndc - m_lastDragNdc;
                Pan(d.x * panSpeed, d.y * panSpeed);
            }
            m_lastDragNdc = ndc;
            return true;
        }

        case SDL_EVENT_MOUSE_WHEEL:
        {
            if (event.wheel.which == SDL_TOUCH_MOUSEID)
            {
                return false;
            }
            // 向前滚（y > 0）拉近。用指数而不是线性：连续滚动时
            // 每一格的观感变化相同，且永远不会越过目标点。
            Zoom(std::exp(-event.wheel.y * kWheelZoomStep * zoomSpeed));
            return true;
        }

        // ---- 触摸 ----------------------------------------------------------
        case SDL_EVENT_FINGER_DOWN:
            OnFingerDown(event.tfinger);
            return true;

        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
            // FINGER_CANCELED 不能漏（来电、通知栏下拉等会触发）。
            // 漏了这个分支，手指状态会一直留在按下态，回到应用后第一次触摸就跳变。
            OnFingerUp(event.tfinger);
            return true;

        case SDL_EVENT_FINGER_MOTION:
            OnFingerMotion(event.tfinger);
            return true;

        default:
            return false;
    }
}

void OrbitCameraController::ResetInputState()
{
    m_mouseRotating = false;
    m_mousePanning  = false;
    for (Finger& f : m_fingers)
    {
        f.active = false;
    }
    m_pinchActive = false;
}

// =============================================================================
// 触摸手势
// =============================================================================

int OrbitCameraController::ActiveFingerCount() const
{
    int n = 0;
    for (const Finger& f : m_fingers)
    {
        if (f.active)
        {
            ++n;
        }
    }
    return n;
}

int OrbitCameraController::FindFinger(SDL_FingerID id) const
{
    for (int i = 0; i < static_cast<int>(m_fingers.size()); ++i)
    {
        if (m_fingers[i].active && m_fingers[i].id == id)
        {
            return i;
        }
    }
    return -1;
}

void OrbitCameraController::BeginPinch()
{
    // 记录双指手势的基准量。每次手指数变成 2 时都要重新记一次，
    // 否则第二根手指刚落下的瞬间会拿旧基准算出一个巨大的缩放跳变。
    const glm::vec2 a   = m_fingers[0].ndc;
    const glm::vec2 b   = m_fingers[1].ndc;
    m_lastPinchDistance = glm::length(b - a);
    m_lastPinchCenter   = (a + b) * 0.5f;
    m_pinchActive       = true;
}

void OrbitCameraController::OnFingerDown(const SDL_TouchFingerEvent& e)
{
    const glm::vec2 ndc = TouchToNdc(e.x, e.y);

    for (Finger& f : m_fingers)
    {
        if (!f.active)
        {
            f.active = true;
            f.id     = e.fingerID;
            f.ndc    = ndc;
            break;
        }
    }
    if (ActiveFingerCount() == 2)
    {
        BeginPinch();
    }
}

void OrbitCameraController::OnFingerUp(const SDL_TouchFingerEvent& e)
{
    const int idx = FindFinger(e.fingerID);
    if (idx >= 0)
    {
        m_fingers[idx].active = false;
    }

    if (ActiveFingerCount() < 2)
    {
        m_pinchActive = false;
    }
    // 双指变单指时，把剩下那根手指的位置作为新的旋转基准 ——
    // 不重设的话，松开一根手指的瞬间会以另一根手指的**旧**位置为起点，
    // 产生一次突兀的旋转。
    if (ActiveFingerCount() == 1)
    {
        for (const Finger& f : m_fingers)
        {
            if (f.active)
            {
                m_lastDragNdc = f.ndc;
                break;
            }
        }
    }
}

void OrbitCameraController::OnFingerMotion(const SDL_TouchFingerEvent& e)
{
    const int idx = FindFinger(e.fingerID);
    if (idx < 0)
    {
        return;  // 不是我们跟踪的前两根手指
    }

    const glm::vec2 prev = m_fingers[idx].ndc;
    const glm::vec2 cur  = TouchToNdc(e.x, e.y);
    m_fingers[idx].ndc   = cur;

    const int n = ActiveFingerCount();

    if (n == 1)
    {
        // 单指：旋转
        ApplyRotate(prev, cur);
        return;
    }

    if (n == 2 && m_pinchActive)
    {
        const glm::vec2 a = m_fingers[0].ndc;
        const glm::vec2 b = m_fingers[1].ndc;

        const float     dist   = glm::length(b - a);
        const glm::vec2 center = (a + b) * 0.5f;

        // 捏合 -> 缩放。取比值：手指分开 dist 变大，应该拉近，所以用 last/cur。
        // 加个下限防止两指重合时除零。
        if (m_lastPinchDistance > 1e-4f && dist > 1e-4f)
        {
            const float ratio = m_lastPinchDistance / dist;
            // 用 pow 把灵敏度作用在比例上，而不是直接乘 zoomSpeed ——
            // 直接乘会破坏「比例为 1 时不缩放」这个不变量。
            Zoom(std::pow(ratio, zoomSpeed));
        }

        // 双指整体位移 -> 平移
        const glm::vec2 d = center - m_lastPinchCenter;
        Pan(d.x * panSpeed, d.y * panSpeed);

        m_lastPinchDistance = dist;
        m_lastPinchCenter   = center;
    }
}

}  // namespace origin

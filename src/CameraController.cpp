#include "CameraController.hpp"

#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_touch.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace origin {
namespace {

// 缩放灵敏度：滚轮一格对应的距离比例。
// 用比例而非固定步长，保证远近处的手感一致。
constexpr float kWheelZoomStep = 0.12f;

// 低于这个位移就不产生旋转。抖动过滤，同时避免在 arcball 里对零向量做 normalize。
constexpr float kMinDragNdc = 1e-5f;

}  // namespace

// =============================================================================
// 坐标与 arcball 数学
// =============================================================================

glm::vec2 CameraController::toNdc(float windowX, float windowY) const {
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    if (w <= 0 || h <= 0) {
        return glm::vec2(0.0f);
    }
    // 注意用 SDL_GetWindowSize（逻辑坐标）而不是 SDL_GetWindowSizeInPixels：
    // 鼠标事件给的就是逻辑坐标，高 DPI 下两者不相等，混用会让灵敏度偏掉。
    //
    // Y 取反：窗口坐标原点在左上、向下为正，而我们要的 NDC 是向上为正，
    // 这样向上拖动才会让物体向上翻。
    return glm::vec2(2.0f * windowX / static_cast<float>(w) - 1.0f,
                     1.0f - 2.0f * windowY / static_cast<float>(h));
}

glm::vec3 CameraController::projectToSphere(const glm::vec2& ndc) {
    const float d2 = ndc.x * ndc.x + ndc.y * ndc.y;
    if (d2 <= 1.0f) {
        // 球内：直接抬到球面上
        return glm::vec3(ndc.x, ndc.y, std::sqrt(1.0f - d2));
    }
    // 球外：投到赤道圈上（Shoemake 的做法）。
    // 好处是视口角落依然可用，并且得到的是绕视线方向的滚转，
    // 而不是像「夹取到球心」那样在边缘突然没有响应。
    const float len = std::sqrt(d2);
    return glm::vec3(ndc.x / len, ndc.y / len, 0.0f);
}

glm::quat CameraController::arcballDelta(const glm::vec2& fromNdc,
                                         const glm::vec2& toNdc) const {
    const glm::vec3 a = projectToSphere(fromNdc);
    const glm::vec3 b = projectToSphere(toNdc);

    glm::vec3 axis = glm::cross(a, b);
    if (glm::dot(axis, axis) < 1e-12f) {
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);  // 同向或反向，无有效旋转轴
    }

    const float cosAngle = glm::clamp(glm::dot(a, b), -1.0f, 1.0f);
    const float angle    = std::acos(cosAngle) * rotateSpeed;
    return glm::angleAxis(angle, glm::normalize(axis));
}

void CameraController::applyRotate(const glm::vec2& fromNdc, const glm::vec2& toNdc) {
    glm::vec2 from = fromNdc;
    glm::vec2 to   = toNdc;
    if (invertY) {
        from.y = -from.y;
        to.y   = -to.y;
    }
    if (glm::dot(to - from, to - from) < kMinDragNdc * kMinDragNdc) {
        return;
    }
    camera_.orbit(arcballDelta(from, to));
}

// =============================================================================
// 事件分发
// =============================================================================

bool CameraController::handleEvent(const SDL_Event& event) {
    switch (event.type) {
        // ---- 鼠标 ----------------------------------------------------------
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            // 关键一处：SDL 默认开启 SDL_HINT_TOUCH_MOUSE_EVENTS，
            // 触摸会**额外**合成一份鼠标事件。若不在这里滤掉，
            // Android 上单指拖动会被「触摸路径」和「鼠标路径」各处理一次，
            // 旋转速度翻倍，而且双指手势会和合成的鼠标拖动打架。
            if (event.button.which == SDL_TOUCH_MOUSEID) {
                return false;
            }
            const bool down = (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            if (event.button.button == SDL_BUTTON_LEFT) {
                mouseRotating_ = down;
            } else if (event.button.button == SDL_BUTTON_RIGHT ||
                       event.button.button == SDL_BUTTON_MIDDLE) {
                mousePanning_ = down;
            } else {
                return false;
            }
            if (down) {
                lastMouseNdc_ = toNdc(event.button.x, event.button.y);
            }
            return true;
        }

        case SDL_EVENT_MOUSE_MOTION: {
            if (event.motion.which == SDL_TOUCH_MOUSEID) {
                return false;
            }
            if (!mouseRotating_ && !mousePanning_) {
                return false;
            }
            const glm::vec2 ndc = toNdc(event.motion.x, event.motion.y);
            if (mouseRotating_) {
                applyRotate(lastMouseNdc_, ndc);
            } else {
                const glm::vec2 d = ndc - lastMouseNdc_;
                camera_.pan(d.x * panSpeed, d.y * panSpeed);
            }
            lastMouseNdc_ = ndc;
            return true;
        }

        case SDL_EVENT_MOUSE_WHEEL: {
            if (event.wheel.which == SDL_TOUCH_MOUSEID) {
                return false;
            }
            // 向前滚（y > 0）拉近。用指数而不是线性：连续滚动时
            // 每一格的观感变化相同，且永远不会越过目标点。
            camera_.zoom(std::exp(-event.wheel.y * kWheelZoomStep * zoomSpeed));
            return true;
        }

        // ---- 触摸 ----------------------------------------------------------
        case SDL_EVENT_FINGER_DOWN:
            onFingerDown(event.tfinger);
            return true;

        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
            // FINGER_CANCELED 不能漏（来电、通知栏下拉等会触发）。
            // 漏了这个分支，手指状态会一直留在按下态，回到应用后第一次触摸就跳变。
            onFingerUp(event.tfinger);
            return true;

        case SDL_EVENT_FINGER_MOTION:
            onFingerMotion(event.tfinger);
            return true;

        default:
            return false;
    }
}

void CameraController::resetInputState() {
    mouseRotating_ = false;
    mousePanning_  = false;
    for (Finger& f : fingers_) {
        f.active = false;
    }
    pinchActive_ = false;
}

// =============================================================================
// 触摸手势
// =============================================================================

int CameraController::activeFingerCount() const {
    int n = 0;
    for (const Finger& f : fingers_) {
        if (f.active) {
            ++n;
        }
    }
    return n;
}

int CameraController::findFinger(SDL_FingerID id) const {
    for (int i = 0; i < static_cast<int>(fingers_.size()); ++i) {
        if (fingers_[i].active && fingers_[i].id == id) {
            return i;
        }
    }
    return -1;
}

void CameraController::beginPinch() {
    // 记录双指手势的基准量。每次手指数变成 2 时都要重新记一次，
    // 否则第二根手指刚落下的瞬间会拿旧基准算出一个巨大的缩放跳变。
    const glm::vec2 a = fingers_[0].ndc;
    const glm::vec2 b = fingers_[1].ndc;
    lastPinchDistance_ = glm::length(b - a);
    lastPinchCenter_   = (a + b) * 0.5f;
    pinchActive_       = true;
}

void CameraController::onFingerDown(const SDL_TouchFingerEvent& e) {
    // tfinger 的 x/y 已经是归一化到 0..1 的，不需要查窗口尺寸，
    // 直接线性映射到 [-1,1]（Y 同样要翻转）。
    const glm::vec2 ndc(2.0f * e.x - 1.0f, 1.0f - 2.0f * e.y);

    for (Finger& f : fingers_) {
        if (!f.active) {
            f.active = true;
            f.id     = e.fingerID;
            f.ndc    = ndc;
            break;
        }
    }
    if (activeFingerCount() == 2) {
        beginPinch();
    }
}

void CameraController::onFingerUp(const SDL_TouchFingerEvent& e) {
    const int idx = findFinger(e.fingerID);
    if (idx >= 0) {
        fingers_[idx].active = false;
    }

    if (activeFingerCount() < 2) {
        pinchActive_ = false;
    }
    // 双指变单指时，把剩下那根手指的位置作为新的旋转基准 ——
    // 不重设的话，松开一根手指的瞬间会以另一根手指的**旧**位置为起点，
    // 产生一次突兀的旋转。
    if (activeFingerCount() == 1) {
        for (const Finger& f : fingers_) {
            if (f.active) {
                lastMouseNdc_ = f.ndc;
                break;
            }
        }
    }
}

void CameraController::onFingerMotion(const SDL_TouchFingerEvent& e) {
    const int idx = findFinger(e.fingerID);
    if (idx < 0) {
        return;  // 不是我们跟踪的前两根手指
    }

    const glm::vec2 prev = fingers_[idx].ndc;
    const glm::vec2 cur(2.0f * e.x - 1.0f, 1.0f - 2.0f * e.y);
    fingers_[idx].ndc = cur;

    const int n = activeFingerCount();

    if (n == 1) {
        // 单指：旋转
        applyRotate(prev, cur);
        return;
    }

    if (n == 2 && pinchActive_) {
        const glm::vec2 a = fingers_[0].ndc;
        const glm::vec2 b = fingers_[1].ndc;

        const float     dist   = glm::length(b - a);
        const glm::vec2 center = (a + b) * 0.5f;

        // 捏合 -> 缩放。取比值：手指分开 dist 变大，应该拉近，所以用 last/cur。
        // 加个下限防止两指重合时除零。
        if (lastPinchDistance_ > 1e-4f && dist > 1e-4f) {
            const float ratio = lastPinchDistance_ / dist;
            // 用 pow 把灵敏度作用在比例上，而不是直接乘 zoomSpeed ——
            // 直接乘会破坏「比例为 1 时不缩放」这个不变量。
            camera_.zoom(std::pow(ratio, zoomSpeed));
        }

        // 双指整体位移 -> 平移
        const glm::vec2 d = center - lastPinchCenter_;
        camera_.pan(d.x * panSpeed, d.y * panSpeed);

        lastPinchDistance_ = dist;
        lastPinchCenter_   = center;
    }
}

}  // namespace origin

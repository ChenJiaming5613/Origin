#include "Camera.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

namespace origin {

glm::mat4 Camera::proj() const {
    glm::mat4 p = glm::perspective(glm::radians(fovYDeg_), aspect_, nearZ_, farZ_);
    // glm 只负责深度范围（由 GLM_FORCE_DEPTH_ZERO_TO_ONE 控制），不管 Y 方向。
    // Vulkan 的 NDC 里 Y 朝下，不翻这一下画面就是上下颠倒的。
    p[1][1] *= -1.0f;
    return p;
}

glm::mat4 Camera::view() const {
    // 从右往左读：先把目标点移到原点，再施加累积的 arcball 旋转，
    // 最后把整个场景沿 -Z 推开 distance —— 相机固定在原点朝 -Z 看。
    return glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -distance_)) *
           glm::mat4_cast(orientation_) *
           glm::translate(glm::mat4(1.0f), -target_);
}

glm::vec3 Camera::position() const {
    // 相机在视图空间的原点，逆变换回世界空间即可。
    // 这里不直接写 inverse(view())：旋转部分用共轭求逆更便宜也更精确。
    const glm::quat invRot = glm::conjugate(orientation_);
    return target_ + invRot * glm::vec3(0.0f, 0.0f, distance_);
}

void Camera::orbit(const glm::quat& delta) {
    orientation_ = glm::normalize(delta * orientation_);
}

void Camera::zoom(float factor) {
    if (!(factor > 0.0f)) {
        return;  // 防御 0 或 NaN：一旦让 distance_ 变成 0/NaN，view 矩阵就全废了
    }
    setDistance(distance_ * factor);
}

void Camera::pan(float dxNdc, float dyNdc) {
    // 平移量随距离缩放：拉远后同样的拖动幅度移动更多世界单位，
    // 这样「抓住画面拖动」的手感在任何距离下都一致。
    // 乘 tan(fovY/2) 是为了让 NDC 位移严格对应到目标平面上的实际距离。
    const float halfHeight = distance_ * std::tan(glm::radians(fovYDeg_) * 0.5f);
    const float worldX     = dxNdc * halfHeight * aspect_;
    const float worldY     = dyNdc * halfHeight;

    // 屏幕右/上方向在世界空间中的朝向。orientation_ 作用于物体，
    // 所以它的共轭才是把视图空间的轴变换回世界空间的那个旋转。
    const glm::quat invRot = glm::conjugate(orientation_);
    const glm::vec3 right  = invRot * glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 up     = invRot * glm::vec3(0.0f, 1.0f, 0.0f);

    // 减号：拖动是「抓着场景走」，目标点要朝相反方向移动，
    // 画面里的物体才会跟着手指同向移动。
    target_ -= right * worldX + up * worldY;
}

void Camera::reset() {
    orientation_ = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    target_      = glm::vec3(0.0f);
    distance_    = 3.0f;
}

void Camera::setDistance(float d) {
    distance_ = std::clamp(d, minDistance_, maxDistance_);
}

void Camera::setDistanceLimits(float minD, float maxD) {
    minDistance_ = std::max(0.01f, minD);
    maxDistance_ = std::max(minDistance_ + 0.01f, maxD);
    setDistance(distance_);
}

}  // namespace origin

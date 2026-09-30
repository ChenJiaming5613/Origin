#include "Camera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace origin
{
namespace
{

// 视图空间的三个基轴。单独列出来是为了让 Forward/Right/Up 的实现
// 一眼能对上"+X 右、+Y 上、-Z 前"这个约定。
constexpr glm::vec3 kViewRight   = glm::vec3(1.0f, 0.0f, 0.0f);
constexpr glm::vec3 kViewUp      = glm::vec3(0.0f, 1.0f, 0.0f);
constexpr glm::vec3 kViewForward = glm::vec3(0.0f, 0.0f, -1.0f);

}  // namespace

void Camera::SetOrientation(const glm::quat& orientation)
{
    // 每次都归一化：控制器会连续左乘旋转增量，浮点误差会累积，
    // 几千帧之后四元数的模会偏离 1，表现为画面被轻微缩放/剪切。
    const float len2 = glm::dot(orientation, orientation);
    if (len2 > 1e-12f)
    {
        m_orientation = orientation / std::sqrt(len2);
    }
    // 传入零四元数时保持原值不变 —— 写进去会让 View() 整个变成 NaN，
    // 那之后每一帧都是黑屏，且很难回溯到是哪次赋值造成的。
}

glm::vec3 Camera::Forward() const
{
    // m_orientation 是 world -> view，所以它的共轭才是把视图空间的轴
    // 变换回世界空间的那个旋转。用共轭而不是 inverse()：单位四元数下
    // 两者相等，但共轭不需要除以模，更便宜也更精确。
    return glm::conjugate(m_orientation) * kViewForward;
}

glm::vec3 Camera::Right() const
{
    return glm::conjugate(m_orientation) * kViewRight;
}

glm::vec3 Camera::Up() const
{
    return glm::conjugate(m_orientation) * kViewUp;
}

void Camera::LookAt(const glm::vec3& eye, const glm::vec3& target, const glm::vec3& up)
{
    m_position = eye;

    // 退化检查放在调 glm::lookAt **之前**：lookAt 在 eye==target 时会对
    // 零向量做 normalize，产出全 NaN 的矩阵，而 NaN 一旦写进 m_orientation
    // 就再也恢复不了（后续任何旋转增量乘上它仍是 NaN）。
    const glm::vec3 toTarget = target - eye;
    if (glm::dot(toTarget, toTarget) < 1e-12f)
    {
        return;
    }
    // up 与视线共线时叉积为零，同样会 NaN。
    const glm::vec3 cross = glm::cross(toTarget, up);
    if (glm::dot(cross, cross) < 1e-12f)
    {
        return;
    }

    // glm::lookAt 给出的是完整的 view 矩阵：view = R * T(-eye)。
    // 取它的 3x3 部分就是 world -> view 的旋转，正好是 m_orientation 的语义。
    m_orientation = glm::normalize(glm::quat_cast(glm::mat3(glm::lookAt(eye, target, up))));
}

void Camera::SetAspect(float aspect)
{
    // 忽略非正值而不是 clamp 到极小值：窗口最小化时 SDL 会报 0 尺寸，
    // 此时保持上一帧的宽高比，恢复窗口后画面不会先闪一下畸变的一帧。
    if (aspect > 0.0f)
    {
        m_aspect = aspect;
    }
}

void Camera::SetClipPlanes(float nearZ, float farZ)
{
    // near 必须严格大于 0（透视投影要除以它），且 far > near。
    // 不做这层保护的话，glm::perspective 会产出含 inf 的矩阵，
    // 症状是深度测试全部失效，看起来像是深度缓冲没绑上。
    m_nearZ = std::max(1e-4f, nearZ);
    m_farZ  = std::max(m_nearZ * 1.001f, farZ);
}

glm::mat4 Camera::View() const
{
    // view = R(world->view) * T(-position)。
    // 从右往左读：先把相机挪到原点，再把世界旋转到相机的朝向。
    return glm::mat4_cast(m_orientation) * glm::translate(glm::mat4(1.0f), -m_position);
}

glm::mat4 Camera::Proj() const
{
    glm::mat4 p = ComputeProjection();
    // Vulkan 的 NDC 里 Y 朝下，而 glm 产出的是 Y 朝上的 GL 约定。
    // 深度范围不用管 —— GLM_FORCE_DEPTH_ZERO_TO_ONE 已经处理好了。
    //
    // 这一行是整个工程里唯一做 Y 翻转的地方，见头文件里为什么不放子类。
    p[1][1] *= -1.0f;
    return p;
}

}  // namespace origin

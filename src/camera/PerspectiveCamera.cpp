#include "PerspectiveCamera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace origin
{

void PerspectiveCamera::SetFovY(float fovYDegrees)
{
    m_fovYDeg = std::clamp(fovYDegrees, 1.0f, 179.0f);
}

float PerspectiveCamera::HalfExtentAtDistance(float distance) const
{
    return distance * std::tan(glm::radians(m_fovYDeg) * 0.5f);
}

glm::mat4 PerspectiveCamera::ComputeProjection() const
{
    // 标准 GL 风格的透视矩阵，Y 朝上。
    // 深度范围 [0,1] 由 GLM_FORCE_DEPTH_ZERO_TO_ONE 保证（CMake 统一传播）。
    // Vulkan 需要的 Y 翻转**不在这里做** —— 基类 Camera::Proj() 统一处理。
    //
    // 注意这里是常规深度（近处 0、远处 1）。若日后改用 reverse-Z
    // （精度分布更好），除了交换 near/far，还要把 pipeline 的
    // depthCompareOp 从 LESS 换成 GREATER，两处必须一起改。
    return glm::perspective(glm::radians(m_fovYDeg), Aspect(), NearZ(), FarZ());
}

}  // namespace origin

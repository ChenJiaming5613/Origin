#pragma once

#include "Camera.h"

namespace origin
{

// 透视投影相机。相对基类只多一个状态：垂直视场角。
//
// 水平视场角不单独存 —— 它由 fovY 与 aspect 推出来。存两份必然会不一致，
// 而"改了宽高比忘了同步水平 fov"这种 bug 表现为画面轻微拉伸，极难察觉。
class PerspectiveCamera : public Camera
{
public:
    PerspectiveCamera() = default;
    explicit PerspectiveCamera(float fovYDegrees) { SetFovY(fovYDegrees); }

    // 合法区间被限制在 (0, 179) 内。
    // fovY 趋于 0 或 180 时 tan(fovY/2) 分别趋于 0 和 inf，
    // 投影矩阵会退化成全零或含 inf —— 画面直接消失，而参数看起来"只是有点极端"。
    void  SetFovY(float fovYDegrees);
    float FovY() const { return m_fovYDeg; }

    // distance * tan(fovY/2)：透视相机的可见范围随距离线性增长。
    // 这正是"拉远后同样的拖动幅度移动更多世界单位"这一手感的来源。
    float HalfExtentAtDistance(float distance) const override;

protected:
    glm::mat4 ComputeProjection() const override;

private:
    float m_fovYDeg = 60.0f;
};

}  // namespace origin

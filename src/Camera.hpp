#pragma once

// GLM_FORCE_DEPTH_ZERO_TO_ONE / GLM_FORCE_RADIANS 由 CMake 的 glm target 统一传播，
// 不要在这里重复 define（两处写得不一致会让同一个 glm::mat4 在不同 TU 里语义不同）。
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace origin {

// 轨道相机（orbit / arcball camera）。
//
// 只做数学，不含任何输入处理、不含任何 Vulkan 调用 —— 输入交给
// CameraController，这样两者可以各自单独测试与替换。
//
// 状态用「目标点 + 朝向四元数 + 距离」而不是常见的 yaw/pitch 两个欧拉角：
//   - arcball 的自然输出就是一个任意轴的旋转增量，四元数可以直接左乘累积，
//     用欧拉角则必须先把增量拆成 yaw/pitch，斜向拖动会失真；
//   - 不存在万向锁，可以越过天顶继续转（欧拉角方案在 pitch 接近 ±90° 时会卡住）。
//
// 语义约定：orientation_ 表示**施加在物体上**的旋转，不是相机自身的朝向。
// 于是 view = T(0,0,-distance) * R(orientation) * T(-target)，
// 视觉效果是 cube 跟着手指/鼠标直接转，符合 arcball 的直觉。
// （若约定成相机朝向，就要到处取共轭，反而容易写错方向。）
class Camera {
public:
    // ---- 投影 --------------------------------------------------------------

    // aspect 由调用方给出「可见区域」的宽高比。
    // 之所以不让 Camera 自己去读 swapchain 尺寸：Android 走 pre-rotation 路径时
    // framebuffer 是设备 identity 朝向的，可见宽高比与 framebuffer 尺寸并不相同，
    // 那段判断属于渲染器的职责。
    void setAspect(float aspect) { aspect_ = aspect > 0.0f ? aspect : 1.0f; }

    void setPerspective(float fovYDeg, float nearZ, float farZ) {
        fovYDeg_ = fovYDeg;
        nearZ_   = nearZ;
        farZ_    = farZ;
    }

    // 已包含 Vulkan 需要的 Y 翻转（NDC 里 Y 朝下）。
    // 深度范围 [0,1] 由 GLM_FORCE_DEPTH_ZERO_TO_ONE 保证。
    // 注意：Android 的 pre-rotation 旋转**不**在这里做 —— 那是呈现层的事，
    // 由渲染器在外面左乘。
    glm::mat4 proj() const;

    // ---- 视图 --------------------------------------------------------------

    glm::mat4 view() const;
    glm::mat4 viewProj() const { return proj() * view(); }

    // 相机在世界空间中的位置（后面加高光/天空盒时会用到）
    glm::vec3 position() const;

    // ---- 操作（由 CameraController 驱动）-----------------------------------

    // 左乘一个旋转增量。左乘而非右乘：增量是在**当前**屏幕空间里量出来的，
    // 必须作用在已累积的朝向之外侧，否则拖动方向会随已有旋转而漂移。
    void orbit(const glm::quat& delta);

    // 相对缩放。用乘法而不是加减：等量的手势在远近处产生的观感变化才一致
    // （加法在靠近目标时会显得过于灵敏）。
    void zoom(float factor);

    // 沿相机的右/上方向平移目标点。dx/dy 是 NDC 位移量，
    // 内部按当前距离换算成世界单位，于是拉远后同样的拖动会移动更多 —— 与观感一致。
    void pan(float dxNdc, float dyNdc);

    void reset();

    // ---- 访问器（面板显示用）------------------------------------------------

    float            distance() const { return distance_; }
    const glm::vec3& target() const { return target_; }
    const glm::quat& orientation() const { return orientation_; }

    void setDistance(float d);
    void setDistanceLimits(float minD, float maxD);

private:
    glm::quat orientation_ = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);  // 单位四元数
    glm::vec3 target_      = glm::vec3(0.0f);
    float     distance_    = 3.0f;

    float minDistance_ = 0.5f;
    float maxDistance_ = 50.0f;

    float aspect_  = 1.0f;
    float fovYDeg_ = 60.0f;
    float nearZ_   = 0.1f;
    float farZ_    = 100.0f;
};

}  // namespace origin

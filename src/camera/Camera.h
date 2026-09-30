#pragma once

// GLM_FORCE_DEPTH_ZERO_TO_ONE / GLM_FORCE_RADIANS 由 CMake 的 glm target 统一传播，
// 不要在这里重复 define（两处写得不一致会让同一个 glm::mat4 在不同 TU 里语义不同）。
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace origin
{

// 相机基类。只回答三个问题：我在哪、朝哪看、怎么投影 —— 最终产物是 ViewProj()。
//
// 状态刻意只保留**所有**相机类型共有的量：位姿 + 宽高比 + 裁剪面。
// 两类东西被有意排除在外：
//
//   1. fovY 只属于透视相机、orthoHeight 只属于正交相机 —— 放各自的子类。
//   2. target / distance 是**轨道控制**的参数化方式，不是相机的固有属性 ——
//      放 OrbitCameraController。这样将来加 FPS 控制器（位置 + yaw/pitch）
//      或做相机动画（直接喂 position/orientation）时，相机类一行都不用改。
//      旧版实现把 target/distance/orbit/zoom/pan 全塞进 Camera，结果是
//      "相机"这个概念被轨道交互绑死了。
//
// 位姿用 position + orientation 而不是 lookAt 的 (eye, center, up) 三向量：
// 它直接对应 view 矩阵的两个组成部分，不需要每帧重新正交化，
// 也不会出现 up 与视线共线时的退化。需要 lookAt 语义时用下面的 LookAt()。
class Camera
{
public:
    virtual ~Camera() = default;

    Camera()                         = default;
    Camera(const Camera&)            = default;
    Camera& operator=(const Camera&) = default;

    // ---- 位姿 --------------------------------------------------------------

    // ⚠️ orientation 的语义是 **world -> view** 的旋转，不是"相机自身的朝向"。
    //
    // 选这个方向是因为 View() 可以直接 mat4_cast 而不必取共轭 —— view 矩阵
    // 每帧都要算，而"相机前方"这类查询只在少数地方用。代价是 Forward/Right/Up
    // 内部要取共轭，那三个函数已经封装好了，调用方不必关心。
    void SetPosition(const glm::vec3& position) { m_position = position; }
    void SetOrientation(const glm::quat& orientation);

    const glm::vec3& Position() const { return m_position; }
    const glm::quat& Orientation() const { return m_orientation; }

    // 相机坐标系三轴在**世界空间**中的方向。
    // 视图空间约定与 glm / OpenGL 一致：+X 右、+Y 上、**-Z 前**。
    glm::vec3 Forward() const;
    glm::vec3 Right() const;
    glm::vec3 Up() const;

    // 便捷设置：效果等同 glm::lookAt，但结果存成 position + orientation。
    // eye 与 target 重合（或 up 与视线共线）时保持原朝向不变，不会写入 NaN。
    void LookAt(const glm::vec3& eye, const glm::vec3& target,
                const glm::vec3& up = glm::vec3(0.0f, 1.0f, 0.0f));

    // ---- 投影参数（共有部分）-----------------------------------------------

    // aspect 由调用方给出「可见区域」的宽高比。
    //
    // 之所以不让相机自己去读 swapchain 尺寸：Android 走 pre-rotation 路径时
    // framebuffer 是设备 identity 朝向的，可见宽高比与 framebuffer 尺寸并不相同，
    // 那段判断属于渲染器的职责。非正数输入会被忽略（退回 1.0），
    // 因为窗口最小化时 SDL 可能报 0 尺寸。
    void  SetAspect(float aspect);
    float Aspect() const { return m_aspect; }

    void  SetClipPlanes(float nearZ, float farZ);
    float NearZ() const { return m_nearZ; }
    float FarZ() const { return m_farZ; }

    // ---- 矩阵 --------------------------------------------------------------

    glm::mat4 View() const;

    // 非虚 —— 子类要改的是 protected 的 ComputeProjection()。
    //
    // 这层包装的唯一目的是把 Vulkan 的 Y 翻转收在**一个**地方：
    // glm 只负责深度范围（GLM_FORCE_DEPTH_ZERO_TO_ONE，由 CMake 传播），
    // 不管 Y 方向，而 Vulkan 的 NDC 里 Y 朝下，不翻这一下画面上下颠倒。
    // 若让每个子类自己翻，新增一种相机时漏翻几乎是必然会发生的事 ——
    // 而且症状（画面倒置）看起来像是模型或视图矩阵错了，很容易查错方向。
    glm::mat4 Proj() const;

    // ⚠️ 每次调用都重算（一次投影构造 + 两次矩阵乘）。
    // 正常用法是在绘制循环**外**取一次存进局部变量，不要在 per-draw 循环里调。
    //
    // 没做 dirty-flag 缓存是刻意的：缓存要靠每个子类在改参数后记得
    // invalidate，而漏 invalidate 导致的"矩阵不更新"比多几次矩阵乘严重得多。
    // 等 profiler 真的指到这里再加。
    glm::mat4 ViewProj() const { return Proj() * View(); }

    // ---- 供控制器做手感换算 -------------------------------------------------

    // 距相机 distance 处，垂直可见范围的**一半**（世界单位）。
    //
    // 这是平移与缩放换算的核心量，也是最需要虚化的地方：
    //   透视相机 —— distance * tan(fovY/2)，随距离线性增长
    //   正交相机 —— 常量，与距离无关
    // 虚化之后 OrbitCameraController 做 pan 时完全不需要知道自己面对的是哪种
    // 投影，"抓住画面拖动"的手感在两种相机下都自动正确。
    virtual float HalfExtentAtDistance(float distance) const = 0;

protected:
    // 子类只需返回**标准**投影矩阵（GL 风格，Y 朝上）。
    // Vulkan 的 Y 翻转由 Proj() 统一处理，子类不要自己翻。
    virtual glm::mat4 ComputeProjection() const = 0;

private:
    glm::vec3 m_position    = glm::vec3(0.0f, 0.0f, 3.0f);
    glm::quat m_orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);  // 单位四元数

    float m_aspect = 1.0f;
    float m_nearZ  = 0.1f;
    float m_farZ   = 100.0f;
};

}  // namespace origin

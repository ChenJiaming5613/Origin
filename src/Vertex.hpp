#pragma once

namespace origin {

// 顶点格式。顶点输入描述是**手写**的，没有走反射：
// SPIRV-Reflect 能反射出 input variable 的 location 和类型，但推不出
// 我们想怎么打包（interleaved 还是分流、是否压缩法线），
// 所以顶点布局留在 C++ 侧显式声明，descriptor 布局才交给反射。
//
// 单独放一个头文件是为了让 GltfModel 能用它而不必 include OriginRenderer.hpp
// —— 后者会连带把 volk 拉进来，而导入器本身不应该依赖任何 Vulkan 头。
struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
};

}  // namespace origin

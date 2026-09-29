// 旋转 cube 的顶点着色器
//
// 关于矩阵约定（HLSL 移植里最容易搞错的一处，结论已用 spirv-dis 实测确认）：
//
// 必须同时看两件事，只看一件会得出相反结论：
//   1. DXC 给 push constant 里的 float4x4 打 **RowMajor** 装饰
//      （OpMemberDecorate ... 0 RowMajor, MatrixStride 16）；
//   2. 下面这个 mul(pc.mvp, ...) 被编译成 **OpVectorTimesMatrix**，
//      而不是 OpMatrixTimesVector。
//
// 叠加后的净效果：result[i] = Σ_k v[k] * mem[k][i]，
// 即把内存当**列主序**矩阵做 M * v —— 和 GLSL / glm 的行为完全一致。
//
// 所以 C++ 侧用 glm::mat4（列主序）直接上传即可，不需要任何转置。
// 早期自建的行主序 Mat4 曾需要 transpose 一次，换成 glm 后这一步就消失了。

struct PushConstants {
    float4x4 mvp;          // 64 字节
    float4   lightDirObj;  // 16 字节；xyz = 光源方向（已变换到物体空间），w = 环境光强度
    float4   lightColor;   // 16 字节；rgb = 光源颜色，a = 光源强度
};                         // 合计 96 字节，仍低于 128 的下限保证

[[vk::push_constant]] PushConstants pc;

struct VSInput {
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
};

struct VSOutput {
    float4 clipPos : SV_Position;
    float2 uv      : TEXCOORD0;
    float3 normal  : TEXCOORD1;  // 物体空间法线，光照在 PS 里算
};

VSOutput main(VSInput v) {
    VSOutput o;
    o.clipPos = mul(pc.mvp, float4(v.position, 1.0));
    o.uv      = v.uv;

    // 法线直接以物体空间传下去。
    // 光源方向已在 CPU 侧用 model 的逆旋转变换到物体空间，两者同一坐标系，
    // 于是 push constant 里不需要再带一个法线矩阵。
    o.normal = v.normal;
    return o;
}

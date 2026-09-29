// 旋转 cube 的像素着色器
//
// binding 必须用 [[vk::binding(N)]] 显式标注，不要用 HLSL 的 register(tN/sN)：
// DXC 只取 register 的**数字**、忽略 b/t/s/u 类别字母，
// 于是 register(t0) 与 register(s0) 会双双落到 binding 0 且不报任何警告。
// 显式标注同时也让这份声明与 C++ 侧的反射结果一一对上。

struct PushConstants {
    float4x4 mvp;
    float4   lightDirObj;  // xyz = 光源方向（物体空间），w = 环境光强度
    float4   lightColor;   // rgb = 光源颜色，a = 光源强度
};

[[vk::push_constant]] PushConstants pc;

[[vk::binding(0)]] Texture2D    baseColor;
[[vk::binding(1)]] SamplerState baseSampler;

struct PSInput {
    float4 clipPos : SV_Position;
    float2 uv      : TEXCOORD0;
    float3 normal  : TEXCOORD1;
};

float4 main(PSInput i) : SV_Target0 {
    float3 albedo = baseColor.Sample(baseSampler, i.uv).rgb;

    // 光照放在 PS 里逐像素算：cube 每个面只有 4 个顶点，
    // 在 VS 里算的话调参时几乎看不出方向变化。
    float3 n = normalize(i.normal);
    float3 l = normalize(pc.lightDirObj.xyz);
    float  ndotl = saturate(dot(n, l));

    float3 diffuse = pc.lightColor.rgb * pc.lightColor.a * ndotl;
    float3 ambient = pc.lightDirObj.www;  // 环境项塞在方向向量的 w 里，省一个 float4

    return float4(albedo * (ambient + diffuse), 1.0);
}

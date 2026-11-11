// Hello Vulkan — fragment (pixel) shader (HLSL, compiled to SPIR-V with DXC).
//
// SV_Target maps to color attachment location 0; the interpolated color comes
// in at [[vk::location(0)]], matching the vertex shader's output.

struct PSInput
{
    [[vk::location(0)]] float3 color : COLOR0;
};

float4 main(PSInput input) : SV_Target
{
    return float4(input.color, 1.0);
}

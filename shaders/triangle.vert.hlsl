// Hello Vulkan — vertex shader (HLSL, compiled to SPIR-V with DXC).
//
// The [[vk::location(N)]] attributes fix the Vulkan input/output locations so
// they match the vertex buffer layout in src/main.cpp:
//   location 0 = float2 position
//   location 1 = float3 color

struct VSInput
{
    [[vk::location(0)]] float2 position : POSITION;
    [[vk::location(1)]] float3 color    : COLOR0;
};

struct VSOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 color : COLOR0;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.color = input.color;
    return output;
}

// Shader with several resource categories so reflection has plenty to surface:
// - constant buffer (uniform)
// - sampled texture + sampler
// - read-write buffer
// - vertex + pixel entry points (semantics, varying I/O)
// - a struct + a user attribute

struct Constants
{
    float4 color;
    int    mode;
};

ConstantBuffer<Constants> uConstants;
Texture2D<float4>         uTexture;
SamplerState              uSampler;
RWStructuredBuffer<float> uOutput;

struct VSIn
{
    float3 position : POSITION;
    float2 uv       : TEXCOORD0;
};

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

[shader("vertex")]
VSOut vsMain(VSIn input)
{
    VSOut o;
    o.position = float4(input.position, 1.0);
    o.uv       = input.uv;
    return o;
}

[shader("pixel")]
float4 psMain(VSOut input) : SV_Target
{
    float4 sampled = uTexture.SampleLevel(uSampler, input.uv, 0);
    return sampled * uConstants.color;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    uOutput[tid.x] =
        uTexture.SampleLevel(uSampler, float2(tid.xy) / 16.0, 0).x * uConstants.color.x;
}

Texture2D<float4>         gTextures[2];
SamplerState              gSamplers[2];
RWStructuredBuffer<float> gOutput;

[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    gOutput[tid.x] = gTextures[0].SampleLevel(gSamplers[1], float2(0.5, 0.5), 0).x;
}

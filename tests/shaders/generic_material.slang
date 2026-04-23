// Module-scope generic parameter. Drives slangmake's type-argument
// specialization path: the two concrete types have structurally different
// binding footprints, so reflection must differ per permutation.
//
// [permutation type] MAT={Metal,Wood}

interface IBrdf
{
    float3 shade();
}

struct Metal : IBrdf
{
    float4 params;
    float3 shade() { return params.rgb; }
}

struct Wood : IBrdf
{
    Texture2D<float4> albedo;
    SamplerState      samp;
    float4            tint;
    float3 shade() { return albedo.SampleLevel(samp, float2(0.5, 0.5), 0).rgb * tint.rgb; }
}

type_param MAT : IBrdf;

ParameterBlock<MAT>        gMat;
RWStructuredBuffer<float3> uOutput;

[shader("compute")]
[numthreads(8, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uOutput[tid.x] = gMat.shade();
}

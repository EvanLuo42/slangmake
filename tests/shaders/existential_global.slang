// Shader with an interface-typed global variable. This declares an
// existential specialisation parameter that shows up in
// IComponentType::getSpecializationParamCount() but NOT in
// ProgramLayout::getTypeParameterByIndex() — so slangmake's typeArgs cannot
// reach it. The compile() wrapper must reject this configuration explicitly.

interface IBrdf
{
    float3 shade();
}

struct Metal : IBrdf
{
    float4 p;
    float3 shade() { return p.rgb; }
}

IBrdf                      gBrdf;
RWStructuredBuffer<float3> uOutput;

[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uOutput[tid.x] = gBrdf.shade();
}

// Nested include chain:
//   with_nested_include.slang -> chain_mid.hlsli -> chain_leaf.hlsli
// chain_mid.hlsli sits in an external directory, so -I is required.

#include "chain_mid.hlsli"

RWStructuredBuffer<uint> uOutput;

[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uOutput[tid.x] = chainMid(tid.x);
}

// Uses an .hlsli that lives OUTSIDE the source's directory, so the file
// can only be resolved via an explicit include search path.

#include "external_helpers.hlsli"

RWStructuredBuffer<uint> uOutput;

[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uOutput[tid.x] = externalFn(tid.x);
}

// Pulls the same .hlsli twice — once directly, once transitively through
// another include. Relies on the include guard inside helpers.hlsli to avoid
// duplicate declarations.

#include "helpers.hlsli"
#include "helpers.hlsli"

RWStructuredBuffer<uint> uOutput;

[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uOutput[tid.x] = helperAdd(tid.x, tid.y);
}

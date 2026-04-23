// Compute shader that pulls helpers from a sibling .hlsli.
// The .hlsli branches on USE_FAST_PATH, so each permutation value produces
// observably-different bytecode — proof that the define crosses the include.

// [permutation] USE_FAST_PATH={0,1}

#include "helpers.hlsli"

RWStructuredBuffer<uint> uOutput;

[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uOutput[tid.x] = helperAdd(tid.x, tid.y);
}

// Permuted compute shader.
// [permutation] USE_SHADOW={0,1}
// [permutation] QUALITY={0,1,2}

#ifndef USE_SHADOW
#define USE_SHADOW 0
#endif
#ifndef QUALITY
#define QUALITY 0
#endif

RWStructuredBuffer<uint> uOutput;

[shader("compute")]
[numthreads(8, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint v = USE_SHADOW * 100u + QUALITY;
    uOutput[tid.x] = v;
}

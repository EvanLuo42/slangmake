// Tiny deterministic compute shader. No defines, no resources.
// Used by the SPIRV byte-match test.

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    // intentionally empty
}

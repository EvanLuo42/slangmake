#include "slangmake_internal.h"

namespace slangmake
{

SlangCompileTarget toSlangCompileTarget(Target t)
{
    switch (t)
    {
    case Target::SPIRV:
        return SLANG_SPIRV;
    case Target::DXIL:
        return SLANG_DXIL;
    case Target::DXBC:
        return SLANG_DXBC;
    case Target::HLSL:
        return SLANG_HLSL;
    case Target::GLSL:
        return SLANG_GLSL;
    case Target::Metal:
        return SLANG_METAL;
    case Target::MetalLib:
        return SLANG_METAL_LIB;
    case Target::WGSL:
        return SLANG_WGSL;
    }
    return SLANG_TARGET_UNKNOWN;
}

// Make sure the on-disk integer codes used by the runtime side
// (detail::targetToBlobCode in slangmake_util.cpp) stay in lockstep with
// Slang's SlangCompileTarget enum so existing blobs keep round-tripping.
static_assert(static_cast<int>(SLANG_GLSL) == 2);
static_assert(static_cast<int>(SLANG_HLSL) == 5);
static_assert(static_cast<int>(SLANG_SPIRV) == 6);
static_assert(static_cast<int>(SLANG_DXBC) == 8);
static_assert(static_cast<int>(SLANG_DXIL) == 10);
static_assert(static_cast<int>(SLANG_METAL) == 24);
static_assert(static_cast<int>(SLANG_METAL_LIB) == 25);
static_assert(static_cast<int>(SLANG_WGSL) == 28);

} // namespace slangmake

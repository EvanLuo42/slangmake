#ifndef HELPERS_HLSLI
#define HELPERS_HLSLI

// The value we add differs by path so bytecode differs per permutation.
// USE_FAST_PATH is set by the caller (a `#define` or a -D / permutation).
#ifdef USE_FAST_PATH
  #if USE_FAST_PATH
    #define HELPER_CONST 1u
  #else
    #define HELPER_CONST 1000u
  #endif
#else
  #define HELPER_CONST 9999u
#endif

uint helperAdd(uint a, uint b)
{
#if defined(USE_FAST_PATH) && USE_FAST_PATH
    return a + b + HELPER_CONST;
#else
    // Deliberately slower / different expression so codegen diverges.
    uint r = a;
    for (uint i = 0u; i < 2u; ++i) r = r + b + HELPER_CONST;
    return r;
#endif
}

#endif

#pragma once

#include <exception>
#include <string>
#include <string_view>

#include "slangmake.h"
#include "slangmake.hpp"

// Helpers shared between slangmake_c.cpp (runtime) and slangmake_c_compiler.cpp.
// inline thread_local storage means a single shared error slot per DLL even
// though the two TUs are compiled separately.

namespace slangmake::cabi
{

inline thread_local std::string g_last_error;

inline void clear_error() noexcept { g_last_error.clear(); }

inline void set_error(std::string_view msg) noexcept
{
    try
    {
        g_last_error.assign(msg);
    }
    catch (...)
    {
        // best-effort: drop the message rather than propagate
    }
}

inline void set_error_from_exception() noexcept
{
    try
    {
        throw;
    }
    catch (const std::exception& e)
    {
        set_error(e.what());
    }
    catch (...)
    {
        set_error("unknown C++ exception");
    }
}

inline const char* cstr_or_empty(const char* s) noexcept { return s ? s : ""; }

inline Target to_cpp_target(sm_target_t t)
{
    switch (t)
    {
    case SM_TARGET_SPIRV:
        return Target::SPIRV;
    case SM_TARGET_DXIL:
        return Target::DXIL;
    case SM_TARGET_DXBC:
        return Target::DXBC;
    case SM_TARGET_HLSL:
        return Target::HLSL;
    case SM_TARGET_GLSL:
        return Target::GLSL;
    case SM_TARGET_METAL:
        return Target::Metal;
    case SM_TARGET_METALLIB:
        return Target::MetalLib;
    case SM_TARGET_WGSL:
        return Target::WGSL;
    }
    return Target::SPIRV;
}

inline sm_target_t to_c_target(Target t)
{
    switch (t)
    {
    case Target::SPIRV:
        return SM_TARGET_SPIRV;
    case Target::DXIL:
        return SM_TARGET_DXIL;
    case Target::DXBC:
        return SM_TARGET_DXBC;
    case Target::HLSL:
        return SM_TARGET_HLSL;
    case Target::GLSL:
        return SM_TARGET_GLSL;
    case Target::Metal:
        return SM_TARGET_METAL;
    case Target::MetalLib:
        return SM_TARGET_METALLIB;
    case Target::WGSL:
        return SM_TARGET_WGSL;
    }
    return SM_TARGET_SPIRV;
}

inline Codec to_cpp_codec(sm_codec_t c)
{
    switch (c)
    {
    case SM_CODEC_NONE:
        return Codec::None;
    case SM_CODEC_LZ4:
        return Codec::LZ4;
    case SM_CODEC_ZSTD:
        return Codec::Zstd;
    }
    return Codec::None;
}

inline sm_codec_t to_c_codec(Codec c)
{
    switch (c)
    {
    case Codec::None:
        return SM_CODEC_NONE;
    case Codec::LZ4:
        return SM_CODEC_LZ4;
    case Codec::Zstd:
        return SM_CODEC_ZSTD;
    }
    return SM_CODEC_NONE;
}

} // namespace slangmake::cabi

// ---- Shared opaque handle types --------------------------------------------
// These need to be visible from both TUs because slangmake_c_compiler.cpp
// uses sm_compile_options, sm_permutation, sm_perm_define_list, sm_buffer
// (which the runtime TU defines and exports).

struct sm_buffer
{
    std::vector<uint8_t> bytes;
};

struct sm_compile_options
{
    slangmake::CompileOptions opts;
};

struct sm_permutation
{
    slangmake::Permutation perm;
    mutable std::string    cached_key;
    mutable bool           key_dirty = true;

    void touch() noexcept { key_dirty = true; }

    const char* key_cstr() const
    {
        if (key_dirty)
        {
            cached_key = perm.key();
            key_dirty  = false;
        }
        return cached_key.c_str();
    }
};

struct sm_perm_define_list
{
    std::vector<slangmake::PermutationDefine> defs;
};

struct sm_permutation_list
{
    std::vector<slangmake::Permutation> perms;
};

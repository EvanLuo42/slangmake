// C ABI wrapper around the slangmake C++ API. Every function defined here
// corresponds 1:1 to a declaration in include/slangmake.h. Exceptions thrown
// from the C++ layer are caught and mapped to NULL / SM_ERR_INTERNAL with a
// thread-local diagnostic accessible via sm_last_error().

#include <algorithm>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "slangmake.h"
#include "slangmake.hpp"

namespace slangmake::detail
{
// Forward declaration so we can compute options hashes without pulling in
// slangmake_internal.h (which transitively includes slang.h). The function is
// defined in slangmake_util.cpp and exported with default visibility from the
// shared library.
uint64_t hashCompileOptions(const slangmake::CompileOptions& o);
} // namespace slangmake::detail

// Compile-time guarantee that every fmt:: POD has the same layout as its
// sm_fmt_*_t mirror. If any of these break, the on-disk format and the C
// header are out of sync and a version bump is required.
static_assert(sizeof(sm_fmt_blob_header_t) == sizeof(slangmake::fmt::BlobHeader));
static_assert(sizeof(sm_fmt_dep_entry_t) == sizeof(slangmake::fmt::DepEntry));
static_assert(sizeof(sm_fmt_entry_record_t) == sizeof(slangmake::fmt::EntryRecord));
static_assert(sizeof(sm_fmt_hashed_str_t) == sizeof(slangmake::fmt::HashedStr));
static_assert(sizeof(sm_fmt_attr_arg_t) == sizeof(slangmake::fmt::AttrArg));
static_assert(sizeof(sm_fmt_attribute_t) == sizeof(slangmake::fmt::Attribute));
static_assert(sizeof(sm_fmt_type_t) == sizeof(slangmake::fmt::Type));
static_assert(sizeof(sm_fmt_type_layout_t) == sizeof(slangmake::fmt::TypeLayout));
static_assert(sizeof(sm_fmt_binding_range_t) == sizeof(slangmake::fmt::BindingRange));
static_assert(sizeof(sm_fmt_descriptor_set_t) == sizeof(slangmake::fmt::DescriptorSet));
static_assert(sizeof(sm_fmt_descriptor_range_t) == sizeof(slangmake::fmt::DescriptorRange));
static_assert(sizeof(sm_fmt_sub_object_range_t) == sizeof(slangmake::fmt::SubObjectRange));
static_assert(sizeof(sm_fmt_variable_t) == sizeof(slangmake::fmt::Variable));
static_assert(sizeof(sm_fmt_var_layout_t) == sizeof(slangmake::fmt::VarLayout));
static_assert(sizeof(sm_fmt_function_t) == sizeof(slangmake::fmt::Function));
static_assert(sizeof(sm_fmt_generic_t) == sizeof(slangmake::fmt::Generic));
static_assert(sizeof(sm_fmt_decl_t) == sizeof(slangmake::fmt::Decl));
static_assert(sizeof(sm_fmt_entry_point_t) == sizeof(slangmake::fmt::EntryPoint));

namespace
{
thread_local std::string g_last_error;

void clear_error() noexcept { g_last_error.clear(); }

void set_error(std::string_view msg) noexcept
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

void set_error_from_exception() noexcept
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

const char* cstr_or_empty(const char* s) noexcept { return s ? s : ""; }

slangmake::Target to_cpp_target(sm_target_t t)
{
    switch (t)
    {
    case SM_TARGET_SPIRV:
        return slangmake::Target::SPIRV;
    case SM_TARGET_DXIL:
        return slangmake::Target::DXIL;
    case SM_TARGET_DXBC:
        return slangmake::Target::DXBC;
    case SM_TARGET_HLSL:
        return slangmake::Target::HLSL;
    case SM_TARGET_GLSL:
        return slangmake::Target::GLSL;
    case SM_TARGET_METAL:
        return slangmake::Target::Metal;
    case SM_TARGET_METALLIB:
        return slangmake::Target::MetalLib;
    case SM_TARGET_WGSL:
        return slangmake::Target::WGSL;
    }
    return slangmake::Target::SPIRV;
}

sm_target_t to_c_target(slangmake::Target t)
{
    switch (t)
    {
    case slangmake::Target::SPIRV:
        return SM_TARGET_SPIRV;
    case slangmake::Target::DXIL:
        return SM_TARGET_DXIL;
    case slangmake::Target::DXBC:
        return SM_TARGET_DXBC;
    case slangmake::Target::HLSL:
        return SM_TARGET_HLSL;
    case slangmake::Target::GLSL:
        return SM_TARGET_GLSL;
    case slangmake::Target::Metal:
        return SM_TARGET_METAL;
    case slangmake::Target::MetalLib:
        return SM_TARGET_METALLIB;
    case slangmake::Target::WGSL:
        return SM_TARGET_WGSL;
    }
    return SM_TARGET_SPIRV;
}

slangmake::Codec to_cpp_codec(sm_codec_t c)
{
    switch (c)
    {
    case SM_CODEC_NONE:
        return slangmake::Codec::None;
    case SM_CODEC_LZ4:
        return slangmake::Codec::LZ4;
    case SM_CODEC_ZSTD:
        return slangmake::Codec::Zstd;
    }
    return slangmake::Codec::None;
}

sm_codec_t to_c_codec(slangmake::Codec c)
{
    switch (c)
    {
    case slangmake::Codec::None:
        return SM_CODEC_NONE;
    case slangmake::Codec::LZ4:
        return SM_CODEC_LZ4;
    case slangmake::Codec::Zstd:
        return SM_CODEC_ZSTD;
    }
    return SM_CODEC_NONE;
}

} // namespace

// ---- Handle definitions (opaque to the C header) -------------------------

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

struct sm_compiler
{
    slangmake::Compiler compiler;
};

struct sm_compile_result
{
    slangmake::Compiler::Result result;
};

struct sm_blob_writer
{
    slangmake::BlobWriter writer;
    explicit sm_blob_writer(slangmake::Target t)
        : writer(t)
    {
    }
};

struct sm_blob_reader
{
    std::optional<slangmake::BlobReader> reader;
    // Cache of NUL-terminated dep paths because the on-disk format stores
    // them concatenated without separators.
    mutable std::vector<std::string> dep_path_cache;
    mutable bool                     dep_paths_built = false;

    void build_dep_path_cache() const
    {
        if (dep_paths_built || !reader)
            return;
        const auto deps = reader->dependencies();
        dep_path_cache.reserve(deps.size());
        for (const auto& d : deps)
            dep_path_cache.emplace_back(d.path);
        dep_paths_built = true;
    }
};

struct sm_reflection
{
    slangmake::ReflectionView view;
    explicit sm_reflection(std::span<const uint8_t> bytes)
        : view(bytes)
    {
    }
};

struct sm_batch_compiler
{
    slangmake::BatchCompiler batch;
    explicit sm_batch_compiler(slangmake::Compiler& c)
        : batch(c)
    {
    }
};

struct sm_batch_output
{
    slangmake::BatchCompiler::Output output;
    std::string                      output_path_cache;
    std::vector<std::string>         compiled_keys;
    // Failures already are std::strings; just expose c_str().
};

// ==========================================================================
// Implementation
// ==========================================================================

extern "C"
{
    const char* sm_last_error(void) { return g_last_error.c_str(); }

    // ---- Enum string helpers ---------------------------------------------

    const char* sm_target_to_string(sm_target_t t) { return slangmake::targetToString(to_cpp_target(t)); }

    sm_status_t sm_target_from_string(const char* s, sm_target_t* out)
    {
        if (!s || !out)
            return SM_ERR_INVALID_ARG;
        auto v = slangmake::parseTarget(s);
        if (!v)
            return SM_ERR_NOT_FOUND;
        *out = to_c_target(*v);
        return SM_OK;
    }

    const char* sm_codec_to_string(sm_codec_t c) { return slangmake::codecToString(to_cpp_codec(c)); }

    sm_status_t sm_codec_from_string(const char* s, sm_codec_t* out)
    {
        if (!s || !out)
            return SM_ERR_INVALID_ARG;
        auto v = slangmake::parseCodec(s);
        if (!v)
            return SM_ERR_NOT_FOUND;
        *out = to_c_codec(*v);
        return SM_OK;
    }

    // ---- Buffer ---------------------------------------------------------

    void sm_buffer_destroy(sm_buffer_t* buf) { delete buf; }

    const uint8_t* sm_buffer_data(const sm_buffer_t* buf)
    {
        return (buf && !buf->bytes.empty()) ? buf->bytes.data() : nullptr;
    }

    size_t sm_buffer_size(const sm_buffer_t* buf) { return buf ? buf->bytes.size() : 0; }

    // ---- CompileOptions --------------------------------------------------

    sm_compile_options_t* sm_compile_options_create(void)
    {
        clear_error();
        try
        {
            return new sm_compile_options{};
        }
        catch (...)
        {
            set_error_from_exception();
            return nullptr;
        }
    }

    void sm_compile_options_destroy(sm_compile_options_t* opts) { delete opts; }

    void sm_compile_options_set_input_file(sm_compile_options_t* opts, const char* path)
    {
        if (!opts)
            return;
        opts->opts.inputFile = path ? std::filesystem::path(path) : std::filesystem::path{};
    }

    void sm_compile_options_set_entry_point(sm_compile_options_t* opts, const char* name)
    {
        if (!opts)
            return;
        opts->opts.entryPoint = cstr_or_empty(name);
    }

    void sm_compile_options_set_target(sm_compile_options_t* opts, sm_target_t t)
    {
        if (!opts)
            return;
        opts->opts.target = to_cpp_target(t);
    }

    void sm_compile_options_set_profile(sm_compile_options_t* opts, const char* profile)
    {
        if (!opts)
            return;
        opts->opts.profile = cstr_or_empty(profile);
    }

    void sm_compile_options_add_include_path(sm_compile_options_t* opts, const char* path)
    {
        if (!opts || !path)
            return;
        opts->opts.includePaths.emplace_back(path);
    }

    void sm_compile_options_add_define(sm_compile_options_t* opts, const char* name, const char* value)
    {
        if (!opts || !name)
            return;
        slangmake::ShaderConstant c{};
        c.name  = name;
        c.value = cstr_or_empty(value);
        opts->opts.defines.push_back(std::move(c));
    }

    void sm_compile_options_clear_defines(sm_compile_options_t* opts)
    {
        if (!opts)
            return;
        opts->opts.defines.clear();
    }

    void sm_compile_options_set_optimization(sm_compile_options_t* opts, sm_optimization_t o)
    {
        if (!opts)
            return;
        opts->opts.optimization = static_cast<slangmake::Optimization>(o);
    }

    void sm_compile_options_set_matrix_layout(sm_compile_options_t* opts, sm_matrix_layout_t l)
    {
        if (!opts)
            return;
        opts->opts.matrixLayout = static_cast<slangmake::MatrixLayout>(l);
    }

    void sm_compile_options_set_fp_mode(sm_compile_options_t* opts, sm_fp_mode_t m)
    {
        if (!opts)
            return;
        opts->opts.fpMode = static_cast<slangmake::FloatingPointMode>(m);
    }

    void sm_compile_options_set_debug_info(sm_compile_options_t* opts, int v)
    {
        if (opts)
            opts->opts.debugInfo = v != 0;
    }
    void sm_compile_options_set_warnings_as_errors(sm_compile_options_t* opts, int v)
    {
        if (opts)
            opts->opts.warningsAsErrors = v != 0;
    }
    void sm_compile_options_set_glsl_scalar_layout(sm_compile_options_t* opts, int v)
    {
        if (opts)
            opts->opts.glslScalarLayout = v != 0;
    }
    void sm_compile_options_set_emit_spirv_directly(sm_compile_options_t* opts, int v)
    {
        if (opts)
            opts->opts.emitSpirvDirectly = v != 0;
    }
    void sm_compile_options_set_dump_intermediates(sm_compile_options_t* opts, int v)
    {
        if (opts)
            opts->opts.dumpIntermediates = v != 0;
    }
    void sm_compile_options_set_emit_reflection(sm_compile_options_t* opts, int v)
    {
        if (opts)
            opts->opts.emitReflection = v != 0;
    }
    void sm_compile_options_set_vulkan_version(sm_compile_options_t* opts, int v)
    {
        if (opts)
            opts->opts.vulkanVersion = v;
    }

    void sm_compile_options_add_vulkan_bind_shift(sm_compile_options_t* opts, uint32_t kind, uint32_t space,
                                                  uint32_t shift)
    {
        if (!opts)
            return;
        slangmake::VulkanBindShift s{};
        s.kind  = kind;
        s.space = space;
        s.shift = shift;
        opts->opts.vulkanBindShifts.push_back(s);
    }

    void sm_compile_options_set_language_version(sm_compile_options_t* opts, const char* v)
    {
        if (!opts)
            return;
        opts->opts.languageVersion = cstr_or_empty(v);
    }

    uint64_t sm_compile_options_hash(const sm_compile_options_t* opts)
    {
        if (!opts)
            return 0;
        // detail::hashCompileOptions lives in slangmake_internal.h, which we
        // don't include from this TU. The same fingerprint is reachable by
        // hashing the public CompileOptions directly through a private helper.
        return slangmake::detail::hashCompileOptions(opts->opts);
    }

    // ---- Permutation ----------------------------------------------------

    sm_permutation_t* sm_permutation_create(void)
    {
        clear_error();
        try
        {
            return new sm_permutation{};
        }
        catch (...)
        {
            set_error_from_exception();
            return nullptr;
        }
    }

    void sm_permutation_destroy(sm_permutation_t* perm) { delete perm; }

    void sm_permutation_add_constant(sm_permutation_t* perm, const char* name, const char* value)
    {
        if (!perm || !name)
            return;
        slangmake::ShaderConstant c{};
        c.name  = name;
        c.value = cstr_or_empty(value);
        perm->perm.constants.push_back(std::move(c));
        perm->touch();
    }

    void sm_permutation_add_type_arg(sm_permutation_t* perm, const char* name, const char* value)
    {
        if (!perm || !name)
            return;
        slangmake::ShaderConstant c{};
        c.name  = name;
        c.value = cstr_or_empty(value);
        perm->perm.typeArgs.push_back(std::move(c));
        perm->touch();
    }

    const char* sm_permutation_key(const sm_permutation_t* perm)
    {
        if (!perm)
            return "";
        return perm->key_cstr();
    }

    size_t sm_permutation_constant_count(const sm_permutation_t* perm)
    {
        return perm ? perm->perm.constants.size() : 0;
    }
    const char* sm_permutation_constant_name(const sm_permutation_t* perm, size_t idx)
    {
        if (!perm || idx >= perm->perm.constants.size())
            return "";
        return perm->perm.constants[idx].name.c_str();
    }
    const char* sm_permutation_constant_value(const sm_permutation_t* perm, size_t idx)
    {
        if (!perm || idx >= perm->perm.constants.size())
            return "";
        return perm->perm.constants[idx].value.c_str();
    }
    size_t sm_permutation_type_arg_count(const sm_permutation_t* perm) { return perm ? perm->perm.typeArgs.size() : 0; }
    const char* sm_permutation_type_arg_name(const sm_permutation_t* perm, size_t idx)
    {
        if (!perm || idx >= perm->perm.typeArgs.size())
            return "";
        return perm->perm.typeArgs[idx].name.c_str();
    }
    const char* sm_permutation_type_arg_value(const sm_permutation_t* perm, size_t idx)
    {
        if (!perm || idx >= perm->perm.typeArgs.size())
            return "";
        return perm->perm.typeArgs[idx].value.c_str();
    }

    // ---- PermutationDefine list -----------------------------------------

    sm_perm_define_list_t* sm_perm_define_list_create(void)
    {
        clear_error();
        try
        {
            return new sm_perm_define_list{};
        }
        catch (...)
        {
            set_error_from_exception();
            return nullptr;
        }
    }

    void sm_perm_define_list_destroy(sm_perm_define_list_t* list) { delete list; }

    void sm_perm_define_list_push(sm_perm_define_list_t* list, const char* name, sm_perm_define_kind_t kind,
                                  const char* const* values, size_t count)
    {
        if (!list || !name)
            return;
        slangmake::PermutationDefine d{};
        d.name = name;
        d.kind = (kind == SM_PERM_KIND_TYPE) ? slangmake::PermutationDefine::Kind::Type
                                             : slangmake::PermutationDefine::Kind::Constant;
        d.values.reserve(count);
        for (size_t i = 0; i < count; ++i)
            d.values.emplace_back(cstr_or_empty(values ? values[i] : nullptr));
        list->defs.push_back(std::move(d));
    }

    size_t sm_perm_define_list_size(const sm_perm_define_list_t* list) { return list ? list->defs.size() : 0; }

    const char* sm_perm_define_list_name(const sm_perm_define_list_t* list, size_t i)
    {
        if (!list || i >= list->defs.size())
            return "";
        return list->defs[i].name.c_str();
    }

    sm_perm_define_kind_t sm_perm_define_list_kind(const sm_perm_define_list_t* list, size_t i)
    {
        if (!list || i >= list->defs.size())
            return SM_PERM_KIND_CONSTANT;
        return list->defs[i].kind == slangmake::PermutationDefine::Kind::Type ? SM_PERM_KIND_TYPE
                                                                              : SM_PERM_KIND_CONSTANT;
    }

    size_t sm_perm_define_list_value_count(const sm_perm_define_list_t* list, size_t i)
    {
        if (!list || i >= list->defs.size())
            return 0;
        return list->defs[i].values.size();
    }

    const char* sm_perm_define_list_value(const sm_perm_define_list_t* list, size_t i, size_t v)
    {
        if (!list || i >= list->defs.size() || v >= list->defs[i].values.size())
            return "";
        return list->defs[i].values[v].c_str();
    }

    sm_perm_define_list_t* sm_parse_permutations_source(const char* src, size_t len)
    {
        clear_error();
        try
        {
            std::string_view sv;
            if (src)
                sv = (len == 0) ? std::string_view(src) : std::string_view(src, len);
            auto* h = new sm_perm_define_list{};
            h->defs = slangmake::PermutationParser::parse(sv);
            return h;
        }
        catch (...)
        {
            set_error_from_exception();
            return nullptr;
        }
    }

    sm_perm_define_list_t* sm_parse_permutations_file(const char* path)
    {
        clear_error();
        if (!path)
            return nullptr;
        try
        {
            auto* h = new sm_perm_define_list{};
            h->defs = slangmake::PermutationParser::parseFile(path);
            return h;
        }
        catch (...)
        {
            set_error_from_exception();
            return nullptr;
        }
    }

    sm_perm_define_list_t* sm_merge_permutations(const sm_perm_define_list_t* file_defines,
                                                 const sm_perm_define_list_t* cli_override)
    {
        clear_error();
        try
        {
            std::vector<slangmake::PermutationDefine> empty;
            const auto&                               f = file_defines ? file_defines->defs : empty;
            const auto&                               c = cli_override ? cli_override->defs : empty;
            auto*                                     h = new sm_perm_define_list{};
            h->defs                                     = slangmake::mergePermutationDefines(f, c);
            return h;
        }
        catch (...)
        {
            set_error_from_exception();
            return nullptr;
        }
    }

    int sm_source_has_no_reflection(const char* src, size_t len)
    {
        if (!src)
            return 0;
        std::string_view sv = (len == 0) ? std::string_view(src) : std::string_view(src, len);
        return slangmake::sourceHasNoReflection(sv) ? 1 : 0;
    }

    int sm_file_has_no_reflection(const char* path)
    {
        if (!path)
            return 0;
        return slangmake::fileHasNoReflection(path) ? 1 : 0;
    }

    // ---- Permutation list -----------------------------------------------

    sm_permutation_list_t* sm_expand_permutations(const sm_perm_define_list_t* defs)
    {
        clear_error();
        try
        {
            std::vector<slangmake::PermutationDefine> empty;
            const auto&                               d = defs ? defs->defs : empty;
            auto*                                     h = new sm_permutation_list{};
            h->perms                                    = slangmake::PermutationExpander::expand(d);
            return h;
        }
        catch (...)
        {
            set_error_from_exception();
            return nullptr;
        }
    }

    void sm_permutation_list_destroy(sm_permutation_list_t* list) { delete list; }

    size_t sm_permutation_list_size(const sm_permutation_list_t* list) { return list ? list->perms.size() : 0; }

    sm_permutation_t* sm_permutation_list_clone_at(const sm_permutation_list_t* list, size_t i)
    {
        if (!list || i >= list->perms.size())
            return nullptr;
        clear_error();
        try
        {
            auto* h = new sm_permutation{};
            h->perm = list->perms[i];
            h->touch();
            return h;
        }
        catch (...)
        {
            set_error_from_exception();
            return nullptr;
        }
    }

    // ---- Compiler -------------------------------------------------------

    sm_compiler_t* sm_compiler_create(void)
    {
        clear_error();
        try
        {
            return new sm_compiler{};
        }
        catch (...)
        {
            set_error_from_exception();
            return nullptr;
        }
    }

    void sm_compiler_destroy(sm_compiler_t* c) { delete c; }

    sm_compile_result_t* sm_compiler_compile(const sm_compiler_t* c, const sm_compile_options_t* opts,
                                             const sm_permutation_t* perm)
    {
        clear_error();
        if (!c || !opts)
        {
            set_error("sm_compiler_compile: null compiler or options");
            return nullptr;
        }
        try
        {
            auto*                  h = new sm_compile_result{};
            slangmake::Permutation p;
            if (perm)
                p = perm->perm;
            h->result = c->compiler.compile(opts->opts, p);
            return h;
        }
        catch (...)
        {
            set_error_from_exception();
            return nullptr;
        }
    }

    void sm_compile_result_destroy(sm_compile_result_t* r) { delete r; }
    int  sm_compile_result_success(const sm_compile_result_t* r) { return (r && r->result.success) ? 1 : 0; }

    const uint8_t* sm_compile_result_code(const sm_compile_result_t* r, size_t* out_size)
    {
        if (out_size)
            *out_size = r ? r->result.code.size() : 0;
        return (r && !r->result.code.empty()) ? r->result.code.data() : nullptr;
    }

    const uint8_t* sm_compile_result_reflection(const sm_compile_result_t* r, size_t* out_size)
    {
        if (out_size)
            *out_size = r ? r->result.reflection.size() : 0;
        return (r && !r->result.reflection.empty()) ? r->result.reflection.data() : nullptr;
    }

    const char* sm_compile_result_diagnostics(const sm_compile_result_t* r)
    {
        return r ? r->result.diagnostics.c_str() : "";
    }

    size_t sm_compile_result_dependency_count(const sm_compile_result_t* r)
    {
        return r ? r->result.dependencies.size() : 0;
    }

    const char* sm_compile_result_dependency(const sm_compile_result_t* r, size_t idx)
    {
        if (!r || idx >= r->result.dependencies.size())
            return "";
        return r->result.dependencies[idx].c_str();
    }

    // ---- Blob writer ----------------------------------------------------

    sm_blob_writer_t* sm_blob_writer_create(sm_target_t target)
    {
        clear_error();
        try
        {
            return new sm_blob_writer(to_cpp_target(target));
        }
        catch (...)
        {
            set_error_from_exception();
            return nullptr;
        }
    }

    void sm_blob_writer_destroy(sm_blob_writer_t* w) { delete w; }

    void sm_blob_writer_add_entry(sm_blob_writer_t* w, const sm_permutation_t* perm, const uint8_t* code,
                                  size_t code_size, const uint8_t* reflection, size_t reflection_size,
                                  const uint32_t* dep_indices, size_t dep_count)
    {
        if (!w || !perm)
            return;
        clear_error();
        try
        {
            std::span<const uint8_t>  code_span(code, code ? code_size : 0);
            std::span<const uint8_t>  refl_span(reflection, reflection ? reflection_size : 0);
            std::span<const uint32_t> dep_span(dep_indices, dep_indices ? dep_count : 0);
            w->writer.addEntry(perm->perm, code_span, refl_span, dep_span);
        }
        catch (...)
        {
            set_error_from_exception();
        }
    }

    void sm_blob_writer_set_dependencies(sm_blob_writer_t* w, const sm_dep_info_t* deps, size_t count)
    {
        if (!w)
            return;
        clear_error();
        try
        {
            std::vector<slangmake::BlobWriter::DepInfo> dv;
            dv.reserve(count);
            for (size_t i = 0; i < count; ++i)
            {
                slangmake::BlobWriter::DepInfo d{};
                d.path        = deps && deps[i].path ? deps[i].path : "";
                d.contentHash = deps ? deps[i].content_hash : 0;
                dv.push_back(std::move(d));
            }
            w->writer.setDependencies(std::move(dv));
        }
        catch (...)
        {
            set_error_from_exception();
        }
    }

    void sm_blob_writer_set_options_hash(sm_blob_writer_t* w, uint64_t h)
    {
        if (w)
            w->writer.setOptionsHash(h);
    }

    void sm_blob_writer_set_compression(sm_blob_writer_t* w, sm_codec_t c)
    {
        if (w)
            w->writer.setCompression(to_cpp_codec(c));
    }

    size_t sm_blob_writer_entry_count(const sm_blob_writer_t* w) { return w ? w->writer.entryCount() : 0; }

    sm_buffer_t* sm_blob_writer_finalize(const sm_blob_writer_t* w)
    {
        if (!w)
            return nullptr;
        clear_error();
        try
        {
            auto* b  = new sm_buffer{};
            b->bytes = w->writer.finalize();
            return b;
        }
        catch (...)
        {
            set_error_from_exception();
            return nullptr;
        }
    }

    sm_status_t sm_blob_writer_write_to_file(const sm_blob_writer_t* w, const char* path)
    {
        if (!w || !path)
            return SM_ERR_INVALID_ARG;
        clear_error();
        try
        {
            w->writer.writeToFile(path);
            return SM_OK;
        }
        catch (...)
        {
            set_error_from_exception();
            return SM_ERR_IO;
        }
    }

    // ---- Blob reader ----------------------------------------------------

    sm_blob_reader_t* sm_blob_reader_open_borrowed(const uint8_t* data, size_t size)
    {
        if (!data && size != 0)
            return nullptr;
        clear_error();
        try
        {
            auto* h   = new sm_blob_reader{};
            h->reader = slangmake::BlobReader(std::span<const uint8_t>(data, size));
            if (!h->reader->valid())
            {
                delete h;
                set_error("blob is not a valid SLNG payload");
                return nullptr;
            }
            return h;
        }
        catch (...)
        {
            set_error_from_exception();
            return nullptr;
        }
    }

    sm_blob_reader_t* sm_blob_reader_open_copy(const uint8_t* data, size_t size)
    {
        if (!data && size != 0)
            return nullptr;
        clear_error();
        try
        {
            std::vector<uint8_t> copy(data, data + size);
            auto*                h = new sm_blob_reader{};
            h->reader              = slangmake::BlobReader(std::move(copy));
            if (!h->reader->valid())
            {
                delete h;
                set_error("blob is not a valid SLNG payload");
                return nullptr;
            }
            return h;
        }
        catch (...)
        {
            set_error_from_exception();
            return nullptr;
        }
    }

    sm_blob_reader_t* sm_blob_reader_open_file(const char* path)
    {
        if (!path)
            return nullptr;
        clear_error();
        try
        {
            auto opt = slangmake::BlobReader::openFile(path);
            if (!opt)
            {
                set_error("failed to open blob file");
                return nullptr;
            }
            auto* h   = new sm_blob_reader{};
            h->reader = std::move(*opt);
            return h;
        }
        catch (...)
        {
            set_error_from_exception();
            return nullptr;
        }
    }

    void sm_blob_reader_destroy(sm_blob_reader_t* r) { delete r; }

    int sm_blob_reader_valid(const sm_blob_reader_t* r) { return (r && r->reader && r->reader->valid()) ? 1 : 0; }

    sm_target_t sm_blob_reader_target(const sm_blob_reader_t* r)
    {
        if (!r || !r->reader)
            return SM_TARGET_SPIRV;
        return to_c_target(r->reader->target());
    }

    uint64_t sm_blob_reader_options_hash(const sm_blob_reader_t* r)
    {
        return (r && r->reader) ? r->reader->optionsHash() : 0;
    }

    sm_codec_t sm_blob_reader_compression(const sm_blob_reader_t* r)
    {
        if (!r || !r->reader)
            return SM_CODEC_NONE;
        return to_c_codec(r->reader->compression());
    }

    size_t sm_blob_reader_entry_count(const sm_blob_reader_t* r)
    {
        return (r && r->reader) ? r->reader->entryCount() : 0;
    }

    sm_status_t sm_blob_reader_at(const sm_blob_reader_t* r, size_t idx, sm_blob_entry_t* out)
    {
        if (!r || !r->reader || !out)
            return SM_ERR_INVALID_ARG;
        if (idx >= r->reader->entryCount())
            return SM_ERR_NOT_FOUND;
        auto e               = r->reader->at(idx);
        out->key             = e.key.data();
        out->key_size        = e.key.size();
        out->code            = e.code.data();
        out->code_size       = e.code.size();
        out->reflection      = e.reflection.data();
        out->reflection_size = e.reflection.size();
        out->dep_indices     = e.depIndices.data();
        out->dep_index_count = e.depIndices.size();
        return SM_OK;
    }

    sm_status_t sm_blob_reader_find(const sm_blob_reader_t* r, const sm_permutation_t* perm, sm_blob_entry_t* out)
    {
        if (!r || !r->reader || !perm || !out)
            return SM_ERR_INVALID_ARG;
        auto e = r->reader->find(perm->perm);
        if (!e)
            return SM_ERR_NOT_FOUND;
        out->key             = e->key.data();
        out->key_size        = e->key.size();
        out->code            = e->code.data();
        out->code_size       = e->code.size();
        out->reflection      = e->reflection.data();
        out->reflection_size = e->reflection.size();
        out->dep_indices     = e->depIndices.data();
        out->dep_index_count = e->depIndices.size();
        return SM_OK;
    }

    size_t sm_blob_reader_dependency_count(const sm_blob_reader_t* r)
    {
        if (!r || !r->reader)
            return 0;
        return r->reader->dependencies().size();
    }

    const char* sm_blob_reader_dependency_path(const sm_blob_reader_t* r, size_t idx)
    {
        if (!r || !r->reader)
            return "";
        r->build_dep_path_cache();
        if (idx >= r->dep_path_cache.size())
            return "";
        return r->dep_path_cache[idx].c_str();
    }

    uint64_t sm_blob_reader_dependency_hash(const sm_blob_reader_t* r, size_t idx)
    {
        if (!r || !r->reader)
            return 0;
        auto deps = r->reader->dependencies();
        if (idx >= deps.size())
            return 0;
        return deps[idx].contentHash;
    }

    // ---- ReflectionView -------------------------------------------------

    sm_reflection_t* sm_reflection_open(const uint8_t* data, size_t size)
    {
        if (!data && size != 0)
            return nullptr;
        clear_error();
        try
        {
            auto* h = new sm_reflection(std::span<const uint8_t>(data, size));
            if (!h->view.valid())
            {
                delete h;
                set_error("reflection bytes are not a valid SLRF section");
                return nullptr;
            }
            return h;
        }
        catch (...)
        {
            set_error_from_exception();
            return nullptr;
        }
    }

    void sm_reflection_destroy(sm_reflection_t* r) { delete r; }

    int sm_reflection_valid(const sm_reflection_t* r) { return (r && r->view.valid()) ? 1 : 0; }

    uint64_t sm_reflection_first_entry_point_hash(const sm_reflection_t* r)
    {
        return r ? r->view.firstEntryPointHash() : 0;
    }
    uint32_t sm_reflection_global_cb_binding(const sm_reflection_t* r)
    {
        return r ? r->view.globalConstantBufferBinding() : 0;
    }
    uint32_t sm_reflection_global_cb_size(const sm_reflection_t* r)
    {
        return r ? r->view.globalConstantBufferSize() : 0;
    }
    uint32_t sm_reflection_bindless_space(const sm_reflection_t* r) { return r ? r->view.bindlessSpaceIndex() : 0; }
    uint32_t sm_reflection_global_params_var_layout(const sm_reflection_t* r)
    {
        if (!r)
            return SM_INVALID_INDEX;
        auto v = r->view.globalParamsVarLayout();
        return v ? *v : SM_INVALID_INDEX;
    }

    const char* sm_reflection_string(const sm_reflection_t* r, uint32_t str_idx)
    {
        if (!r)
            return "";
        // ReflectionView::string returns a string_view into the embedded string
        // pool; the pool is laid out as a contiguous run of NUL-terminated
        // strings, so .data() yields a valid C string.
        return r->view.string(str_idx).data();
    }

    const char* sm_reflection_hashed_string(const sm_reflection_t* r, uint32_t index)
    {
        if (!r)
            return "";
        return r->view.hashedString(index).data();
    }

#define SM_TABLE_ACCESSOR(NAME, CTYPE, CALL)                   \
    const CTYPE* NAME(const sm_reflection_t* r, size_t* count) \
    {                                                          \
        if (count)                                             \
            *count = 0;                                        \
        if (!r)                                                \
            return nullptr;                                    \
        auto s = r->view.CALL();                               \
        if (count)                                             \
            *count = s.size();                                 \
        return reinterpret_cast<const CTYPE*>(s.data());       \
    }

    SM_TABLE_ACCESSOR(sm_reflection_types, sm_fmt_type_t, types)
    SM_TABLE_ACCESSOR(sm_reflection_type_layouts, sm_fmt_type_layout_t, typeLayouts)
    SM_TABLE_ACCESSOR(sm_reflection_variables, sm_fmt_variable_t, variables)
    SM_TABLE_ACCESSOR(sm_reflection_var_layouts, sm_fmt_var_layout_t, varLayouts)
    SM_TABLE_ACCESSOR(sm_reflection_functions, sm_fmt_function_t, functions)
    SM_TABLE_ACCESSOR(sm_reflection_generics, sm_fmt_generic_t, generics)
    SM_TABLE_ACCESSOR(sm_reflection_decls, sm_fmt_decl_t, decls)
    SM_TABLE_ACCESSOR(sm_reflection_entry_points, sm_fmt_entry_point_t, entryPoints)
    SM_TABLE_ACCESSOR(sm_reflection_attributes, sm_fmt_attribute_t, attributes)
    SM_TABLE_ACCESSOR(sm_reflection_attribute_args, sm_fmt_attr_arg_t, attributeArgs)
    SM_TABLE_ACCESSOR(sm_reflection_modifier_pool, uint32_t, modifierPool)
    SM_TABLE_ACCESSOR(sm_reflection_hashed_strings, sm_fmt_hashed_str_t, hashedStrings)
    SM_TABLE_ACCESSOR(sm_reflection_binding_ranges, sm_fmt_binding_range_t, bindingRanges)
    SM_TABLE_ACCESSOR(sm_reflection_descriptor_sets, sm_fmt_descriptor_set_t, descriptorSets)
    SM_TABLE_ACCESSOR(sm_reflection_descriptor_ranges, sm_fmt_descriptor_range_t, descriptorRanges)
    SM_TABLE_ACCESSOR(sm_reflection_sub_object_ranges, sm_fmt_sub_object_range_t, subObjectRanges)
    SM_TABLE_ACCESSOR(sm_reflection_u32_pool, uint32_t, u32Pool)

#undef SM_TABLE_ACCESSOR

    // ---- BatchCompiler --------------------------------------------------

    sm_batch_compiler_t* sm_batch_compiler_create(sm_compiler_t* compiler)
    {
        if (!compiler)
            return nullptr;
        clear_error();
        try
        {
            return new sm_batch_compiler(compiler->compiler);
        }
        catch (...)
        {
            set_error_from_exception();
            return nullptr;
        }
    }

    void sm_batch_compiler_destroy(sm_batch_compiler_t* b) { delete b; }

    void sm_batch_compiler_set_keep_going(sm_batch_compiler_t* b, int v)
    {
        if (b)
            b->batch.setKeepGoing(v != 0);
    }
    void sm_batch_compiler_set_verbose(sm_batch_compiler_t* b, int v)
    {
        if (b)
            b->batch.setVerbose(v != 0);
    }
    void sm_batch_compiler_set_quiet(sm_batch_compiler_t* b, int v)
    {
        if (b)
            b->batch.setQuiet(v != 0);
    }
    void sm_batch_compiler_set_jobs(sm_batch_compiler_t* b, int n)
    {
        if (b)
            b->batch.setJobs(n);
    }
    void sm_batch_compiler_set_incremental(sm_batch_compiler_t* b, int v)
    {
        if (b)
            b->batch.setIncremental(v != 0);
    }
    void sm_batch_compiler_set_compression(sm_batch_compiler_t* b, sm_codec_t c)
    {
        if (b)
            b->batch.setCompression(to_cpp_codec(c));
    }

    size_t sm_batch_compiler_last_reused(const sm_batch_compiler_t* b) { return b ? b->batch.lastStats().reused : 0; }
    size_t sm_batch_compiler_last_compiled(const sm_batch_compiler_t* b)
    {
        return b ? b->batch.lastStats().compiled : 0;
    }

    sm_batch_output_t* sm_batch_compile_file(sm_batch_compiler_t* b, const char* file, const sm_compile_options_t* opts,
                                             const sm_perm_define_list_t* cli_override, const char* output_path)
    {
        if (!b || !file || !opts || !output_path)
        {
            set_error("sm_batch_compile_file: missing required argument");
            return nullptr;
        }
        clear_error();
        try
        {
            slangmake::BatchCompiler::Input in{};
            in.file    = file;
            in.options = opts->opts;
            if (cli_override)
                in.cliOverride = cli_override->defs;

            auto* h              = new sm_batch_output{};
            h->output            = b->batch.compileFile(in, std::filesystem::path(output_path));
            h->output_path_cache = h->output.outputFile.string();
            h->compiled_keys.reserve(h->output.compiled.size());
            for (const auto& p : h->output.compiled)
                h->compiled_keys.emplace_back(p.key());
            return h;
        }
        catch (...)
        {
            set_error_from_exception();
            return nullptr;
        }
    }

    void sm_batch_output_destroy(sm_batch_output_t* o) { delete o; }

    const char* sm_batch_output_path(const sm_batch_output_t* o) { return o ? o->output_path_cache.c_str() : ""; }

    const uint8_t* sm_batch_output_blob(const sm_batch_output_t* o, size_t* out_size)
    {
        if (out_size)
            *out_size = o ? o->output.blob.size() : 0;
        return (o && !o->output.blob.empty()) ? o->output.blob.data() : nullptr;
    }

    size_t sm_batch_output_compiled_count(const sm_batch_output_t* o) { return o ? o->output.compiled.size() : 0; }

    const char* sm_batch_output_compiled_key(const sm_batch_output_t* o, size_t i)
    {
        if (!o || i >= o->compiled_keys.size())
            return "";
        return o->compiled_keys[i].c_str();
    }

    sm_permutation_t* sm_batch_output_compiled_clone(const sm_batch_output_t* o, size_t i)
    {
        if (!o || i >= o->output.compiled.size())
            return nullptr;
        clear_error();
        try
        {
            auto* h = new sm_permutation{};
            h->perm = o->output.compiled[i];
            h->touch();
            return h;
        }
        catch (...)
        {
            set_error_from_exception();
            return nullptr;
        }
    }

    size_t sm_batch_output_failure_count(const sm_batch_output_t* o) { return o ? o->output.failures.size() : 0; }

    const char* sm_batch_output_failure(const sm_batch_output_t* o, size_t i)
    {
        if (!o || i >= o->output.failures.size())
            return "";
        return o->output.failures[i].c_str();
    }

} // extern "C"

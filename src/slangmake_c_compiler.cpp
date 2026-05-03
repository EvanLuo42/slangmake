// Compiler portion of the C ABI wrapper around the slangmake C++ API. This TU
// links against slang and DXC; everything that does not need them lives in
// slangmake_c.cpp instead.

#include <exception>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "slangmake.h"
#include "slangmake.hpp"
#include "slangmake_c_internal.h"

using slangmake::cabi::clear_error;
using slangmake::cabi::set_error;
using slangmake::cabi::set_error_from_exception;
using slangmake::cabi::to_cpp_codec;

// ---- Compiler-owned handle types -----------------------------------------

struct sm_compiler
{
    slangmake::Compiler compiler;
};

struct sm_compile_result
{
    slangmake::Compiler::Result result;
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
};

// ==========================================================================
// Implementation
// ==========================================================================

extern "C"
{

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

#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>

#include "slangmake.hpp"

using namespace slangmake;

namespace fs = std::filesystem;

TEST_CASE("Compiler::compile compiles compute.slang to SPIRV")
{
    Compiler       c;
    CompileOptions opts;
    opts.inputFile = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "compute.slang";
    opts.target    = Target::SPIRV;
    opts.profile   = "sm_6_5";

    auto r = c.compile(opts, Permutation{});
    INFO("diagnostics: " << r.diagnostics);
    REQUIRE(r.success);
    REQUIRE(r.code.size() >= 4);
    // SPIR-V magic 0x07230203, little-endian on x86 / ARM64 Slang targets.
    uint32_t magic;
    std::memcpy(&magic, r.code.data(), 4);
    CHECK(magic == 0x07230203u);
    CHECK_FALSE(r.reflection.empty());
}

TEST_CASE("Compiler::compile honours --no-reflection (emitReflection=false)")
{
    Compiler       c;
    CompileOptions opts;
    opts.inputFile      = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "compute.slang";
    opts.target         = Target::SPIRV;
    opts.profile        = "sm_6_5";
    opts.emitReflection = false;

    auto r = c.compile(opts, Permutation{});
    REQUIRE(r.success);
    CHECK(r.reflection.empty());
}

TEST_CASE("Compiler::compile honours per-permutation defines")
{
    Compiler       c;
    CompileOptions opts;
    opts.inputFile = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "permuted.slang";
    opts.target    = Target::SPIRV;
    opts.profile   = "sm_6_5";

    Permutation a{{{"USE_SHADOW", "0"}, {"QUALITY", "0"}}};
    Permutation b{{{"USE_SHADOW", "1"}, {"QUALITY", "2"}}};
    auto        ra = c.compile(opts, a);
    auto        rb = c.compile(opts, b);
    INFO("a diag: " << ra.diagnostics);
    INFO("b diag: " << rb.diagnostics);
    REQUIRE(ra.success);
    REQUIRE(rb.success);
    CHECK(ra.code != rb.code); // different defines must produce different bytecode
}

TEST_CASE("Compiler::compile reports failure on a missing entry point")
{
    Compiler       c;
    CompileOptions opts;
    opts.inputFile  = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "compute.slang";
    opts.target     = Target::SPIRV;
    opts.profile    = "sm_6_5";
    opts.entryPoint = "doesNotExist";

    auto r = c.compile(opts, Permutation{});
    CHECK_FALSE(r.success);
    CHECK_FALSE(r.diagnostics.empty());
}

TEST_CASE("Compiler::compile reports failure on a missing input file")
{
    Compiler       c;
    CompileOptions opts;
    opts.inputFile = "/does/not/exist.slang";
    opts.target    = Target::SPIRV;
    opts.profile   = "sm_6_5";
    auto r         = c.compile(opts, Permutation{});
    CHECK_FALSE(r.success);
    CHECK_FALSE(r.diagnostics.empty());
}

TEST_CASE("Compiler::compile can target a named entry in a multi-entry shader")
{
    Compiler       c;
    CompileOptions opts;
    opts.inputFile = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "with_resources.slang";
    opts.target    = Target::SPIRV;
    opts.profile   = "sm_6_5";

    opts.entryPoint = "csMain";
    auto rCs        = c.compile(opts, Permutation{});
    INFO("cs diag: " << rCs.diagnostics);
    REQUIRE(rCs.success);
    REQUIRE(rCs.code.size() >= 4);

    opts.entryPoint = "vsMain";
    auto rVs        = c.compile(opts, Permutation{});
    INFO("vs diag: " << rVs.diagnostics);
    REQUIRE(rVs.success);
    REQUIRE(rVs.code.size() >= 4);

    // Two different entry points at the same target must produce distinct
    // bytecode — proof that opts.entryPoint actually selected a single entry.
    CHECK(rVs.code != rCs.code);
}

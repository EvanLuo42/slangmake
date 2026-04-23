#include <doctest/doctest.h>

#include <filesystem>

#include "slangmake.h"

using namespace slangmake;
namespace fs = std::filesystem;

TEST_CASE("Compiler resolves #include when the include path is set")
{
    Compiler       c;
    CompileOptions opts;
    opts.inputFile    = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "with_include.slang";
    opts.target       = Target::SPIRV;
    opts.profile      = "sm_6_5";
    opts.includePaths = {fs::path(SLANG_MAKE_TESTS_SHADER_DIR)};

    auto r = c.compile(opts, Permutation{{{"USE_FAST_PATH", "1"}}});
    INFO("diag: " << r.diagnostics);
    REQUIRE(r.success);
    REQUIRE(r.code.size() >= 4);
    uint32_t magic;
    std::memcpy(&magic, r.code.data(), 4);
    CHECK(magic == 0x07230203u);
}

TEST_CASE("Defines reach #ifdef inside the included .hlsli (permutations differ)")
{
    Compiler       c;
    CompileOptions opts;
    opts.inputFile    = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "with_include.slang";
    opts.target       = Target::SPIRV;
    opts.profile      = "sm_6_5";
    opts.includePaths = {fs::path(SLANG_MAKE_TESTS_SHADER_DIR)};

    Permutation fast{{{"USE_FAST_PATH", "1"}}};
    Permutation slow{{{"USE_FAST_PATH", "0"}}};

    auto rFast = c.compile(opts, fast);
    auto rSlow = c.compile(opts, slow);
    INFO("fast diag: " << rFast.diagnostics);
    INFO("slow diag: " << rSlow.diagnostics);
    REQUIRE(rFast.success);
    REQUIRE(rSlow.success);

    // Both branches in helpers.hlsli are gated on USE_FAST_PATH, so the
    // bytecode MUST differ — that is the proof that the define propagated
    // through the include.
    CHECK(rFast.code != rSlow.code);
}

TEST_CASE("External .hlsli (outside source dir) requires -I to resolve")
{
    Compiler       c;
    CompileOptions opts;
    opts.inputFile = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "with_external_include.slang";
    opts.target    = Target::SPIRV;
    opts.profile   = "sm_6_5";

    // Without the extra search path, "external_helpers.hlsli" cannot be found.
    auto rFail = c.compile(opts, Permutation{});
    CHECK_FALSE(rFail.success);
    CHECK_FALSE(rFail.diagnostics.empty());

    // Adding the search path makes it resolve.
    opts.includePaths = {fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "external"};
    auto rOk = c.compile(opts, Permutation{});
    INFO("diag: " << rOk.diagnostics);
    REQUIRE(rOk.success);
    REQUIRE(rOk.code.size() >= 4);
}

TEST_CASE("Nested .hlsli include chain resolves with a single -I")
{
    Compiler       c;
    CompileOptions opts;
    opts.inputFile    = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "with_nested_include.slang";
    opts.target       = Target::SPIRV;
    opts.profile      = "sm_6_5";
    opts.includePaths = {fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "external"};

    // Chain: slang -> chain_mid.hlsli -> chain_leaf.hlsli (relative to mid's dir).
    auto r = c.compile(opts, Permutation{});
    INFO("diag: " << r.diagnostics);
    REQUIRE(r.success);
    REQUIRE(r.code.size() >= 4);
}

TEST_CASE("Including the same .hlsli twice relies on the include guard")
{
    // If the `#ifndef HELPERS_HLSLI` guard in helpers.hlsli is honoured, this
    // compiles fine — otherwise the second include would re-declare helperAdd
    // and produce a diagnostic.
    Compiler       c;
    CompileOptions opts;
    opts.inputFile = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "with_double_include.slang";
    opts.target    = Target::SPIRV;
    opts.profile   = "sm_6_5";

    auto r = c.compile(opts, Permutation{{{"USE_FAST_PATH", "1"}}});
    INFO("diag: " << r.diagnostics);
    REQUIRE(r.success);
    REQUIRE(r.code.size() >= 4);
}

TEST_CASE("Global -D (CompileOptions::defines) reaches #ifdef inside the .hlsli")
{
    // Same idea as the permutation test, but the define is supplied via
    // `CompileOptions::defines` (the global -D channel) instead of the per-call
    // `Permutation`. The bytecode for the two global defines must differ,
    // proving global -Ds are visible inside the included file.
    Compiler       c;
    CompileOptions base;
    base.inputFile    = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "with_include.slang";
    base.target       = Target::SPIRV;
    base.profile      = "sm_6_5";
    base.includePaths = {fs::path(SLANG_MAKE_TESTS_SHADER_DIR)};

    auto optsFast = base;
    optsFast.defines.push_back({"USE_FAST_PATH", "1"});
    auto optsSlow = base;
    optsSlow.defines.push_back({"USE_FAST_PATH", "0"});

    auto rFast = c.compile(optsFast, Permutation{});
    auto rSlow = c.compile(optsSlow, Permutation{});
    INFO("fast diag: " << rFast.diagnostics);
    INFO("slow diag: " << rSlow.diagnostics);
    REQUIRE(rFast.success);
    REQUIRE(rSlow.success);
    CHECK(rFast.code != rSlow.code);
}

TEST_CASE("BatchCompiler compiles an #include-ing shader with permutations")
{
    auto outDir = fs::temp_directory_path() / "slangmake_include_test";
    fs::remove_all(outDir);
    fs::create_directories(outDir);

    Compiler       c;
    BatchCompiler  bc(c);
    bc.setQuiet(true);

    BatchCompiler::Input in;
    in.file            = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "with_include.slang";
    in.options.target  = Target::SPIRV;
    in.options.profile = "sm_6_5";
    in.options.includePaths = {fs::path(SLANG_MAKE_TESTS_SHADER_DIR)};

    auto outPath = outDir / "with_include.bin";
    auto out     = bc.compileFile(in, outPath);

    REQUIRE(out.failures.empty());
    CHECK(out.compiled.size() == 2); // USE_FAST_PATH={0,1}

    auto reader = BlobReader::openFile(outPath);
    REQUIRE(reader.has_value());
    REQUIRE(reader->entryCount() == 2);

    // The two permutations hit different branches in the .hlsli, so the
    // bytecode for each must differ.
    auto e0 = reader->at(0);
    auto e1 = reader->at(1);
    std::vector<uint8_t> c0(e0.code.begin(), e0.code.end());
    std::vector<uint8_t> c1(e1.code.begin(), e1.code.end());
    CHECK(c0 != c1);

    fs::remove_all(outDir);
}

TEST_CASE("Compiler::Result::dependencies lists the source and its #include-d files")
{
    Compiler       c;
    CompileOptions opts;
    opts.inputFile    = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "with_include.slang";
    opts.target       = Target::SPIRV;
    opts.profile      = "sm_6_5";
    opts.includePaths = {fs::path(SLANG_MAKE_TESTS_SHADER_DIR)};

    auto r = c.compile(opts, Permutation{{{"USE_FAST_PATH", "1"}}});
    INFO("diag: " << r.diagnostics);
    REQUIRE(r.success);
    REQUIRE_FALSE(r.dependencies.empty());

    // The dep list must include both the top-level source and the .hlsli it
    // #includes. Slang reports absolute paths; compare by filename.
    auto hasFilename = [&](std::string_view name) {
        for (const auto& p : r.dependencies)
            if (fs::path(p).filename() == name)
                return true;
        return false;
    };
    CHECK(hasFilename("with_include.slang"));
    CHECK(hasFilename("helpers.hlsli"));
}

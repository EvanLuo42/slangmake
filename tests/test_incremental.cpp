#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "slangmake.h"

using namespace slangmake;
namespace fs = std::filesystem;

namespace
{

fs::path makeScratchDir(const char* tag)
{
    auto p = fs::temp_directory_path() / (std::string("slangmake_inc_") + tag);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

// Copy a fixture into the scratch dir so we can mutate it without touching
// tests/shaders/.
fs::path copyFixture(const fs::path& scratch, const char* fixtureName)
{
    auto dst = scratch / fixtureName;
    fs::copy_file(fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / fixtureName, dst);
    return dst;
}

} // namespace

TEST_CASE("Second compile with no changes reuses every entry")
{
    auto scratch = makeScratchDir("noop");
    auto src     = copyFixture(scratch, "permuted.slang");
    auto outPath = scratch / "permuted.bin";

    Compiler      c;
    BatchCompiler bc(c);
    bc.setQuiet(true);

    BatchCompiler::Input in;
    in.file            = src;
    in.options.target  = Target::SPIRV;
    in.options.profile = "sm_6_5";

    auto first = bc.compileFile(in, outPath);
    REQUIRE(first.failures.empty());
    REQUIRE(first.compiled.size() == 6);
    CHECK(bc.lastStats().reused == 0);
    CHECK(bc.lastStats().compiled == 6);

    // Same inputs, same output path → every entry should be reused.
    auto second = bc.compileFile(in, outPath);
    REQUIRE(second.failures.empty());
    REQUIRE(second.compiled.size() == 6);
    CHECK(bc.lastStats().reused == 6);
    CHECK(bc.lastStats().compiled == 0);
}

TEST_CASE("Adding a new permutation value only compiles the new ones")
{
    auto scratch = makeScratchDir("add_perm");
    auto src     = copyFixture(scratch, "permuted.slang");
    auto outPath = scratch / "permuted.bin";

    Compiler      c;
    BatchCompiler bc(c);
    bc.setQuiet(true);

    BatchCompiler::Input in;
    in.file            = src;
    in.options.target  = Target::SPIRV;
    in.options.profile = "sm_6_5";

    // First run: file says USE_SHADOW={0,1} × QUALITY={0,1,2} = 6 permutations.
    auto first = bc.compileFile(in, outPath);
    REQUIRE(first.compiled.size() == 6);
    CHECK(bc.lastStats().compiled == 6);

    // Second run: extend USE_SHADOW with a new value via CLI override (3×3=9).
    in.cliOverride = {{"USE_SHADOW", {"0", "1", "2"}}};
    auto second    = bc.compileFile(in, outPath);
    REQUIRE(second.failures.empty());
    REQUIRE(second.compiled.size() == 9);
    // The three pre-existing USE_SHADOW values × three QUALITY values were
    // already compiled → 6 reused. The new USE_SHADOW=2 value × 3 QUALITY
    // values are fresh → 3 compiled.
    CHECK(bc.lastStats().reused == 6);
    CHECK(bc.lastStats().compiled == 3);
}

TEST_CASE("Modifying the source file invalidates the cache")
{
    auto scratch = makeScratchDir("edit_src");
    auto src     = copyFixture(scratch, "permuted.slang");
    auto outPath = scratch / "permuted.bin";

    Compiler      c;
    BatchCompiler bc(c);
    bc.setQuiet(true);

    BatchCompiler::Input in;
    in.file            = src;
    in.options.target  = Target::SPIRV;
    in.options.profile = "sm_6_5";

    bc.compileFile(in, outPath);
    CHECK(bc.lastStats().compiled == 6);

    // Append a harmless trailing comment — content hash changes, so every
    // permutation must recompile.
    {
        std::ofstream f(src, std::ios::app | std::ios::binary);
        f << "\n// edited\n";
    }
    bc.compileFile(in, outPath);
    CHECK(bc.lastStats().reused == 0);
    CHECK(bc.lastStats().compiled == 6);
}

TEST_CASE("Modifying an included .hlsli invalidates the cache")
{
    auto scratch = makeScratchDir("edit_hlsli");
    auto src     = copyFixture(scratch, "with_include.slang");
    auto hlsli   = copyFixture(scratch, "helpers.hlsli");
    auto outPath = scratch / "with_include.bin";

    Compiler      c;
    BatchCompiler bc(c);
    bc.setQuiet(true);

    BatchCompiler::Input in;
    in.file                 = src;
    in.options.target       = Target::SPIRV;
    in.options.profile      = "sm_6_5";
    in.options.includePaths = {scratch};

    bc.compileFile(in, outPath);
    CHECK(bc.lastStats().compiled == 2);

    // Touch the included file so its content hash changes.
    {
        std::ofstream f(hlsli, std::ios::app | std::ios::binary);
        f << "\n// edited include\n";
    }
    bc.compileFile(in, outPath);
    CHECK(bc.lastStats().reused == 0);
    CHECK(bc.lastStats().compiled == 2);
}

TEST_CASE("Changing a CompileOption invalidates the cache")
{
    auto scratch = makeScratchDir("opts");
    auto src     = copyFixture(scratch, "compute.slang");
    auto outPath = scratch / "compute.bin";

    Compiler      c;
    BatchCompiler bc(c);
    bc.setQuiet(true);

    BatchCompiler::Input in;
    in.file            = src;
    in.options.target  = Target::SPIRV;
    in.options.profile = "sm_6_5";

    bc.compileFile(in, outPath);
    CHECK(bc.lastStats().compiled == 1);

    // Flip an option that feeds into the optionsHash; cache must invalidate.
    in.options.debugInfo = true;
    bc.compileFile(in, outPath);
    CHECK(bc.lastStats().reused == 0);
    CHECK(bc.lastStats().compiled == 1);
}

TEST_CASE("Editing a .hlsli only invalidates permutations that actually include it")
{
    // Layout:
    //   with_include.slang  -> #include "helpers.hlsli"            (both perms)
    //   extra.hlsli                                                  (only USE_FAST_PATH=1 via its own gate)
    // We emulate the asymmetric dep by making one perm depend on an extra file
    // that the other perm does not see. We do this with a tiny two-file setup.
    auto scratch = makeScratchDir("partial");

    // Copy helpers.hlsli alongside so the compile finds it.
    copyFixture(scratch, "helpers.hlsli");

    // Write a fresh shader that #includes an extra.hlsli only when USE_FAST_PATH=1.
    auto extra = scratch / "extra.hlsli";
    {
        std::ofstream f(extra, std::ios::binary);
        f << "#ifndef EXTRA_HLSLI\n#define EXTRA_HLSLI\nuint extraFn(uint x){return x+5u;}\n#endif\n";
    }

    auto shader = scratch / "partial.slang";
    {
        std::ofstream f(shader, std::ios::binary);
        f << "// [permutation] USE_FAST_PATH={0,1}\n";
        f << "#include \"helpers.hlsli\"\n";
        f << "#if USE_FAST_PATH\n#include \"extra.hlsli\"\n#endif\n";
        f << "RWStructuredBuffer<uint> uOut;\n";
        f << "[shader(\"compute\")][numthreads(1,1,1)]\n";
        f << "void main(uint3 tid : SV_DispatchThreadID){\n";
        f << "#if USE_FAST_PATH\n  uOut[tid.x] = extraFn(tid.x);\n";
        f << "#else\n  uOut[tid.x] = helperAdd(tid.x, tid.y);\n#endif\n}\n";
    }

    Compiler      c;
    BatchCompiler bc(c);
    bc.setQuiet(true);

    BatchCompiler::Input in;
    in.file                 = shader;
    in.options.target       = Target::SPIRV;
    in.options.profile      = "sm_6_5";
    in.options.includePaths = {scratch};

    auto outPath = scratch / "partial.bin";

    bc.compileFile(in, outPath);
    CHECK(bc.lastStats().compiled == 2);

    // Touch extra.hlsli — only the USE_FAST_PATH=1 permutation depends on it.
    {
        std::ofstream f(extra, std::ios::app | std::ios::binary);
        f << "\n// edited\n";
    }
    bc.compileFile(in, outPath);
    CHECK(bc.lastStats().reused == 1);
    CHECK(bc.lastStats().compiled == 1);
}

TEST_CASE("setIncremental(false) forces a full rebuild even when inputs match")
{
    auto scratch = makeScratchDir("force");
    auto src     = copyFixture(scratch, "permuted.slang");
    auto outPath = scratch / "permuted.bin";

    Compiler      c;
    BatchCompiler bc(c);
    bc.setQuiet(true);

    BatchCompiler::Input in;
    in.file            = src;
    in.options.target  = Target::SPIRV;
    in.options.profile = "sm_6_5";

    bc.compileFile(in, outPath);
    CHECK(bc.lastStats().compiled == 6);

    bc.setIncremental(false);
    bc.compileFile(in, outPath);
    CHECK(bc.lastStats().reused == 0);
    CHECK(bc.lastStats().compiled == 6);
}

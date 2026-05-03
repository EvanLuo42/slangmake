#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "slangmake.hpp"

using namespace slangmake;

namespace fs = std::filesystem;

namespace
{

fs::path makeTempDir(const char* tag)
{
    auto base = fs::temp_directory_path() / (std::string("slangmake_test_") + tag);
    fs::remove_all(base);
    fs::create_directories(base);
    return base;
}

} // namespace

TEST_CASE("BatchCompiler::compileFile expands embedded permutation directives")
{
    Compiler      c;
    BatchCompiler bc(c);
    bc.setKeepGoing(true);
    bc.setQuiet(true);

    BatchCompiler::Input in;
    in.file            = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "permuted.slang";
    in.options.target  = Target::SPIRV;
    in.options.profile = "sm_6_5";

    auto outDir  = makeTempDir("batch_file");
    auto outPath = outDir / "permuted.bin";
    auto out     = bc.compileFile(in, outPath);

    CHECK(out.outputFile == outPath);
    CHECK(fs::exists(outPath));
    CHECK(out.failures.empty());
    // 2 USE_SHADOW * 3 QUALITY = 6 permutations.
    CHECK(out.compiled.size() == 6);

    auto reader = BlobReader::openFile(outPath);
    REQUIRE(reader.has_value());
    CHECK(reader->entryCount() == 6);

    fs::remove_all(outDir);
}

TEST_CASE("BatchCompiler::compileFile applies CLI override on top of file directives")
{
    Compiler      c;
    BatchCompiler bc(c);
    bc.setKeepGoing(true);
    bc.setQuiet(true);

    BatchCompiler::Input in;
    in.file            = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "permuted.slang";
    in.options.target  = Target::SPIRV;
    in.options.profile = "sm_6_5";
    // Override USE_SHADOW with three values; QUALITY left at file value (3 values).
    in.cliOverride = {{"USE_SHADOW", {"0", "1", "2"}}};

    auto outDir  = makeTempDir("batch_cli");
    auto outPath = outDir / "permuted.bin";
    auto out     = bc.compileFile(in, outPath);

    CHECK(out.failures.empty());
    CHECK(out.compiled.size() == 9); // 3 * 3

    fs::remove_all(outDir);
}

TEST_CASE("BatchCompiler::compileFile rejects duplicate permutation axis names after merge")
{
    Compiler      c;
    BatchCompiler bc(c);
    bc.setKeepGoing(true);
    bc.setQuiet(true);

    BatchCompiler::Input in;
    in.file            = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "generic_material.slang";
    in.options.target  = Target::SPIRV;
    in.options.profile = "sm_6_5";
    in.cliOverride     = {
        {"MAT", {"Metal"}, PermutationDefine::Kind::Type},
        {"MAT", {"Wood"}, PermutationDefine::Kind::Type},
    };

    auto outDir  = makeTempDir("batch_dup_axis");
    auto outPath = outDir / "generic_material.bin";
    auto out     = bc.compileFile(in, outPath);

    REQUIRE(out.compiled.empty());
    REQUIRE(out.failures.size() == 1);
    CHECK(out.failures[0].find("duplicate permutation axis name") != std::string::npos);
    CHECK(out.failures[0].find("'MAT'") != std::string::npos);
    CHECK_FALSE(fs::exists(outPath));

    fs::remove_all(outDir);
}

TEST_CASE("BatchCompiler::compileDirectory walks *.slang and writes parallel .bin tree")
{
    auto srcDir = makeTempDir("batch_src");
    auto outDir = makeTempDir("batch_out");

    // Copy two shaders into the source tree, one nested.
    auto a    = srcDir / "a.slang";
    auto bDir = srcDir / "sub";
    fs::create_directories(bDir);
    auto b = bDir / "b.slang";
    fs::copy_file(fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "compute.slang", a);
    fs::copy_file(fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "compute.slang", b);

    Compiler      c;
    BatchCompiler bc(c);
    bc.setKeepGoing(true);
    bc.setQuiet(true);

    CompileOptions base;
    base.target  = Target::SPIRV;
    base.profile = "sm_6_5";
    auto outputs = bc.compileDirectory(srcDir, base, outDir);

    CHECK(outputs.size() == 2);
    CHECK(fs::exists(outDir / "a.bin"));
    CHECK(fs::exists(outDir / "sub" / "b.bin"));

    fs::remove_all(srcDir);
    fs::remove_all(outDir);
}

TEST_CASE("`// [noreflection]` directive suppresses reflection output per file")
{
    auto srcDir = makeTempDir("noreflection_src");
    auto outDir = makeTempDir("noreflection_out");

    auto shader = srcDir / "quiet.slang";
    {
        std::ofstream f(shader, std::ios::binary);
        f << "// [noreflection]\n";
        f << "RWStructuredBuffer<uint> uOut;\n";
        f << "[shader(\"compute\")][numthreads(1,1,1)]\n";
        f << "void main(uint3 t : SV_DispatchThreadID){ uOut[t.x] = t.x; }\n";
    }

    Compiler      c;
    BatchCompiler bc(c);
    bc.setQuiet(true);

    BatchCompiler::Input in;
    in.file            = shader;
    in.options.target  = Target::SPIRV;
    in.options.profile = "sm_6_5";

    auto outPath = outDir / "quiet.bin";
    auto out     = bc.compileFile(in, outPath);
    REQUIRE(out.failures.empty());
    REQUIRE(out.compiled.size() == 1);

    auto reader = BlobReader::openFile(outPath);
    REQUIRE(reader.has_value());
    auto entry = reader->at(0);
    CHECK(entry.code.size() >= 4); // SPIR-V still emitted
    CHECK(entry.reflection.empty());

    fs::remove_all(srcDir);
    fs::remove_all(outDir);
}

TEST_CASE("BatchCompiler reports failure when compilation fails (no --keep-going)")
{
    auto srcDir = makeTempDir("batch_fail_src");
    auto outDir = makeTempDir("batch_fail_out");

    auto bad = srcDir / "broken.slang";
    {
        std::ofstream f(bad, std::ios::binary);
        f << "this is not valid slang code !!!\n";
    }

    Compiler      c;
    BatchCompiler bc(c);
    bc.setQuiet(true);

    BatchCompiler::Input in;
    in.file            = bad;
    in.options.target  = Target::SPIRV;
    in.options.profile = "sm_6_5";
    auto out           = bc.compileFile(in, outDir / "broken.bin");

    CHECK_FALSE(out.failures.empty());
    CHECK(out.compiled.empty());

    fs::remove_all(srcDir);
    fs::remove_all(outDir);
}

TEST_CASE("setKeepGoing controls whether later permutations run after a mid-sequence failure")
{
    auto srcDir = makeTempDir("batch_keepgoing_src");
    auto outDir = makeTempDir("batch_keepgoing_out");

    // Three permutations; the middle one trips #error and fails.
    auto shader = srcDir / "tri.slang";
    {
        std::ofstream f(shader, std::ios::binary);
        f << "// [permutation] MODE={0,1,2}\n";
        f << "#if MODE == 1\n#error \"intentional failure for MODE=1\"\n#endif\n";
        f << "RWStructuredBuffer<uint> uOut;\n";
        f << "[shader(\"compute\")][numthreads(1,1,1)]\n";
        f << "void main(uint3 t : SV_DispatchThreadID){ uOut[t.x] = MODE; }\n";
    }

    BatchCompiler::Input in;
    in.file            = shader;
    in.options.target  = Target::SPIRV;
    in.options.profile = "sm_6_5";

    {
        Compiler      c;
        BatchCompiler bc(c);
        bc.setQuiet(true);
        bc.setKeepGoing(false);
        auto out = bc.compileFile(in, outDir / "stop.bin");
        // MODE=0 succeeds, MODE=1 fails and we bail — MODE=2 never makes it in.
        CHECK(out.failures.size() == 1);
        CHECK(out.compiled.size() == 1);
    }

    {
        Compiler      c;
        BatchCompiler bc(c);
        bc.setQuiet(true);
        bc.setKeepGoing(true);
        auto out = bc.compileFile(in, outDir / "go.bin");
        // Failure is still reported, but MODE=0 and MODE=2 both make it in.
        CHECK(out.failures.size() == 1);
        CHECK(out.compiled.size() == 2);
    }

    fs::remove_all(srcDir);
    fs::remove_all(outDir);
}

TEST_CASE("compileDirectory applies cliOverride uniformly to every discovered file")
{
    auto srcDir = makeTempDir("batch_dir_cli_src");
    auto outDir = makeTempDir("batch_dir_cli_out");

    // permuted.slang declares USE_SHADOW={0,1} × QUALITY={0,1,2} = 6 perms.
    // CLI override extends USE_SHADOW to 3 values → 3 × 3 = 9 per file.
    fs::copy_file(fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "permuted.slang", srcDir / "a.slang");
    auto sub = srcDir / "sub";
    fs::create_directories(sub);
    fs::copy_file(fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "permuted.slang", sub / "b.slang");

    Compiler      c;
    BatchCompiler bc(c);
    bc.setQuiet(true);

    CompileOptions base;
    base.target  = Target::SPIRV;
    base.profile = "sm_6_5";

    std::vector<PermutationDefine> cli     = {{"USE_SHADOW", {"0", "1", "2"}}};
    auto                           outputs = bc.compileDirectory(srcDir, base, outDir, cli);

    REQUIRE(outputs.size() == 2);
    for (const auto& o : outputs)
    {
        CHECK(o.failures.empty());
        CHECK(o.compiled.size() == 9);
    }

    fs::remove_all(srcDir);
    fs::remove_all(outDir);
}

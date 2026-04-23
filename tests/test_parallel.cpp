#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <thread>

#include "slangmake.h"

using namespace slangmake;
namespace fs = std::filesystem;

namespace
{

std::vector<std::vector<uint8_t>> collectCodes(BlobReader& reader)
{
    std::vector<std::vector<uint8_t>> out;
    out.reserve(reader.entryCount());
    for (size_t i = 0; i < reader.entryCount(); ++i)
    {
        auto e = reader.at(i);
        out.emplace_back(e.code.begin(), e.code.end());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> collectKeys(BlobReader& reader)
{
    auto v = reader.enumerate();
    std::sort(v.begin(), v.end());
    return v;
}

} // namespace

TEST_CASE("BatchCompiler parallel compilation produces the same blob as serial")
{
    const int hw = std::max(2u, std::thread::hardware_concurrency());

    auto outDir = fs::temp_directory_path() / "slangmake_parallel_test";
    fs::remove_all(outDir);
    fs::create_directories(outDir);

    Compiler      c;
    BatchCompiler bc(c);
    bc.setQuiet(true);

    BatchCompiler::Input in;
    in.file            = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "permuted.slang";
    in.options.target  = Target::SPIRV;
    in.options.profile = "sm_6_5";

    // Serial
    bc.setJobs(1);
    auto serialPath = outDir / "serial.bin";
    auto serialOut  = bc.compileFile(in, serialPath);
    REQUIRE(serialOut.failures.empty());
    REQUIRE(serialOut.compiled.size() == 6);

    auto serialReader = BlobReader::openFile(serialPath);
    REQUIRE(serialReader.has_value());
    auto serialKeys  = collectKeys(*serialReader);
    auto serialCodes = collectCodes(*serialReader);

    // Parallel
    bc.setJobs(hw);
    auto parallelPath = outDir / "parallel.bin";
    auto parallelOut  = bc.compileFile(in, parallelPath);
    REQUIRE(parallelOut.failures.empty());
    REQUIRE(parallelOut.compiled.size() == 6);

    auto parallelReader = BlobReader::openFile(parallelPath);
    REQUIRE(parallelReader.has_value());
    auto parallelKeys  = collectKeys(*parallelReader);
    auto parallelCodes = collectCodes(*parallelReader);

    // Both runs must produce exactly the same set of permutation keys and
    // exactly the same set of SPIRV byte-vectors (order in the blob may differ
    // under parallel execution).
    CHECK(parallelKeys == serialKeys);
    CHECK(parallelCodes == serialCodes);

    fs::remove_all(outDir);
}

TEST_CASE("BatchCompiler jobs=N with fewer permutations than threads still works")
{
    auto outDir = fs::temp_directory_path() / "slangmake_parallel_small";
    fs::remove_all(outDir);
    fs::create_directories(outDir);

    Compiler      c;
    BatchCompiler bc(c);
    bc.setQuiet(true);
    bc.setJobs(8);

    BatchCompiler::Input in;
    in.file            = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "compute.slang";
    in.options.target  = Target::SPIRV;
    in.options.profile = "sm_6_5";
    auto outPath       = outDir / "compute.bin";
    auto out           = bc.compileFile(in, outPath);

    REQUIRE(out.failures.empty());
    REQUIRE(out.compiled.size() == 1);

    auto reader = BlobReader::openFile(outPath);
    REQUIRE(reader.has_value());
    CHECK(reader->entryCount() == 1);

    fs::remove_all(outDir);
}

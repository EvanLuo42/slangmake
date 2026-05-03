#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

#include "slangmake.hpp"

#ifndef SLANG_MAKE_TESTS_CLI_EXE
#error "SLANG_MAKE_TESTS_CLI_EXE must be defined by the build"
#endif
#ifndef SLANG_MAKE_TESTS_SHADER_DIR
#error "SLANG_MAKE_TESTS_SHADER_DIR must be defined by the build"
#endif

namespace fs = std::filesystem;
using namespace slangmake;

namespace
{

std::string uniqueSuffix()
{
    static std::atomic<uint64_t> counter{0};
    std::random_device           rd;
    return std::to_string(counter.fetch_add(1)) + "_" + std::to_string(static_cast<uint64_t>(rd()));
}

struct CliResult
{
    int         exitCode = -1;
    std::string output;
};

// Runs the slangmake CLI binary, capturing stdout+stderr into a string. Uses
// std::system + a temp redirect file so this works on both MSVC (cmd.exe) and
// POSIX shells without pulling in a process library.
CliResult runCli(const std::vector<std::string>& args)
{
    auto logPath = fs::temp_directory_path() / ("slangmake_cli_" + uniqueSuffix() + ".log");

    std::string cmd;
    cmd.reserve(256);
    cmd += '"';
    cmd += SLANG_MAKE_TESTS_CLI_EXE;
    cmd += '"';
    for (const auto& a : args)
    {
        cmd += ' ';
        cmd += '"';
        cmd += a;
        cmd += '"';
    }
    cmd += " > \"";
    cmd += logPath.string();
    cmd += "\" 2>&1";

#ifdef _WIN32
    // cmd.exe strips a single pair of outer quotes from the whole command
    // string. Wrap in an extra pair so the executable-path quotes survive.
    std::string invoked = "\"" + cmd + "\"";
#else
    std::string invoked = cmd;
#endif

    int rc = std::system(invoked.c_str());
#if !defined(_WIN32)
    if (WIFEXITED(rc))
        rc = WEXITSTATUS(rc);
#endif

    std::string   out;
    std::ifstream in(logPath, std::ios::binary);
    if (in)
    {
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
    }
    std::error_code ec;
    fs::remove(logPath, ec);
    return {rc, std::move(out)};
}

fs::path uniqueTempDir(const std::string& tag)
{
    auto p = fs::temp_directory_path() / ("slangmake_cli_" + tag + "_" + uniqueSuffix());
    fs::create_directories(p);
    return p;
}

} // namespace

TEST_CASE("CLI --version exits 0 and prints a version string")
{
    auto r = runCli({"--version"});
    CHECK(r.exitCode == 0);
    CHECK_FALSE(r.output.empty());
}

TEST_CASE("CLI with no args is a usage error")
{
    auto r = runCli({});
    // CLI11 prints usage and exits with a non-zero code when required flags
    // are missing.
    CHECK(r.exitCode != 0);
}

TEST_CASE("CLI rejects an unknown --target value")
{
    auto r = runCli({"-i", "x.slang", "-o", "x.bin", "-t", "NOT_A_TARGET", "-p", "sm_6_5"});
    CHECK(r.exitCode != 0);
}

TEST_CASE("CLI compiles compute.slang and --dump decodes it")
{
    auto outDir = uniqueTempDir("compile");
    auto outBin = outDir / "compute.bin";
    auto input  = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "compute.slang";

    auto r = runCli({"-i", input.string(), "-o", outBin.string(), "-t", "SPIRV", "-p", "sm_6_5", "-q"});
    INFO("compile output:\n" << r.output);
    REQUIRE(r.exitCode == 0);
    REQUIRE(fs::exists(outBin));

    auto dump = runCli({"--dump", outBin.string()});
    INFO("dump output:\n" << dump.output);
    CHECK(dump.exitCode == 0);
    CHECK(dump.output.find("entry count") != std::string::npos);
    CHECK(dump.output.find("SPIRV") != std::string::npos);

    std::error_code ec;
    fs::remove_all(outDir, ec);
}

TEST_CASE("CLI --dump on a missing file exits non-zero")
{
    auto r = runCli({"--dump", "definitely/not/a/real/path.bin"});
    CHECK(r.exitCode != 0);
}

TEST_CASE("CLI --dump on a malformed blob exits non-zero")
{
    auto outDir = uniqueTempDir("malformed");
    auto bad    = outDir / "bad.bin";
    {
        std::ofstream out(bad, std::ios::binary);
        const char    garbage[] = "XXXXnotablob";
        out.write(garbage, sizeof(garbage) - 1);
    }
    auto r = runCli({"--dump", bad.string()});
    CHECK(r.exitCode != 0);

    std::error_code ec;
    fs::remove_all(outDir, ec);
}

TEST_CASE("CLI expands file-declared permutations into one entry per cell")
{
    auto outDir = uniqueTempDir("perm_file");
    auto outBin = outDir / "permuted.bin";
    auto input  = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "permuted.slang";

    auto r = runCli({"-i", input.string(), "-o", outBin.string(), "-t", "SPIRV", "-p", "sm_6_5", "-q"});
    INFO("compile output:\n" << r.output);
    REQUIRE(r.exitCode == 0);

    auto reader = BlobReader::openFile(outBin);
    REQUIRE(reader.has_value());
    REQUIRE(reader->valid());
    // USE_SHADOW={0,1} x QUALITY={0,1,2} = 6 entries.
    CHECK(reader->entryCount() == 6);

    std::error_code ec;
    fs::remove_all(outDir, ec);
}

TEST_CASE("CLI -P overrides a file-declared permutation axis")
{
    auto outDir = uniqueTempDir("perm_override");
    auto outBin = outDir / "permuted.bin";
    auto input  = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "permuted.slang";

    auto r = runCli({"-i", input.string(), "-o", outBin.string(), "-t", "SPIRV", "-p", "sm_6_5", "-P", "USE_SHADOW={1}",
                     "-P", "QUALITY={2}", "-q"});
    INFO("compile output:\n" << r.output);
    REQUIRE(r.exitCode == 0);

    auto reader = BlobReader::openFile(outBin);
    REQUIRE(reader.has_value());
    REQUIRE(reader->valid());
    CHECK(reader->entryCount() == 1);

    std::error_code ec;
    fs::remove_all(outDir, ec);
}

TEST_CASE("CLI --dump-reflection decodes entry points")
{
    auto outDir = uniqueTempDir("dump_refl");
    auto outBin = outDir / "compute.bin";
    auto input  = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "compute.slang";

    auto compile = runCli({"-i", input.string(), "-o", outBin.string(), "-t", "SPIRV", "-p", "sm_6_5", "-q"});
    REQUIRE(compile.exitCode == 0);

    auto r = runCli({"--dump", outBin.string(), "--dump-reflection"});
    INFO("dump output:\n" << r.output);
    CHECK(r.exitCode == 0);
    CHECK(r.output.find("entry points") != std::string::npos);

    std::error_code ec;
    fs::remove_all(outDir, ec);
}

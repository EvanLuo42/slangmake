#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "slangmake.h"

using namespace slangmake;
namespace fs = std::filesystem;

namespace {

fs::path scratch(const char* tag)
{
    auto p = fs::temp_directory_path() / (std::string("slangmake_codec_") + tag);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

// Compile permuted.slang with a given codec and return the on-disk file size
// plus all extracted SPIRV bytes, keyed by permutation.
struct CompiledSet
{
    size_t                                        fileSize = 0;
    std::map<std::string, std::vector<uint8_t>>   codeByKey;
    std::map<std::string, std::vector<uint8_t>>   reflByKey;
};

CompiledSet compileWith(Codec codec, const fs::path& dir, const char* fileName)
{
    Compiler      c;
    BatchCompiler bc(c);
    bc.setQuiet(true);
    bc.setCompression(codec);
    bc.setIncremental(false);

    BatchCompiler::Input in;
    in.file            = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "permuted.slang";
    in.options.target  = Target::SPIRV;
    in.options.profile = "sm_6_5";

    auto outPath = dir / fileName;
    auto out     = bc.compileFile(in, outPath);
    REQUIRE(out.failures.empty());

    CompiledSet s;
    s.fileSize = fs::file_size(outPath);

    auto reader = BlobReader::openFile(outPath);
    REQUIRE(reader.has_value());
    REQUIRE(reader->valid());
    for (size_t i = 0; i < reader->entryCount(); ++i) {
        auto e = reader->at(i);
        s.codeByKey[std::string(e.key)] =
            std::vector<uint8_t>(e.code.begin(), e.code.end());
        s.reflByKey[std::string(e.key)] =
            std::vector<uint8_t>(e.reflection.begin(), e.reflection.end());
    }
    return s;
}

} // namespace

TEST_CASE("codecToString / parseCodec round-trip")
{
    CHECK(std::string(codecToString(Codec::None)) == "none");
    CHECK(std::string(codecToString(Codec::LZ4))  == "lz4");
    CHECK(std::string(codecToString(Codec::Zstd)) == "zstd");

    CHECK(parseCodec("none").value() == Codec::None);
    CHECK(parseCodec("LZ4").value()  == Codec::LZ4);
    CHECK(parseCodec("zstd").value() == Codec::Zstd);
    CHECK(parseCodec("zst").value()  == Codec::Zstd);
    CHECK_FALSE(parseCodec("bogus").has_value());

    // Empty input maps to Codec::None (documented behaviour).
    CHECK(parseCodec("").value() == Codec::None);
}

TEST_CASE("LZ4 compression shrinks the blob and decodes identically")
{
    auto dir  = scratch("lz4");
    auto uncx = compileWith(Codec::None, dir, "none.bin");
    auto cx   = compileWith(Codec::LZ4,  dir, "lz4.bin");

    CHECK(cx.fileSize < uncx.fileSize);
    CHECK(cx.codeByKey == uncx.codeByKey);
    CHECK(cx.reflByKey == uncx.reflByKey);
}

TEST_CASE("Zstd compression shrinks the blob and decodes identically")
{
    auto dir  = scratch("zstd");
    auto uncx = compileWith(Codec::None, dir, "none.bin");
    auto cx   = compileWith(Codec::Zstd, dir, "zstd.bin");

    CHECK(cx.fileSize < uncx.fileSize);
    CHECK(cx.codeByKey == uncx.codeByKey);
    CHECK(cx.reflByKey == uncx.reflByKey);
}

TEST_CASE("Mixing codecs across rebuilds still produces correct bytes")
{
    auto dir = scratch("mix");

    // Produce with LZ4, decode with the reader — reader auto-picks the codec.
    Compiler      c;
    BatchCompiler bc(c);
    bc.setQuiet(true);
    bc.setCompression(Codec::LZ4);
    bc.setIncremental(false);

    BatchCompiler::Input in;
    in.file            = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "compute.slang";
    in.options.target  = Target::SPIRV;
    in.options.profile = "sm_6_5";

    auto lz4Path = dir / "a.bin";
    bc.compileFile(in, lz4Path);
    auto lz4Reader = BlobReader::openFile(lz4Path);
    REQUIRE(lz4Reader.has_value());
    REQUIRE(lz4Reader->entryCount() == 1);

    bc.setCompression(Codec::Zstd);
    auto zstdPath = dir / "b.bin";
    bc.compileFile(in, zstdPath);
    auto zstdReader = BlobReader::openFile(zstdPath);
    REQUIRE(zstdReader.has_value());
    REQUIRE(zstdReader->entryCount() == 1);

    auto a = lz4Reader->at(0);
    auto b = zstdReader->at(0);
    std::vector<uint8_t> ca(a.code.begin(), a.code.end());
    std::vector<uint8_t> cb(b.code.begin(), b.code.end());
    CHECK(ca == cb);
}

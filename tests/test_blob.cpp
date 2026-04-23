#include <doctest/doctest.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "slangmake.h"

using namespace slangmake;

namespace
{

std::vector<uint8_t> bytes(std::initializer_list<uint8_t> v) { return std::vector<uint8_t>(v); }

Permutation perm(std::initializer_list<std::pair<std::string, std::string>> kv)
{
    Permutation p;
    for (auto& e : kv)
        p.constants.push_back({e.first, e.second});
    return p;
}

} // namespace

TEST_CASE("BlobWriter::addEntry + entryCount + finalize header")
{
    BlobWriter w(Target::SPIRV);
    CHECK(w.entryCount() == 0);
    w.addEntry(perm({}), bytes({1, 2, 3, 4}), bytes({}));
    w.addEntry(perm({{"A", "1"}}), bytes({9}), bytes({}));
    CHECK(w.entryCount() == 2);

    auto blob = w.finalize();
    REQUIRE(blob.size() >= sizeof(fmt::BlobHeader));
    const auto* hdr = reinterpret_cast<const fmt::BlobHeader*>(blob.data());
    CHECK(hdr->magic == fmt::kBlobMagic);
    CHECK(hdr->version == fmt::kBlobVersion);
    CHECK(hdr->entryCount == 2);
    CHECK(hdr->target == static_cast<uint32_t>(SLANG_SPIRV));
}

TEST_CASE("BlobReader round-trips entries written by BlobWriter")
{
    BlobWriter w(Target::DXIL);
    auto       code1 = bytes({0xDE, 0xAD, 0xBE, 0xEF});
    auto       code2 = bytes({0x01, 0x02, 0x03});
    auto       refl1 = bytes({'R', '1'});
    auto       refl2 = bytes({'R', 'E', 'F', '2'});
    w.addEntry(perm({{"X", "0"}}), code1, refl1);
    w.addEntry(perm({{"X", "1"}}), code2, refl2);
    auto blob = w.finalize();

    BlobReader r(blob);
    REQUIRE(r.valid());
    CHECK(r.target() == Target::DXIL);
    REQUIRE(r.entryCount() == 2);

    auto e0 = r.at(0);
    CHECK(e0.key == "X=0");
    CHECK(std::vector<uint8_t>(e0.code.begin(), e0.code.end()) == code1);
    CHECK(std::vector<uint8_t>(e0.reflection.begin(), e0.reflection.end()) == refl1);

    auto e1 = r.at(1);
    CHECK(e1.key == "X=1");
    CHECK(std::vector<uint8_t>(e1.code.begin(), e1.code.end()) == code2);
    CHECK(std::vector<uint8_t>(e1.reflection.begin(), e1.reflection.end()) == refl2);
}

TEST_CASE("BlobReader::find locates by ShaderConstant set in any order")
{
    BlobWriter w(Target::SPIRV);
    w.addEntry(perm({{"A", "1"}, {"B", "2"}}), bytes({0xAA}), bytes({}));
    w.addEntry(perm({{"A", "9"}, {"B", "8"}}), bytes({0xBB}), bytes({}));
    auto       blob = w.finalize();
    BlobReader r(blob);

    std::array<ShaderConstant, 2> q{ShaderConstant{"B", "2"}, ShaderConstant{"A", "1"}};
    auto                          found = r.find(std::span<const ShaderConstant>(q.data(), q.size()));
    REQUIRE(found.has_value());
    REQUIRE(found->code.size() == 1);
    CHECK(found->code[0] == 0xAA);
}

TEST_CASE("BlobReader::find returns nullopt when no entry matches")
{
    BlobWriter w(Target::SPIRV);
    w.addEntry(perm({{"X", "0"}}), bytes({1}), bytes({}));
    auto                          blob = w.finalize();
    BlobReader                    r(blob);
    std::array<ShaderConstant, 1> q{ShaderConstant{"X", "9"}};
    CHECK_FALSE(r.find(std::span<const ShaderConstant>(q.data(), q.size())).has_value());
}

TEST_CASE("BlobWriter deduplicates identical reflection payloads")
{
    BlobWriter w(Target::SPIRV);
    auto       code1      = bytes({0x11});
    auto       code2      = bytes({0x22});
    auto       code3      = bytes({0x33});
    auto       sharedRefl = bytes({'R', 'E', 'F', 'L', 0, 1, 2, 3});
    auto       uniqueRefl = bytes({'X', 'Y', 'Z'});

    w.addEntry(perm({{"K", "a"}}), code1, sharedRefl);
    w.addEntry(perm({{"K", "b"}}), code2, uniqueRefl);
    w.addEntry(perm({{"K", "c"}}), code3, sharedRefl); // byte-identical to entry 0

    auto blob = w.finalize();

    const auto* hdr     = reinterpret_cast<const fmt::BlobHeader*>(blob.data());
    const auto* records = reinterpret_cast<const fmt::EntryRecord*>(blob.data() + sizeof(fmt::BlobHeader));

    // Entries 0 and 2 must point at the same reflection bytes.
    CHECK(records[0].reflOffset == records[2].reflOffset);
    CHECK(records[0].reflSize == records[2].reflSize);
    // Entry 1 has distinct content and must live at a different offset.
    CHECK(records[1].reflOffset != records[0].reflOffset);

    // Reader still returns the correct bytes for every entry.
    BlobReader r(blob);
    REQUIRE(r.valid());
    auto e0 = r.at(0);
    auto e1 = r.at(1);
    auto e2 = r.at(2);
    CHECK(std::vector<uint8_t>(e0.reflection.begin(), e0.reflection.end()) == sharedRefl);
    CHECK(std::vector<uint8_t>(e1.reflection.begin(), e1.reflection.end()) == uniqueRefl);
    CHECK(std::vector<uint8_t>(e2.reflection.begin(), e2.reflection.end()) == sharedRefl);
}

TEST_CASE("BlobReader::enumerate returns keys in insertion order")
{
    BlobWriter w(Target::SPIRV);
    w.addEntry(perm({{"K", "a"}}), bytes({1}), bytes({}));
    w.addEntry(perm({{"K", "b"}}), bytes({2}), bytes({}));
    w.addEntry(perm({{"K", "c"}}), bytes({3}), bytes({}));
    auto       blob = w.finalize();
    BlobReader r(blob);
    auto       keys = r.enumerate();
    CHECK(keys == std::vector<std::string>{"K=a", "K=b", "K=c"});
}

TEST_CASE("BlobReader rejects malformed buffers")
{
    BlobReader bad(std::span<const uint8_t>{});
    CHECK_FALSE(bad.valid());
    CHECK(bad.entryCount() == 0);

    auto       buf = bytes({'X', 'X', 'X', 'X'});
    BlobReader bad2{std::span<const uint8_t>(buf)};
    CHECK_FALSE(bad2.valid());
}

TEST_CASE("BlobWriter::writeToFile + BlobReader::openFile round-trip")
{
    BlobWriter w(Target::WGSL);
    w.addEntry(perm({{"P", "v"}}), bytes({0x55, 0x66}), bytes({0x77}));
    auto path = std::filesystem::temp_directory_path() / "slangmake_blob_io.bin";
    w.writeToFile(path);

    auto opened = BlobReader::openFile(path);
    REQUIRE(opened.has_value());
    CHECK(opened->valid());
    CHECK(opened->target() == Target::WGSL);
    REQUIRE(opened->entryCount() == 1);
    auto e = opened->at(0);
    CHECK(e.key == "P=v");
    CHECK(std::vector<uint8_t>(e.code.begin(), e.code.end()) == bytes({0x55, 0x66}));
    CHECK(std::vector<uint8_t>(e.reflection.begin(), e.reflection.end()) == bytes({0x77}));

    std::filesystem::remove(path);
}

TEST_CASE("BlobReader::openFile returns nullopt on missing file")
{
    auto opened = BlobReader::openFile("/no/such/file/should/exist.bin");
    CHECK_FALSE(opened.has_value());
}

TEST_CASE("BlobWriter::setDependencies round-trips through BlobReader::dependencies")
{
    BlobWriter w(Target::SPIRV);
    w.addEntry(perm({{"K", "a"}}), bytes({1}), bytes({}));
    std::vector<BlobWriter::DepInfo> deps = {
        {"a.hlsli", 0x1111222233334444ull},
        {"sub/b.hlsli", 0xAABBCCDDEEFF0011ull},
    };
    w.setDependencies(deps);
    auto blob = w.finalize();

    BlobReader r(blob);
    REQUIRE(r.valid());
    auto got = r.dependencies();
    REQUIRE(got.size() == 2);
    CHECK(got[0].path == "a.hlsli");
    CHECK(got[0].contentHash == 0x1111222233334444ull);
    CHECK(got[1].path == "sub/b.hlsli");
    CHECK(got[1].contentHash == 0xAABBCCDDEEFF0011ull);
}

TEST_CASE("BlobWriter::setOptionsHash round-trips through BlobReader::optionsHash")
{
    BlobWriter w(Target::SPIRV);
    w.setOptionsHash(0xDEADBEEFCAFEBABEull);
    w.addEntry(perm({}), bytes({1}), bytes({}));
    auto blob = w.finalize();

    BlobReader r(blob);
    REQUIRE(r.valid());
    CHECK(r.optionsHash() == 0xDEADBEEFCAFEBABEull);
}

TEST_CASE("BlobWriter::addEntry preserves depIndices through BlobReader")
{
    BlobWriter w(Target::SPIRV);
    w.setDependencies({{"a.hlsli", 0x11ull}, {"b.hlsli", 0x22ull}, {"c.hlsli", 0x33ull}});

    std::array<uint32_t, 2> idxA{0u, 2u};
    std::array<uint32_t, 1> idxB{1u};
    w.addEntry(perm({{"K", "a"}}), bytes({1}), bytes({}), std::span<const uint32_t>(idxA.data(), idxA.size()));
    w.addEntry(perm({{"K", "b"}}), bytes({2}), bytes({}), std::span<const uint32_t>(idxB.data(), idxB.size()));

    auto       blob = w.finalize();
    BlobReader r(blob);
    REQUIRE(r.valid());

    auto e0 = r.at(0);
    auto e1 = r.at(1);
    REQUIRE(e0.depIndices.size() == 2);
    CHECK(e0.depIndices[0] == 0u);
    CHECK(e0.depIndices[1] == 2u);
    REQUIRE(e1.depIndices.size() == 1);
    CHECK(e1.depIndices[0] == 1u);
}

TEST_CASE("BlobReader::at returns an empty entry for out-of-range index")
{
    BlobWriter w(Target::SPIRV);
    w.addEntry(perm({{"K", "a"}}), bytes({1}), bytes({}));
    auto       blob = w.finalize();
    BlobReader r(blob);
    REQUIRE(r.valid());

    auto e = r.at(42);
    CHECK(e.key.empty());
    CHECK(e.code.empty());
    CHECK(e.reflection.empty());
    CHECK(e.depIndices.empty());
}

TEST_CASE("BlobWriter::setCompression round-trips via BlobReader auto-decompress")
{
    BlobWriter w(Target::SPIRV);
    w.setCompression(Codec::LZ4);
    auto code = bytes({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
    auto refl = bytes({'R', 'E', 'F', 'L'});
    w.addEntry(perm({{"K", "a"}}), code, refl);
    auto blob = w.finalize();

    BlobReader r(blob);
    REQUIRE(r.valid());
    // After transparent decompression the in-memory header reads as None.
    CHECK(r.compression() == Codec::None);

    auto e = r.at(0);
    CHECK(std::vector<uint8_t>(e.code.begin(), e.code.end()) == code);
    CHECK(std::vector<uint8_t>(e.reflection.begin(), e.reflection.end()) == refl);
}

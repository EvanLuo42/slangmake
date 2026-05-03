#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "slangmake.hpp"

// Runtime-side internal header. Contains only the helpers that the read-side
// (BlobReader, ReflectionView, parsers, compression glue) needs. Pulling this
// header in MUST NOT pull in slang.h, dxc, or any other compile-time-only
// dependency, so the slangmake-rt.dll target can be built without those.

namespace slangmake::detail
{

constexpr size_t align8(size_t n) { return (n + 7u) & ~static_cast<size_t>(7); }
constexpr size_t align4(size_t n) { return (n + 3u) & ~static_cast<size_t>(3); }

inline void padTo8(std::vector<uint8_t>& out)
{
    while (out.size() % 8)
        out.push_back(0);
}
inline void padTo4(std::vector<uint8_t>& out)
{
    while (out.size() % 4)
        out.push_back(0);
}

template <class T>
void appendBytes(std::vector<uint8_t>& out, const T& v)
{
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}
inline void appendBytes(std::vector<uint8_t>& out, const void* data, size_t n)
{
    const auto* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + n);
}

constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
constexpr uint64_t kFnvPrime  = 0x00000100000001B3ull;

inline void fnv1aMix(uint64_t& h, const void* data, size_t n)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i)
    {
        h ^= p[i];
        h *= kFnvPrime;
    }
}
template <class T>
void fnv1aMixPod(uint64_t& h, const T& v)
{
    fnv1aMix(h, &v, sizeof(T));
}
inline void fnv1aMixStr(uint64_t& h, std::string_view s)
{
    auto n = static_cast<uint32_t>(s.size());
    fnv1aMixPod(h, n);
    fnv1aMix(h, s.data(), s.size());
}

uint64_t hashFileContents(const std::filesystem::path& path);
uint64_t hashCompileOptions(const CompileOptions& o);

std::optional<std::vector<std::string>> parsePermutationValueList(std::string_view inside, bool allowAngleNesting);

std::vector<uint8_t> compressPayload(Codec codec, std::span<const uint8_t> in);
std::vector<uint8_t> decompressPayload(Codec codec, std::span<const uint8_t> in, size_t outSize);

// Encode a Target into / decode it from the u32 stored in BlobHeader::target.
// The integer values match Slang's SlangCompileTarget enum so blobs written by
// older versions (which encoded SlangCompileTarget directly) keep round-tripping;
// duplicating the table here lets the runtime DLL parse blobs without linking
// slang. The compiler-side TU static_asserts the values stay in sync.
uint32_t targetToBlobCode(Target t);
Target   blobCodeToTarget(uint32_t code);

} // namespace slangmake::detail

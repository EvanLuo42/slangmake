#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "slangmake.h"

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

/**
 * FNV-1a 64 hash of a file's contents.
 *
 * @param path path to hash
 * @return     hash, or 0 if the file cannot be opened
 */
uint64_t hashFileContents(const std::filesystem::path& path);

/**
 * Stable fingerprint of every CompileOption field that contributes to bytecode
 * identity across permutations (targets, defines, includes, codegen flags, ...).
 *
 * @param o options to hash
 * @return  64-bit fingerprint
 */
uint64_t hashCompileOptions(const CompileOptions& o);

/**
 * Split a `{a,b,c}`-style permutation value list on top-level commas only.
 * Nested `()`, `[]`, `{}`, `<>`, and quoted strings are preserved.
 *
 * @param inside the substring between the outer braces
 * @return       trimmed values, or std::nullopt on unbalanced nesting / quotes
 */
std::optional<std::vector<std::string>> parsePermutationValueList(std::string_view inside, bool allowAngleNesting);

/**
 * Run `codec` over `in` and return the compressed bytes. Codec::None copies.
 *
 * @throws std::runtime_error if the underlying codec call fails
 */
std::vector<uint8_t> compressPayload(Codec codec, std::span<const uint8_t> in);

/**
 * Inverse of compressPayload. `outSize` must be the exact uncompressed size.
 *
 * @throws std::runtime_error on decode failure or size mismatch
 */
std::vector<uint8_t> decompressPayload(Codec codec, std::span<const uint8_t> in, size_t outSize);

/**
 * Walk Slang's reflection API for one linked program and pack the result into
 * the custom binary layout used by ReflectionView.
 *
 * @param gs          owning global session (for hashing helpers)
 * @param program     linked component from which layout() is queried
 * @param module      front-end module to walk for the decl tree; may be null
 * @param targetIndex target index inside the program (typically 0)
 * @return            serialised reflection section, ready to embed in a blob
 */
std::vector<uint8_t> serializeReflection(slang::IGlobalSession* gs, slang::IComponentType* program,
                                         slang::IModule* module, int targetIndex);

} // namespace slangmake::detail

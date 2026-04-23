#include <lz4.h>
#include <zstd.h>

#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "slangmake_internal.h"

namespace slangmake::detail
{

std::vector<uint8_t> compressPayload(Codec codec, std::span<const uint8_t> in)
{
    std::vector<uint8_t> out;
    if (codec == Codec::None || in.empty())
    {
        out.assign(in.begin(), in.end());
        return out;
    }
    if (codec == Codec::LZ4)
    {
        int inSize = static_cast<int>(in.size());
        int bound  = LZ4_compressBound(inSize);
        out.resize(static_cast<size_t>(bound));
        int n = LZ4_compress_default(reinterpret_cast<const char*>(in.data()), reinterpret_cast<char*>(out.data()),
                                     inSize, bound);
        if (n <= 0)
            throw std::runtime_error("LZ4_compress_default failed");
        out.resize(static_cast<size_t>(n));
        return out;
    }
    if (codec == Codec::Zstd)
    {
        size_t bound = ZSTD_compressBound(in.size());
        out.resize(bound);
        size_t n = ZSTD_compress(out.data(), bound, in.data(), in.size(), ZSTD_CLEVEL_DEFAULT);
        if (ZSTD_isError(n))
            throw std::runtime_error(std::string("ZSTD_compress failed: ") + ZSTD_getErrorName(n));
        out.resize(n);
        return out;
    }
    throw std::runtime_error("unknown codec");
}

std::vector<uint8_t> decompressPayload(Codec codec, std::span<const uint8_t> in, size_t outSize)
{
    std::vector<uint8_t> out(outSize);
    if (codec == Codec::None)
    {
        if (in.size() != outSize)
            throw std::runtime_error("uncompressed size mismatch");
        std::memcpy(out.data(), in.data(), outSize);
        return out;
    }
    if (codec == Codec::LZ4)
    {
        int n = LZ4_decompress_safe(reinterpret_cast<const char*>(in.data()), reinterpret_cast<char*>(out.data()),
                                    static_cast<int>(in.size()), static_cast<int>(outSize));
        if (n < 0 || static_cast<size_t>(n) != outSize)
            throw std::runtime_error("LZ4_decompress_safe failed");
        return out;
    }
    if (codec == Codec::Zstd)
    {
        size_t n = ZSTD_decompress(out.data(), outSize, in.data(), in.size());
        if (ZSTD_isError(n) || n != outSize)
            throw std::runtime_error(std::string("ZSTD_decompress failed: ") +
                                     (ZSTD_isError(n) ? ZSTD_getErrorName(n) : "size mismatch"));
        return out;
    }
    throw std::runtime_error("unknown codec");
}

} // namespace slangmake::detail

namespace slangmake
{

BlobWriter::BlobWriter(Target target)
    : m_target(target)
{
}

void BlobWriter::addEntry(const Permutation&        perm,
                          std::span<const uint8_t>  code,
                          std::span<const uint8_t>  reflection,
                          std::span<const uint32_t> depIndices)
{
    Entry e;
    e.key = perm.key();
    e.code.assign(code.begin(), code.end());
    e.refl.assign(reflection.begin(), reflection.end());
    e.depIndices.assign(depIndices.begin(), depIndices.end());
    m_entries.push_back(std::move(e));
}

std::vector<uint8_t> BlobWriter::finalize() const
{
    using namespace detail;
    std::vector<uint8_t> out;

    fmt::BlobHeader hdr{};
    hdr.magic       = fmt::kBlobMagic;
    hdr.version     = fmt::kBlobVersion;
    hdr.target      = static_cast<uint32_t>(toSlangCompileTarget(m_target));
    hdr.entryCount  = static_cast<uint32_t>(m_entries.size());
    hdr.optionsHash = m_optionsHash;

    constexpr size_t headerEnd = sizeof(fmt::BlobHeader);
    const size_t     tableSize = m_entries.size() * sizeof(fmt::EntryRecord);
    const size_t     dataStart = align8(headerEnd + tableSize);

    // Reflection dedup: two entries whose reflection bytes are byte-identical
    // share a single payload in the file. Collision-safe via full comparison
    // after hash match.
    std::vector<size_t>                  reflCanonical(m_entries.size());
    std::unordered_map<uint64_t, size_t> reflHashToCanonical;
    for (size_t i = 0; i < m_entries.size(); ++i)
    {
        reflCanonical[i] = i;
        if (m_entries[i].refl.empty())
            continue;
        uint64_t h = kFnvOffset;
        fnv1aMix(h, m_entries[i].refl.data(), m_entries[i].refl.size());
        auto it = reflHashToCanonical.find(h);
        if (it != reflHashToCanonical.end() && m_entries[it->second].refl == m_entries[i].refl)
            reflCanonical[i] = it->second;
        else
            reflHashToCanonical.emplace(h, i);
    }

    std::vector<fmt::EntryRecord> records(m_entries.size());
    size_t                        cursor = dataStart;
    for (size_t i = 0; i < m_entries.size(); ++i)
    {
        const auto& e = m_entries[i];

        records[i].keyOffset = static_cast<uint32_t>(cursor);
        records[i].keySize   = static_cast<uint32_t>(e.key.size() + 1);
        cursor += e.key.size() + 1;

        cursor                = align8(cursor);
        records[i].codeOffset = static_cast<uint32_t>(cursor);
        records[i].codeSize   = static_cast<uint32_t>(e.code.size());
        cursor += e.code.size();

        if (reflCanonical[i] == i)
        {
            cursor                = align8(cursor);
            records[i].reflOffset = static_cast<uint32_t>(cursor);
            records[i].reflSize   = static_cast<uint32_t>(e.refl.size());
            cursor += e.refl.size();
        }
        else
        {
            records[i].reflOffset = records[reflCanonical[i]].reflOffset;
            records[i].reflSize   = records[reflCanonical[i]].reflSize;
        }
    }

    cursor                                                       = align8(cursor);
    size_t                                     depsStringsOffset = cursor;
    std::vector<std::pair<uint32_t, uint32_t>> depPathSlots;
    depPathSlots.reserve(m_deps.size());
    for (const auto& d : m_deps)
    {
        depPathSlots.emplace_back(static_cast<uint32_t>(cursor), static_cast<uint32_t>(d.path.size()));
        cursor += d.path.size();
    }
    size_t depsStringsSize = cursor - depsStringsOffset;

    cursor                 = align8(cursor);
    size_t depsTableOffset = cursor;
    cursor += m_deps.size() * sizeof(fmt::DepEntry);

    // Per-entry dep index pool: flat u32 array, with each EntryRecord storing
    // (depsIdxOff, depsIdxCount) into it.
    cursor                         = align4(cursor);
    size_t   entryDepsIdxOffset    = cursor;
    uint32_t runningEntryDepsCount = 0;
    for (size_t i = 0; i < m_entries.size(); ++i)
    {
        records[i].depsIdxOff   = runningEntryDepsCount;
        records[i].depsIdxCount = static_cast<uint32_t>(m_entries[i].depIndices.size());
        runningEntryDepsCount += records[i].depsIdxCount;
    }
    cursor += runningEntryDepsCount * sizeof(uint32_t);

    hdr.depsOffset         = static_cast<uint32_t>(depsTableOffset);
    hdr.depsCount          = static_cast<uint32_t>(m_deps.size());
    hdr.depsStringsOffset  = static_cast<uint32_t>(depsStringsOffset);
    hdr.depsStringsSize    = static_cast<uint32_t>(depsStringsSize);
    hdr.entryDepsIdxOffset = static_cast<uint32_t>(entryDepsIdxOffset);
    hdr.entryDepsIdxCount  = runningEntryDepsCount;

    out.reserve(cursor);
    appendBytes(out, hdr);
    appendBytes(out, records.data(), tableSize);
    while (out.size() < dataStart)
        out.push_back(0);

    for (size_t i = 0; i < m_entries.size(); ++i)
    {
        const auto& e = m_entries[i];
        out.insert(out.end(), e.key.begin(), e.key.end());
        out.push_back('\0');
        padTo8(out);
        out.insert(out.end(), e.code.begin(), e.code.end());
        if (reflCanonical[i] == i)
        {
            padTo8(out);
            out.insert(out.end(), e.refl.begin(), e.refl.end());
        }
    }

    padTo8(out);
    for (const auto& d : m_deps)
        out.insert(out.end(), d.path.begin(), d.path.end());
    padTo8(out);
    for (size_t i = 0; i < m_deps.size(); ++i)
    {
        fmt::DepEntry de{};
        de.pathOffset  = depPathSlots[i].first;
        de.pathSize    = depPathSlots[i].second;
        de.contentHash = m_deps[i].contentHash;
        appendBytes(out, de);
    }
    padTo4(out);
    for (const auto& e : m_entries)
        for (uint32_t idx : e.depIndices)
            appendBytes(out, idx);
    while (out.size() < cursor)
        out.push_back(0);

    if (m_compression != Codec::None)
    {
        constexpr size_t         headerSize = sizeof(fmt::BlobHeader);
        std::span<const uint8_t> payload(out.data() + headerSize, out.size() - headerSize);
        auto                     compressed = compressPayload(m_compression, payload);

        std::vector<uint8_t> wrapped;
        wrapped.reserve(headerSize + compressed.size());
        wrapped.insert(wrapped.end(), out.begin(), out.begin() + headerSize);

        auto* wrappedHdr                    = reinterpret_cast<fmt::BlobHeader*>(wrapped.data());
        wrappedHdr->compression             = static_cast<uint32_t>(m_compression);
        wrappedHdr->uncompressedPayloadSize = static_cast<uint32_t>(payload.size());

        wrapped.insert(wrapped.end(), compressed.begin(), compressed.end());
        return wrapped;
    }

    auto* outHdr                    = reinterpret_cast<fmt::BlobHeader*>(out.data());
    outHdr->compression             = 0;
    outHdr->uncompressedPayloadSize = static_cast<uint32_t>(out.size() - sizeof(fmt::BlobHeader));
    return out;
}

void BlobWriter::writeToFile(const std::filesystem::path& path) const
{
    auto bytes  = finalize();
    auto parent = path.parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent);
    std::ofstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("failed to open for write: " + path.string());
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

BlobReader::BlobReader(std::span<const uint8_t> blob) { rebind(blob); }

BlobReader::BlobReader(std::vector<uint8_t>&& blob)
{
    m_owned = std::move(blob);
    rebind(std::span<const uint8_t>(m_owned));
}

std::optional<BlobReader> BlobReader::openFile(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return std::nullopt;
    auto size = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    if (size > 0)
        f.read(reinterpret_cast<char*>(buf.data()), size);

    BlobReader r{std::span<const uint8_t>{}};
    r.m_owned = std::move(buf);
    r.rebind(std::span<const uint8_t>(r.m_owned));
    if (!r.valid())
        return std::nullopt;
    return r;
}

void BlobReader::rebind(std::span<const uint8_t> blob)
{
    m_blob    = blob;
    m_hdr     = nullptr;
    m_entries = nullptr;
    if (m_blob.size() < sizeof(fmt::BlobHeader))
        return;
    const auto* hdr = reinterpret_cast<const fmt::BlobHeader*>(m_blob.data());
    if (hdr->magic != fmt::kBlobMagic)
        return;
    if (hdr->version != fmt::kBlobVersion)
        return;

    // Transparent decompression: expand compressed payload into an owned buffer
    // so the entry-table offsets (always expressed in uncompressed coordinates)
    // stay valid as zero-copy spans.
    if (hdr->compression != static_cast<uint32_t>(Codec::None))
    {
        constexpr size_t headerSize = sizeof(fmt::BlobHeader);
        if (m_blob.size() < headerSize)
        {
            m_blob = {};
            return;
        }
        auto                     codec = static_cast<Codec>(hdr->compression);
        std::span payload(m_blob.data() + headerSize, m_blob.size() - headerSize);
        std::vector<uint8_t>     decompressed;
        try
        {
            decompressed = detail::decompressPayload(codec, payload, hdr->uncompressedPayloadSize);
        }
        catch (...)
        {
            m_blob = {};
            return;
        }
        std::vector<uint8_t> expanded;
        expanded.reserve(headerSize + decompressed.size());
        expanded.insert(expanded.end(), m_blob.data(), m_blob.data() + headerSize);
        expanded.insert(expanded.end(), decompressed.begin(), decompressed.end());
        auto* newHdr        = reinterpret_cast<fmt::BlobHeader*>(expanded.data());
        newHdr->compression = static_cast<uint32_t>(Codec::None);
        m_owned             = std::move(expanded);
        m_blob              = std::span<const uint8_t>(m_owned);
        hdr                 = reinterpret_cast<const fmt::BlobHeader*>(m_blob.data());
    }

    if (m_blob.size() < sizeof(fmt::BlobHeader) + hdr->entryCount * sizeof(fmt::EntryRecord))
        return;
    m_hdr     = hdr;
    m_entries = reinterpret_cast<const fmt::EntryRecord*>(m_blob.data() + sizeof(fmt::BlobHeader));
}

Target BlobReader::target() const
{
    if (!m_hdr)
        return Target::SPIRV;
    auto t = static_cast<SlangCompileTarget>(m_hdr->target);
    switch (t)
    {
    case SLANG_SPIRV:
        return Target::SPIRV;
    case SLANG_DXIL:
        return Target::DXIL;
    case SLANG_DXBC:
        return Target::DXBC;
    case SLANG_HLSL:
        return Target::HLSL;
    case SLANG_GLSL:
        return Target::GLSL;
    case SLANG_METAL:
        return Target::Metal;
    case SLANG_METAL_LIB:
        return Target::MetalLib;
    case SLANG_WGSL:
        return Target::WGSL;
    default:
        return Target::SPIRV;
    }
}

size_t BlobReader::entryCount() const { return m_hdr ? m_hdr->entryCount : 0; }

BlobReader::Entry BlobReader::at(size_t index) const
{
    Entry e{};
    if (!m_hdr || index >= m_hdr->entryCount)
        return e;
    const auto& r    = m_entries[index];
    const auto* blob = m_blob.data();

    if (r.keyOffset + r.keySize <= m_blob.size() && r.keySize > 0)
        e.key = std::string_view(reinterpret_cast<const char*>(blob + r.keyOffset), r.keySize - 1);
    if (r.codeOffset + r.codeSize <= m_blob.size())
        e.code = std::span<const uint8_t>(blob + r.codeOffset, r.codeSize);
    if (r.reflOffset + r.reflSize <= m_blob.size())
        e.reflection = std::span<const uint8_t>(blob + r.reflOffset, r.reflSize);
    if (r.depsIdxCount > 0)
    {
        const size_t byteOff = m_hdr->entryDepsIdxOffset + r.depsIdxOff * sizeof(uint32_t);
        const size_t byteEnd = byteOff + r.depsIdxCount * sizeof(uint32_t);
        if (byteEnd <= m_blob.size())
            e.depIndices =
                std::span<const uint32_t>(reinterpret_cast<const uint32_t*>(blob + byteOff), r.depsIdxCount);
    }
    return e;
}

std::optional<BlobReader::Entry> BlobReader::find(std::span<const ShaderConstant> constants) const
{
    Permutation p;
    p.constants.assign(constants.begin(), constants.end());
    auto key = p.key();
    for (size_t i = 0; i < entryCount(); ++i)
    {
        auto e = at(i);
        if (e.key == key)
            return e;
    }
    return std::nullopt;
}

std::vector<std::string> BlobReader::enumerate() const
{
    std::vector<std::string> out;
    out.reserve(entryCount());
    for (size_t i = 0; i < entryCount(); ++i)
        out.emplace_back(at(i).key);
    return out;
}

std::vector<BlobReader::Dep> BlobReader::dependencies() const
{
    std::vector<Dep> out;
    if (!m_hdr)
        return out;
    if (m_hdr->depsOffset + m_hdr->depsCount * sizeof(fmt::DepEntry) > m_blob.size())
        return out;
    const auto* deps = reinterpret_cast<const fmt::DepEntry*>(m_blob.data() + m_hdr->depsOffset);
    out.reserve(m_hdr->depsCount);
    for (uint32_t i = 0; i < m_hdr->depsCount; ++i)
    {
        Dep d{};
        if (deps[i].pathOffset + deps[i].pathSize <= m_blob.size())
            d.path =
                std::string_view(reinterpret_cast<const char*>(m_blob.data() + deps[i].pathOffset), deps[i].pathSize);
        d.contentHash = deps[i].contentHash;
        out.push_back(d);
    }
    return out;
}

uint64_t BlobReader::optionsHash() const { return m_hdr ? m_hdr->optionsHash : 0; }

Codec BlobReader::compression() const
{
    // After rebind()'s transparent decompression, m_hdr->compression reads 0.
    return m_hdr ? static_cast<Codec>(m_hdr->compression) : Codec::None;
}

} // namespace slangmake

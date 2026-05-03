#include <cstring>

#include "slangmake_runtime_internal.h"

namespace slangmake
{

ReflectionView::ReflectionView(std::span<const uint8_t> bytes)
    : m_bytes(bytes)
{
    if (bytes.size() < sizeof(fmt::ReflHeader))
        return;
    const auto* hdr = reinterpret_cast<const fmt::ReflHeader*>(bytes.data());
    if (hdr->magic != fmt::kReflectionMagic)
        return;
    if (hdr->version != fmt::kReflectionVersion)
        return;
    if (hdr->size > bytes.size())
        return;
    m_hdr = hdr;
}

uint64_t ReflectionView::firstEntryPointHash() const
{
    if (!m_hdr)
        return 0;
    return (uint64_t(m_hdr->entryPointHashHigh) << 32) | m_hdr->entryPointHashLow;
}

uint32_t ReflectionView::globalConstantBufferBinding() const
{
    return m_hdr ? m_hdr->globalCbBinding : fmt::kInvalidIndex;
}
uint32_t ReflectionView::globalConstantBufferSize() const { return m_hdr ? m_hdr->globalCbSize : fmt::kInvalidIndex; }
uint32_t ReflectionView::bindlessSpaceIndex() const { return m_hdr ? m_hdr->bindlessSpaceIndex : fmt::kInvalidIndex; }
std::optional<uint32_t> ReflectionView::globalParamsVarLayout() const
{
    if (!m_hdr || m_hdr->globalParamsVarLayoutIdx == fmt::kInvalidIndex)
        return std::nullopt;
    return m_hdr->globalParamsVarLayoutIdx;
}

namespace
{
template <class T>
std::span<const T> tableSpan(const uint8_t* base, uint32_t off, uint32_t count)
{
    return std::span<const T>(reinterpret_cast<const T*>(base + off), count);
}
} // namespace

std::span<const fmt::Type> ReflectionView::types() const
{
    if (!m_hdr)
        return {};
    return tableSpan<fmt::Type>(m_bytes.data(), m_hdr->type_off, m_hdr->type_count);
}
std::span<const fmt::TypeLayout> ReflectionView::typeLayouts() const
{
    if (!m_hdr)
        return {};
    return tableSpan<fmt::TypeLayout>(m_bytes.data(), m_hdr->typeLayout_off, m_hdr->typeLayout_count);
}
std::span<const fmt::Variable> ReflectionView::variables() const
{
    if (!m_hdr)
        return {};
    return tableSpan<fmt::Variable>(m_bytes.data(), m_hdr->var_off, m_hdr->var_count);
}
std::span<const fmt::VarLayout> ReflectionView::varLayouts() const
{
    if (!m_hdr)
        return {};
    return tableSpan<fmt::VarLayout>(m_bytes.data(), m_hdr->varLayout_off, m_hdr->varLayout_count);
}
std::span<const fmt::Function> ReflectionView::functions() const
{
    if (!m_hdr)
        return {};
    return tableSpan<fmt::Function>(m_bytes.data(), m_hdr->func_off, m_hdr->func_count);
}
std::span<const fmt::Generic> ReflectionView::generics() const
{
    if (!m_hdr)
        return {};
    return tableSpan<fmt::Generic>(m_bytes.data(), m_hdr->generic_off, m_hdr->generic_count);
}
std::span<const fmt::Decl> ReflectionView::decls() const
{
    if (!m_hdr)
        return {};
    return tableSpan<fmt::Decl>(m_bytes.data(), m_hdr->decl_off, m_hdr->decl_count);
}
std::span<const fmt::EntryPoint> ReflectionView::entryPoints() const
{
    if (!m_hdr)
        return {};
    return tableSpan<fmt::EntryPoint>(m_bytes.data(), m_hdr->entryPoint_off, m_hdr->entryPoint_count);
}
std::span<const fmt::Attribute> ReflectionView::attributes() const
{
    if (!m_hdr)
        return {};
    return tableSpan<fmt::Attribute>(m_bytes.data(), m_hdr->attr_off, m_hdr->attr_count);
}
std::span<const fmt::AttrArg> ReflectionView::attributeArgs() const
{
    if (!m_hdr)
        return {};
    return tableSpan<fmt::AttrArg>(m_bytes.data(), m_hdr->attrArg_off, m_hdr->attrArg_count);
}
std::span<const uint32_t> ReflectionView::modifierPool() const
{
    if (!m_hdr)
        return {};
    return tableSpan<uint32_t>(m_bytes.data(), m_hdr->modifier_off, m_hdr->modifier_count);
}
std::span<const fmt::HashedStr> ReflectionView::hashedStrings() const
{
    if (!m_hdr)
        return {};
    return tableSpan<fmt::HashedStr>(m_bytes.data(), m_hdr->hashedStr_off, m_hdr->hashedStr_count);
}
std::span<const fmt::BindingRange> ReflectionView::bindingRanges() const
{
    if (!m_hdr)
        return {};
    return tableSpan<fmt::BindingRange>(m_bytes.data(), m_hdr->bindingRange_off, m_hdr->bindingRange_count);
}
std::span<const fmt::DescriptorSet> ReflectionView::descriptorSets() const
{
    if (!m_hdr)
        return {};
    return tableSpan<fmt::DescriptorSet>(m_bytes.data(), m_hdr->descriptorSet_off, m_hdr->descriptorSet_count);
}
std::span<const fmt::DescriptorRange> ReflectionView::descriptorRanges() const
{
    if (!m_hdr)
        return {};
    return tableSpan<fmt::DescriptorRange>(m_bytes.data(), m_hdr->descriptorRange_off, m_hdr->descriptorRange_count);
}
std::span<const fmt::SubObjectRange> ReflectionView::subObjectRanges() const
{
    if (!m_hdr)
        return {};
    return tableSpan<fmt::SubObjectRange>(m_bytes.data(), m_hdr->subObjectRange_off, m_hdr->subObjectRange_count);
}
std::span<const uint32_t> ReflectionView::u32Pool() const
{
    if (!m_hdr)
        return {};
    return tableSpan<uint32_t>(m_bytes.data(), m_hdr->u32Pool_off, m_hdr->u32Pool_count);
}

std::string_view ReflectionView::string(uint32_t strIdx) const
{
    if (!m_hdr || strIdx == fmt::kInvalidIndex)
        return {};
    if (strIdx >= m_hdr->strings_size)
        return {};
    const char* base = reinterpret_cast<const char*>(m_bytes.data() + m_hdr->strings_off);
    return {base + strIdx};
}

std::string_view ReflectionView::hashedString(uint32_t index) const
{
    auto hs = hashedStrings();
    if (index >= hs.size())
        return {};
    return string(hs[index].strIdx);
}

ReflectionView::Param ReflectionView::decodeParam(const fmt::VarLayout& vl) const
{
    Param p{};
    auto  vars = variables();
    if (vl.varIdx < vars.size())
        p.name = string(vars[vl.varIdx].nameStrIdx);
    p.category      = vl.category;
    p.binding       = vl.bindingIndex;
    p.imageFormat   = vl.imageFormat;
    p.semanticName  = string(vl.semanticNameStrIdx);
    p.semanticIndex = vl.semanticIndex;
    p.stage         = vl.stage;
    auto pool       = u32Pool();
    p.space         = (vl.categoryCount > 0 && vl.bindingSpacePoolOff < pool.size()) ? pool[vl.bindingSpacePoolOff] : 0;
    p.byteOffset    = (vl.categoryCount > 0 && vl.offsetPoolOff < pool.size()) ? pool[vl.offsetPoolOff] : 0;

    p.byteSize     = 0;
    p.elementCount = 1;
    auto tls       = typeLayouts();
    if (vl.typeLayoutIdx < tls.size())
    {
        const auto& tl = tls[vl.typeLayoutIdx];
        if (tl.categoryCount > 0 && tl.sizePoolOff < pool.size())
            p.byteSize = pool[tl.sizePoolOff];
        if (tl.typeIdx < types().size())
        {
            const auto& ty = types()[tl.typeIdx];
            if (ty.elementCount != fmt::kInvalidIndex)
                p.elementCount = ty.elementCount;
        }
    }
    return p;
}

std::vector<ReflectionView::EntryPointInfo> ReflectionView::decodedEntryPoints() const
{
    std::vector<EntryPointInfo> out;
    if (!m_hdr)
        return out;
    auto eps   = entryPoints();
    auto vls   = varLayouts();
    auto attrs = attributes();
    auto args  = attributeArgs();
    auto pool  = u32Pool();
    out.reserve(eps.size());
    for (const auto& ep : eps)
    {
        EntryPointInfo info{};
        info.name            = string(ep.nameStrIdx);
        info.nameOverride    = string(ep.nameOverrideStrIdx);
        info.stage           = ep.stage;
        info.threadGroupSize = {ep.threadGroupSizeX, ep.threadGroupSizeY, ep.threadGroupSizeZ};
        info.waveSize        = ep.waveSize;
        info.hash            = (uint64_t(ep.hashHigh) << 32) | ep.hashLow;

        for (uint32_t k = 0; k < ep.paramVarLayoutCount; ++k)
        {
            uint32_t vlIdx = pool[ep.paramVarLayoutOff + k];
            if (vlIdx < vls.size())
                info.parameters.push_back(decodeParam(vls[vlIdx]));
        }
        for (uint32_t k = 0; k < ep.attrCount; ++k)
        {
            const auto&               a = attrs[ep.attrOff + k];
            std::vector<fmt::AttrArg> argList;
            for (uint32_t j = 0; j < a.argCount; ++j)
                argList.push_back(args[a.argOff + j]);
            info.attributes.emplace_back(string(a.nameStrIdx), std::move(argList));
        }
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<ReflectionView::Param> ReflectionView::decodedGlobalParameters() const
{
    std::vector<Param> out;
    if (!m_hdr)
        return out;
    auto vls = varLayouts();
    if (m_hdr->globalParamsVarLayoutIdx == fmt::kInvalidIndex)
        return out;
    if (m_hdr->globalParamsVarLayoutIdx >= vls.size())
        return out;
    const auto& gvl  = vls[m_hdr->globalParamsVarLayoutIdx];
    auto        tls  = typeLayouts();
    auto        pool = u32Pool();
    if (gvl.typeLayoutIdx >= tls.size())
        return out;
    const auto& tl = tls[gvl.typeLayoutIdx];
    for (uint32_t k = 0; k < tl.fieldCount; ++k)
    {
        uint32_t vlIdx = pool[tl.fieldLayoutPoolOff + k];
        if (vlIdx < vls.size())
            out.push_back(decodeParam(vls[vlIdx]));
    }
    return out;
}

std::optional<ReflectionView::DeclNode> ReflectionView::rootDecl() const
{
    auto ds = decls();
    if (ds.empty())
        return std::nullopt;
    return decl(0);
}

ReflectionView::DeclNode ReflectionView::decl(uint32_t idx) const
{
    DeclNode n{};
    auto     ds = decls();
    if (idx >= ds.size())
        return n;
    const auto& d   = ds[idx];
    n.kind          = d.kind;
    n.name          = string(d.nameStrIdx);
    n.parentDeclIdx = d.parentDeclIdx;
    n.payloadIdx    = d.payloadIdx;
    auto pool       = u32Pool();
    if (d.childOff + d.childCount <= pool.size())
        n.children = pool.subspan(d.childOff, d.childCount);
    return n;
}

} // namespace slangmake

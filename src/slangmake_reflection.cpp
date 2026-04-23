#include <cstring>
#include <functional>
#include <unordered_map>
#include <utility>

#include "slangmake_internal.h"

namespace slangmake
{

namespace
{

using detail::align8;
using detail::appendBytes;

class ReflectionSerializer
{
public:
    explicit ReflectionSerializer(slang::IGlobalSession* gs)
        : m_globalSession(gs)
    {
    }

    std::vector<uint8_t> serialize(slang::IComponentType* program, slang::IModule* module, int targetIndex);

private:
    slang::IGlobalSession* m_globalSession;

    std::vector<char>                         m_strings;
    std::unordered_map<std::string, uint32_t> m_stringIdx;
    uint32_t                                  internStr(const char* s);
    uint32_t                                  internStr(std::string_view s);

    std::vector<fmt::Type>              m_types;
    std::unordered_map<void*, uint32_t> m_typeIdx;
    std::vector<fmt::TypeLayout>        m_typeLayouts;
    std::unordered_map<void*, uint32_t> m_typeLayoutIdx;
    std::vector<fmt::Variable>          m_vars;
    std::unordered_map<void*, uint32_t> m_varIdx;
    std::vector<fmt::VarLayout>         m_varLayouts;
    std::unordered_map<void*, uint32_t> m_varLayoutIdx;
    std::vector<fmt::Function>          m_funcs;
    std::unordered_map<void*, uint32_t> m_funcIdx;
    std::vector<fmt::Generic>           m_generics;
    std::unordered_map<void*, uint32_t> m_genericIdx;
    std::vector<fmt::Decl>              m_decls;
    std::unordered_map<void*, uint32_t> m_declIdx;
    std::vector<fmt::EntryPoint>        m_entryPoints;
    std::vector<fmt::Attribute>         m_attrs;
    std::vector<fmt::AttrArg>           m_attrArgs;
    std::vector<uint32_t>               m_modifierPool;
    std::vector<fmt::HashedStr>         m_hashedStrs;
    std::vector<fmt::BindingRange>      m_bindingRanges;
    std::vector<fmt::DescriptorSet>     m_descriptorSets;
    std::vector<fmt::DescriptorRange>   m_descriptorRanges;
    std::vector<fmt::SubObjectRange>    m_subObjectRanges;
    std::vector<uint32_t>               m_u32Pool;

    uint32_t addType(slang::TypeReflection* t);
    uint32_t addTypeLayout(slang::TypeLayoutReflection* tl);
    uint32_t addVariable(slang::VariableReflection* v);
    uint32_t addVarLayout(slang::VariableLayoutReflection* vl);
    uint32_t addFunction(slang::FunctionReflection* f);
    uint32_t addGeneric(slang::GenericReflection* g);
    uint32_t addDecl(slang::DeclReflection* d, uint32_t parent);
    uint32_t addEntryPoint(slang::EntryPointReflection* ep, slang::IComponentType* program, int entryPointIndex,
                           int targetIndex);

    std::pair<uint32_t, uint32_t> addAttributesFromVar(slang::VariableReflection* v);
    std::pair<uint32_t, uint32_t> addAttributesFromFunc(slang::FunctionReflection* f);
    std::pair<uint32_t, uint32_t> addModifiersFromVar(slang::VariableReflection* v);
    std::pair<uint32_t, uint32_t> addModifiersFromFunc(slang::FunctionReflection* f);

    uint32_t pushU32List(const std::vector<uint32_t>& list);
};

uint32_t ReflectionSerializer::internStr(const char* s)
{
    if (!s)
        return fmt::kInvalidIndex;
    return internStr(std::string_view(s));
}

uint32_t ReflectionSerializer::internStr(std::string_view s)
{
    if (s.data() == nullptr)
        return fmt::kInvalidIndex;
    std::string key(s);
    auto        it = m_stringIdx.find(key);
    if (it != m_stringIdx.end())
        return it->second;
    auto off = static_cast<uint32_t>(m_strings.size());
    m_strings.insert(m_strings.end(), s.begin(), s.end());
    m_strings.push_back('\0');
    m_stringIdx.emplace(std::move(key), off);
    return off;
}

uint32_t ReflectionSerializer::pushU32List(const std::vector<uint32_t>& list)
{
    if (list.empty())
        return 0;
    auto off = static_cast<uint32_t>(m_u32Pool.size());
    m_u32Pool.insert(m_u32Pool.end(), list.begin(), list.end());
    return off;
}

constexpr slang::Modifier::ID kAllModifiers[] = {
    slang::Modifier::Shared, slang::Modifier::NoDiff, slang::Modifier::Static,         slang::Modifier::Const,
    slang::Modifier::Export, slang::Modifier::Extern, slang::Modifier::Differentiable, slang::Modifier::Mutating,
    slang::Modifier::In,     slang::Modifier::Out,    slang::Modifier::InOut,
};

std::pair<uint32_t, uint32_t> ReflectionSerializer::addModifiersFromVar(slang::VariableReflection* v)
{
    if (!v)
        return {0, 0};
    std::vector<uint32_t> ids;
    for (auto id : kAllModifiers)
        if (v->findModifier(id))
            ids.push_back(static_cast<uint32_t>(id));
    if (ids.empty())
        return {0, 0};
    auto off = static_cast<uint32_t>(m_modifierPool.size());
    m_modifierPool.insert(m_modifierPool.end(), ids.begin(), ids.end());
    return {off, static_cast<uint32_t>(ids.size())};
}

std::pair<uint32_t, uint32_t> ReflectionSerializer::addModifiersFromFunc(slang::FunctionReflection* f)
{
    if (!f)
        return {0, 0};
    std::vector<uint32_t> ids;
    for (auto id : kAllModifiers)
        if (f->findModifier(id))
            ids.push_back(id);
    if (ids.empty())
        return {0, 0};
    auto off = static_cast<uint32_t>(m_modifierPool.size());
    m_modifierPool.insert(m_modifierPool.end(), ids.begin(), ids.end());
    return {off, static_cast<uint32_t>(ids.size())};
}

template <class GetCount, class GetByIndex>
std::pair<uint32_t, uint32_t>
serializeAttributes(std::vector<fmt::Attribute>& attrs, std::vector<fmt::AttrArg>& attrArgs, GetCount getCount,
                    GetByIndex getByIndex, const std::function<uint32_t(std::string_view)>& intern)
{
    uint32_t count = getCount();
    if (count == 0)
        return {0, 0};
    auto off = static_cast<uint32_t>(attrs.size());
    for (uint32_t i = 0; i < count; ++i)
    {
        slang::Attribute* a = getByIndex(i);
        if (!a)
        {
            attrs.push_back(fmt::Attribute{fmt::kInvalidIndex, 0, 0});
            continue;
        }
        uint32_t nameIdx  = intern(a->getName() ? a->getName() : "");
        uint32_t argCount = a->getArgumentCount();
        auto     argOff   = static_cast<uint32_t>(attrArgs.size());
        for (uint32_t k = 0; k < argCount; ++k)
        {
            fmt::AttrArg arg{};
            arg.kind   = fmt::AttrArg::None;
            arg.strIdx = fmt::kInvalidIndex;
            int   iv   = 0;
            float fv   = 0.0f;
            if (SLANG_SUCCEEDED(a->getArgumentValueInt(k, &iv)))
            {
                arg.kind    = fmt::AttrArg::Int;
                int64_t v64 = iv;
                std::memcpy(&arg.raw, &v64, sizeof(uint64_t));
            }
            else if (SLANG_SUCCEEDED(a->getArgumentValueFloat(k, &fv)))
            {
                arg.kind  = fmt::AttrArg::Float;
                double dv = fv;
                std::memcpy(&arg.raw, &dv, sizeof(uint64_t));
            }
            else
            {
                size_t      sz = 0;
                const char* cs = a->getArgumentValueString(k, &sz);
                if (cs)
                {
                    arg.kind   = fmt::AttrArg::String;
                    arg.strIdx = intern(std::string_view(cs, sz));
                }
            }
            attrArgs.push_back(arg);
        }
        attrs.push_back(fmt::Attribute{nameIdx, argOff, argCount});
    }
    return {off, count};
}

std::pair<uint32_t, uint32_t> ReflectionSerializer::addAttributesFromVar(slang::VariableReflection* v)
{
    if (!v)
        return {0, 0};
    return serializeAttributes(
        m_attrs, m_attrArgs, [&] { return v->getUserAttributeCount(); }, [&](uint32_t i) -> slang::Attribute*
        { return v->getUserAttributeByIndex(i); }, [&](std::string_view s) { return internStr(s); });
}

std::pair<uint32_t, uint32_t> ReflectionSerializer::addAttributesFromFunc(slang::FunctionReflection* f)
{
    if (!f)
        return {0, 0};
    return serializeAttributes(
        m_attrs, m_attrArgs, [&] { return f->getUserAttributeCount(); }, [&](uint32_t i) -> slang::Attribute*
        { return f->getUserAttributeByIndex(i); }, [&](std::string_view s) { return internStr(s); });
}

uint32_t ReflectionSerializer::addType(slang::TypeReflection* t)
{
    if (!t)
        return fmt::kInvalidIndex;
    void* key = t;
    auto  it  = m_typeIdx.find(key);
    if (it != m_typeIdx.end())
        return it->second;

    auto idx       = static_cast<uint32_t>(m_types.size());
    m_typeIdx[key] = idx;
    m_types.push_back({});

    fmt::Type rec{};
    rec.kind       = static_cast<uint32_t>(t->getKind());
    rec.nameStrIdx = internStr(t->getName());
    {
        Slang::ComPtr<ISlangBlob> blob;
        if (SLANG_SUCCEEDED(t->getFullName(blob.writeRef())) && blob)
        {
            std::string_view sv(reinterpret_cast<const char*>(blob->getBufferPointer()), blob->getBufferSize());
            rec.fullNameStrIdx = internStr(sv);
        }
        else
        {
            rec.fullNameStrIdx = fmt::kInvalidIndex;
        }
    }
    rec.scalarKind = static_cast<uint32_t>(t->getScalarType());

    rec.fieldCount   = 0;
    rec.fieldPoolOff = 0;
    if (t->getKind() == slang::TypeReflection::Kind::Struct)
    {
        unsigned              fc = t->getFieldCount();
        std::vector<uint32_t> fieldVarIdx;
        fieldVarIdx.reserve(fc);
        for (unsigned i = 0; i < fc; ++i)
            fieldVarIdx.push_back(addVariable(t->getFieldByIndex(i)));
        rec.fieldCount   = fc;
        rec.fieldPoolOff = pushU32List(fieldVarIdx);
    }

    rec.rowCount       = t->getRowCount();
    rec.colCount       = t->getColumnCount();
    rec.elementTypeIdx = addType(t->getElementType());
    {
        size_t ec = t->getElementCount();
        rec.elementCount =
            (ec == SLANG_UNKNOWN_SIZE || ec == SLANG_UNBOUNDED_SIZE) ? fmt::kInvalidIndex : static_cast<uint32_t>(ec);
        size_t tec                 = t->getTotalArrayElementCount();
        rec.totalArrayElementCount = (tec == SLANG_UNKNOWN_SIZE || tec == SLANG_UNBOUNDED_SIZE)
                                         ? fmt::kInvalidIndex
                                         : static_cast<uint32_t>(tec);
    }
    rec.resourceShape         = static_cast<uint32_t>(t->getResourceShape());
    rec.resourceAccess        = static_cast<uint32_t>(t->getResourceAccess());
    rec.resourceResultTypeIdx = addType(t->getResourceResultType());

    m_types[idx] = rec;
    return idx;
}

uint32_t ReflectionSerializer::addTypeLayout(slang::TypeLayoutReflection* tl)
{
    if (!tl)
        return fmt::kInvalidIndex;
    void* key = tl;
    auto  it  = m_typeLayoutIdx.find(key);
    if (it != m_typeLayoutIdx.end())
        return it->second;

    auto idx             = static_cast<uint32_t>(m_typeLayouts.size());
    m_typeLayoutIdx[key] = idx;
    m_typeLayouts.push_back({});

    fmt::TypeLayout rec{};
    rec.typeIdx           = addType(tl->getType());
    rec.kind              = static_cast<uint32_t>(tl->getKind());
    rec.parameterCategory = static_cast<uint32_t>(tl->getParameterCategory());

    unsigned cc       = tl->getCategoryCount();
    rec.categoryCount = cc;
    {
        std::vector<uint32_t> cats(cc);
        std::vector<uint32_t> sizes;
        sizes.reserve(cc * 3);
        for (unsigned i = 0; i < cc; ++i)
        {
            auto cat   = tl->getCategoryByIndex(i);
            cats[i]    = static_cast<uint32_t>(cat);
            size_t  sz = tl->getSize(static_cast<SlangParameterCategory>(cat));
            size_t  st = tl->getStride(static_cast<SlangParameterCategory>(cat));
            int32_t al = tl->getAlignment(static_cast<SlangParameterCategory>(cat));
            sizes.push_back(static_cast<uint32_t>(sz));
            sizes.push_back(static_cast<uint32_t>(st));
            sizes.push_back(static_cast<uint32_t>(al));
        }
        rec.categoryPoolOff = pushU32List(cats);
        rec.sizePoolOff     = pushU32List(sizes);
    }

    rec.fieldCount         = 0;
    rec.fieldLayoutPoolOff = 0;
    if (tl->getKind() == slang::TypeReflection::Kind::Struct)
    {
        unsigned              fc = tl->getFieldCount();
        std::vector<uint32_t> fieldVarLayouts;
        fieldVarLayouts.reserve(fc);
        for (unsigned i = 0; i < fc; ++i)
            fieldVarLayouts.push_back(addVarLayout(tl->getFieldByIndex(i)));
        rec.fieldCount         = fc;
        rec.fieldLayoutPoolOff = pushU32List(fieldVarLayouts);
    }

    rec.containerVarLayoutIdx       = addVarLayout(tl->getContainerVarLayout());
    rec.elementTypeLayoutIdx        = addTypeLayout(tl->getElementTypeLayout());
    rec.elementVarLayoutIdx         = addVarLayout(tl->getElementVarLayout());
    rec.explicitCounterVarLayoutIdx = addVarLayout(tl->getExplicitCounter());
    rec.matrixLayoutMode            = static_cast<uint32_t>(tl->getMatrixLayoutMode());
    rec.genericParamIndex =
        (tl->getGenericParamIndex() < 0) ? fmt::kInvalidIndex : static_cast<uint32_t>(tl->getGenericParamIndex());

    // Slang only populates binding-range metadata for container layouts; calling
    // the binding-range accessors on a leaf resource dereferences uninitialised
    // state inside slang.
    using TK               = slang::TypeReflection::Kind;
    auto kind              = tl->getKind();
    bool walkBindingTables = (kind == TK::Struct) || (kind == TK::ConstantBuffer) || (kind == TK::ParameterBlock) ||
                             (kind == TK::TextureBuffer) || (kind == TK::ShaderStorageBuffer);

    rec.bindingRangeCount   = 0;
    rec.bindingRangeOff     = static_cast<uint32_t>(m_bindingRanges.size());
    rec.descriptorSetCount  = 0;
    rec.descriptorSetOff    = static_cast<uint32_t>(m_descriptorSets.size());
    rec.subObjectRangeCount = 0;
    rec.subObjectRangeOff   = static_cast<uint32_t>(m_subObjectRanges.size());

    if (walkBindingTables)
    {
        SlangInt brCount      = tl->getBindingRangeCount();
        rec.bindingRangeCount = static_cast<uint32_t>(brCount);
        for (SlangInt b = 0; b < brCount; ++b)
        {
            fmt::BindingRange br{};
            br.bindingType       = static_cast<uint32_t>(tl->getBindingRangeType(b));
            br.bindingCount      = static_cast<uint32_t>(tl->getBindingRangeBindingCount(b));
            br.leafTypeLayoutIdx = fmt::kInvalidIndex;
            if (auto* leaf = tl->getBindingRangeLeafTypeLayout(b))
            {
                auto it2             = m_typeLayoutIdx.find(leaf);
                br.leafTypeLayoutIdx = (it2 != m_typeLayoutIdx.end()) ? it2->second : fmt::kInvalidIndex;
            }
            br.leafVariableIdx = fmt::kInvalidIndex;
            if (auto* leafV = tl->getBindingRangeLeafVariable(b))
            {
                auto it2           = m_varIdx.find(leafV);
                br.leafVariableIdx = (it2 != m_varIdx.end()) ? it2->second : addVariable(leafV);
            }
            // getBindingRangeImageFormat reads through a null pointer inside
            // slang for non-image bindings; guard it.
            br.imageFormat = 0;
            {
                auto bt = static_cast<SlangBindingType>(br.bindingType);
                if (bt == SLANG_BINDING_TYPE_TEXTURE || bt == SLANG_BINDING_TYPE_MUTABLE_TETURE ||
                    bt == SLANG_BINDING_TYPE_TYPED_BUFFER || bt == SLANG_BINDING_TYPE_MUTABLE_TYPED_BUFFER)
                {
                    br.imageFormat = static_cast<uint32_t>(tl->getBindingRangeImageFormat(b));
                }
            }
            br.isSpecializable           = tl->isBindingRangeSpecializable(b) ? 1u : 0u;
            SlangInt dsi                 = tl->getBindingRangeDescriptorSetIndex(b);
            br.descriptorSetIndex        = (dsi < 0) ? fmt::kInvalidIndex : static_cast<uint32_t>(dsi);
            br.firstDescriptorRangeIndex = static_cast<uint32_t>(tl->getBindingRangeFirstDescriptorRangeIndex(b));
            br.descriptorRangeCount      = static_cast<uint32_t>(tl->getBindingRangeDescriptorRangeCount(b));
            m_bindingRanges.push_back(br);
        }

        SlangInt dsCount       = tl->getDescriptorSetCount();
        rec.descriptorSetCount = static_cast<uint32_t>(dsCount);
        for (SlangInt s = 0; s < dsCount; ++s)
        {
            fmt::DescriptorSet ds{};
            ds.spaceOffset          = static_cast<uint32_t>(tl->getDescriptorSetSpaceOffset(s));
            ds.descriptorRangeStart = static_cast<uint32_t>(m_descriptorRanges.size());
            SlangInt drCount        = tl->getDescriptorSetDescriptorRangeCount(s);
            ds.descriptorRangeCount = static_cast<uint32_t>(drCount);
            m_descriptorSets.push_back(ds);
            for (SlangInt r = 0; r < drCount; ++r)
            {
                fmt::DescriptorRange dr{};
                dr.indexOffset       = static_cast<uint32_t>(tl->getDescriptorSetDescriptorRangeIndexOffset(s, r));
                dr.descriptorCount   = static_cast<uint32_t>(tl->getDescriptorSetDescriptorRangeDescriptorCount(s, r));
                dr.bindingType       = static_cast<uint32_t>(tl->getDescriptorSetDescriptorRangeType(s, r));
                dr.parameterCategory = static_cast<uint32_t>(tl->getDescriptorSetDescriptorRangeCategory(s, r));
                m_descriptorRanges.push_back(dr);
            }
        }

        SlangInt soCount        = tl->getSubObjectRangeCount();
        rec.subObjectRangeCount = static_cast<uint32_t>(soCount);
        for (SlangInt s = 0; s < soCount; ++s)
        {
            fmt::SubObjectRange sr{};
            sr.bindingRangeIndex  = static_cast<uint32_t>(tl->getSubObjectRangeBindingRangeIndex(s));
            SlangInt sp           = tl->getSubObjectRangeSpaceOffset(s);
            sr.spaceOffset        = (sp < 0) ? fmt::kInvalidIndex : static_cast<uint32_t>(sp);
            sr.offsetVarLayoutIdx = addVarLayout(tl->getSubObjectRangeOffset(s));
            m_subObjectRanges.push_back(sr);
        }
    }

    m_typeLayouts[idx] = rec;
    return idx;
}

uint32_t ReflectionSerializer::addVariable(slang::VariableReflection* v)
{
    if (!v)
        return fmt::kInvalidIndex;
    void* key = v;
    auto  it  = m_varIdx.find(key);
    if (it != m_varIdx.end())
        return it->second;

    auto idx      = static_cast<uint32_t>(m_vars.size());
    m_varIdx[key] = idx;
    m_vars.push_back({});

    fmt::Variable rec{};
    rec.nameStrIdx      = internStr(v->getName());
    rec.typeIdx         = addType(v->getType());
    auto mods           = addModifiersFromVar(v);
    rec.modifierOff     = mods.first;
    rec.modifierCount   = mods.second;
    auto attrs          = addAttributesFromVar(v);
    rec.attrOff         = attrs.first;
    rec.attrCount       = attrs.second;
    rec.hasDefault      = v->hasDefaultValue() ? 1u : 0u;
    rec.defaultKind     = 0;
    rec.defaultValueRaw = 0;
    if (rec.hasDefault)
    {
        int64_t iv = 0;
        if (SLANG_SUCCEEDED(v->getDefaultValueInt(&iv)))
        {
            rec.defaultKind = 1;
            std::memcpy(&rec.defaultValueRaw, &iv, sizeof(uint64_t));
        }
        else
        {
            float fv = 0.0f;
            if (SLANG_SUCCEEDED(v->getDefaultValueFloat(&fv)))
            {
                rec.defaultKind = 2;
                double dv       = fv;
                std::memcpy(&rec.defaultValueRaw, &dv, sizeof(uint64_t));
            }
        }
    }
    rec.genericContainerIdx = addGeneric(v->getGenericContainer());
    m_vars[idx]             = rec;
    return idx;
}

uint32_t ReflectionSerializer::addVarLayout(slang::VariableLayoutReflection* vl)
{
    if (!vl)
        return fmt::kInvalidIndex;
    void* key = vl;
    auto  it  = m_varLayoutIdx.find(key);
    if (it != m_varLayoutIdx.end())
        return it->second;

    auto idx            = static_cast<uint32_t>(m_varLayouts.size());
    m_varLayoutIdx[key] = idx;
    m_varLayouts.push_back({});

    fmt::VarLayout rec{};
    rec.varIdx        = addVariable(vl->getVariable());
    rec.typeLayoutIdx = addTypeLayout(vl->getTypeLayout());
    rec.category      = static_cast<uint32_t>(vl->getCategory());

    unsigned cc       = vl->getCategoryCount();
    rec.categoryCount = cc;
    std::vector<uint32_t> cats(cc), offs(cc), spaces(cc);
    for (unsigned i = 0; i < cc; ++i)
    {
        auto cat  = vl->getCategoryByIndex(i);
        cats[i]   = static_cast<uint32_t>(cat);
        offs[i]   = static_cast<uint32_t>(vl->getOffset(static_cast<SlangParameterCategory>(cat)));
        spaces[i] = static_cast<uint32_t>(vl->getBindingSpace(static_cast<SlangParameterCategory>(cat)));
    }
    rec.categoryPoolOff     = pushU32List(cats);
    rec.offsetPoolOff       = pushU32List(offs);
    rec.bindingSpacePoolOff = pushU32List(spaces);
    rec.bindingIndex        = vl->getBindingIndex();
    rec.imageFormat         = static_cast<uint32_t>(vl->getImageFormat());
    rec.semanticNameStrIdx  = internStr(vl->getSemanticName());
    rec.semanticIndex       = static_cast<uint32_t>(vl->getSemanticIndex());
    rec.stage               = static_cast<uint32_t>(vl->getStage());

    auto mods         = addModifiersFromVar(vl->getVariable());
    rec.modifierOff   = mods.first;
    rec.modifierCount = mods.second;
    auto attrs        = addAttributesFromVar(vl->getVariable());
    rec.attrOff       = attrs.first;
    rec.attrCount     = attrs.second;

    m_varLayouts[idx] = rec;
    return idx;
}

uint32_t ReflectionSerializer::addFunction(slang::FunctionReflection* f)
{
    if (!f)
        return fmt::kInvalidIndex;
    void* key = f;
    auto  it  = m_funcIdx.find(key);
    if (it != m_funcIdx.end())
        return it->second;

    auto idx       = static_cast<uint32_t>(m_funcs.size());
    m_funcIdx[key] = idx;
    m_funcs.push_back({});

    fmt::Function rec{};
    rec.nameStrIdx           = internStr(f->getName());
    rec.returnTypeIdx        = addType(f->getReturnType());
    unsigned              pc = f->getParameterCount();
    std::vector<uint32_t> params(pc);
    for (unsigned i = 0; i < pc; ++i)
        params[i] = addVariable(f->getParameterByIndex(i));
    rec.paramVarOff         = pushU32List(params);
    rec.paramVarCount       = pc;
    auto mods               = addModifiersFromFunc(f);
    rec.modifierOff         = mods.first;
    rec.modifierCount       = mods.second;
    auto attrs              = addAttributesFromFunc(f);
    rec.attrOff             = attrs.first;
    rec.attrCount           = attrs.second;
    rec.genericContainerIdx = addGeneric(f->getGenericContainer());
    m_funcs[idx]            = rec;
    return idx;
}

uint32_t ReflectionSerializer::addGeneric(slang::GenericReflection* g)
{
    if (!g)
        return fmt::kInvalidIndex;
    void* key = g;
    auto  it  = m_genericIdx.find(key);
    if (it != m_genericIdx.end())
        return it->second;

    auto idx          = static_cast<uint32_t>(m_generics.size());
    m_genericIdx[key] = idx;
    m_generics.push_back({});

    fmt::Generic rec{};
    rec.nameStrIdx   = internStr(g->getName());
    rec.innerKind    = static_cast<uint32_t>(g->getInnerKind());
    rec.innerDeclIdx = fmt::kInvalidIndex;

    unsigned              tpc = g->getTypeParameterCount();
    std::vector<uint32_t> typeParams(tpc);
    for (unsigned i = 0; i < tpc; ++i)
        typeParams[i] = addVariable(g->getTypeParameter(i));
    rec.typeParamOff   = pushU32List(typeParams);
    rec.typeParamCount = tpc;

    unsigned              vpc = g->getValueParameterCount();
    std::vector<uint32_t> valParams(vpc);
    for (unsigned i = 0; i < vpc; ++i)
        valParams[i] = addVariable(g->getValueParameter(i));
    rec.valueParamOff   = pushU32List(valParams);
    rec.valueParamCount = vpc;

    std::vector<uint32_t> constraints;
    for (unsigned i = 0; i < tpc; ++i)
    {
        auto*    tp   = g->getTypeParameter(i);
        unsigned ccnt = g->getTypeParameterConstraintCount(tp);
        for (unsigned k = 0; k < ccnt; ++k)
        {
            constraints.push_back(typeParams[i]);
            constraints.push_back(addType(g->getTypeParameterConstraintType(tp, k)));
        }
    }
    rec.constraintPoolOff = pushU32List(constraints);
    rec.constraintCount   = static_cast<uint32_t>(constraints.size() / 2);
    rec.outerGenericIdx   = addGeneric(g->getOuterGenericContainer());
    m_generics[idx]       = rec;
    return idx;
}

uint32_t ReflectionSerializer::addDecl(slang::DeclReflection* d, uint32_t parent)
{
    if (!d)
        return fmt::kInvalidIndex;
    void* key = d;
    auto  it  = m_declIdx.find(key);
    if (it != m_declIdx.end())
        return it->second;

    auto idx       = static_cast<uint32_t>(m_decls.size());
    m_declIdx[key] = idx;
    m_decls.push_back({});

    fmt::Decl rec{};
    rec.kind          = static_cast<uint32_t>(d->getKind());
    rec.nameStrIdx    = internStr(d->getName());
    rec.parentDeclIdx = parent;
    rec.payloadIdx    = fmt::kInvalidIndex;

    using Kind = slang::DeclReflection::Kind;
    auto k     = d->getKind();
    if (k == Kind::Struct || k == Kind::Enum)
        rec.payloadIdx = addType(d->getType());
    else if (k == Kind::Func)
        rec.payloadIdx = addFunction(d->asFunction());
    else if (k == Kind::Variable)
        rec.payloadIdx = addVariable(d->asVariable());
    else if (k == Kind::Generic)
        rec.payloadIdx = addGeneric(d->asGeneric());

    unsigned              cc = d->getChildrenCount();
    std::vector<uint32_t> children;
    children.reserve(cc);
    for (unsigned i = 0; i < cc; ++i)
        children.push_back(addDecl(d->getChild(i), idx));
    rec.childOff   = pushU32List(children);
    rec.childCount = cc;
    m_decls[idx]   = rec;
    return idx;
}

uint32_t ReflectionSerializer::addEntryPoint(slang::EntryPointReflection* ep, slang::IComponentType* program,
                                             int entryPointIndex, int targetIndex)
{
    if (!ep)
        return fmt::kInvalidIndex;
    auto            idx = static_cast<uint32_t>(m_entryPoints.size());
    fmt::EntryPoint rec{};
    rec.nameStrIdx         = internStr(ep->getName());
    rec.nameOverrideStrIdx = internStr(ep->getNameOverride());
    rec.stage              = static_cast<uint32_t>(ep->getStage());

    SlangUInt sizes[3] = {0, 0, 0};
    ep->getComputeThreadGroupSize(3, sizes);
    rec.threadGroupSizeX = static_cast<uint32_t>(sizes[0]);
    rec.threadGroupSizeY = static_cast<uint32_t>(sizes[1]);
    rec.threadGroupSizeZ = static_cast<uint32_t>(sizes[2]);

    SlangUInt wave = 0;
    ep->getComputeWaveSize(&wave);
    rec.waveSize                 = static_cast<uint32_t>(wave);
    rec.usesAnySampleRateInput   = ep->usesAnySampleRateInput() ? 1u : 0u;
    rec.hasDefaultConstantBuffer = ep->hasDefaultConstantBuffer() ? 1u : 0u;
    rec.varLayoutIdx             = addVarLayout(ep->getVarLayout());
    rec.typeLayoutIdx            = addTypeLayout(ep->getTypeLayout());
    rec.resultVarLayoutIdx       = addVarLayout(ep->getResultVarLayout());
    rec.functionIdx              = addFunction(ep->getFunction());

    unsigned              pc = ep->getParameterCount();
    std::vector<uint32_t> params(pc);
    for (unsigned i = 0; i < pc; ++i)
        params[i] = addVarLayout(ep->getParameterByIndex(i));
    rec.paramVarLayoutOff   = pushU32List(params);
    rec.paramVarLayoutCount = pc;

    auto attrs    = addAttributesFromFunc(ep->getFunction());
    rec.attrOff   = attrs.first;
    rec.attrCount = attrs.second;

    rec.hashLow  = 0;
    rec.hashHigh = 0;
    if (program)
    {
        Slang::ComPtr<slang::IBlob> hashBlob;
        program->getEntryPointHash(entryPointIndex, targetIndex, hashBlob.writeRef());
        if (hashBlob && hashBlob->getBufferSize() >= sizeof(uint64_t))
        {
            uint64_t h = 0;
            std::memcpy(&h, hashBlob->getBufferPointer(), sizeof(uint64_t));
            rec.hashLow  = static_cast<uint32_t>(h & 0xFFFFFFFFu);
            rec.hashHigh = static_cast<uint32_t>(h >> 32);
        }
    }

    m_entryPoints.push_back(rec);
    return idx;
}

std::vector<uint8_t> ReflectionSerializer::serialize(slang::IComponentType* program, slang::IModule* module,
                                                     int targetIndex)
{
    Slang::ComPtr<slang::IBlob> diag;
    auto*                       layout = program->getLayout(targetIndex, diag.writeRef());
    if (!layout)
        return {};

    unsigned paramCount = layout->getParameterCount();
    for (unsigned i = 0; i < paramCount; ++i)
        addVarLayout(layout->getParameterByIndex(i));
    uint32_t globalVL = addVarLayout(layout->getGlobalParamsVarLayout());

    if (module)
    {
        if (auto* moduleDecl = module->getModuleReflection())
            addDecl(moduleDecl, fmt::kInvalidIndex);
    }

    SlangUInt epc = layout->getEntryPointCount();
    for (SlangUInt i = 0; i < epc; ++i)
        addEntryPoint(layout->getEntryPointByIndex(i), program, static_cast<int>(i), targetIndex);

    SlangUInt hsc = layout->getHashedStringCount();
    for (SlangUInt i = 0; i < hsc; ++i)
    {
        size_t         sz = 0;
        const char*    s  = layout->getHashedString(i, &sz);
        fmt::HashedStr rec{};
        rec.strIdx = (s ? internStr(std::string_view(s, sz)) : fmt::kInvalidIndex);
        // Slang uses a stable FNV-1a 32 of the string bytes for these.
        uint32_t h = 0x811c9dc5u;
        if (s)
        {
            for (size_t k = 0; k < sz; ++k)
            {
                h ^= static_cast<uint8_t>(s[k]);
                h *= 0x01000193u;
            }
        }
        rec.hash = h;
        m_hashedStrs.push_back(rec);
    }
    (void)m_globalSession;

    fmt::ReflHeader hdr{};
    hdr.magic              = fmt::kReflectionMagic;
    hdr.version            = fmt::kReflectionVersion;
    hdr.flags              = 0;
    hdr.entryPointHashLow  = m_entryPoints.empty() ? 0u : m_entryPoints[0].hashLow;
    hdr.entryPointHashHigh = m_entryPoints.empty() ? 0u : m_entryPoints[0].hashHigh;
    {
        SlangUInt b         = layout->getGlobalConstantBufferBinding();
        hdr.globalCbBinding = b == SLANG_UNKNOWN_SIZE ? fmt::kInvalidIndex : static_cast<uint32_t>(b);
        size_t cs           = layout->getGlobalConstantBufferSize();
        hdr.globalCbSize =
            cs == SLANG_UNKNOWN_SIZE || cs == SLANG_UNBOUNDED_SIZE ? fmt::kInvalidIndex : static_cast<uint32_t>(cs);
    }
    hdr.globalParamsVarLayoutIdx = globalVL;
    {
        SlangInt bls           = layout->getBindlessSpaceIndex();
        hdr.bindlessSpaceIndex = (bls < 0) ? fmt::kInvalidIndex : static_cast<uint32_t>(bls);
    }

    auto layoutTable = [](size_t& cur, size_t count, size_t stride) -> uint32_t
    {
        cur      = align8(cur);
        auto off = static_cast<uint32_t>(cur);
        cur += count * stride;
        return off;
    };

    size_t cur = sizeof(fmt::ReflHeader);

    cur              = align8(cur);
    hdr.strings_off  = static_cast<uint32_t>(cur);
    hdr.strings_size = static_cast<uint32_t>(m_strings.size());
    cur += m_strings.size();

    hdr.hashedStr_off         = layoutTable(cur, m_hashedStrs.size(), sizeof(fmt::HashedStr));
    hdr.hashedStr_count       = static_cast<uint32_t>(m_hashedStrs.size());
    hdr.attrArg_off           = layoutTable(cur, m_attrArgs.size(), sizeof(fmt::AttrArg));
    hdr.attrArg_count         = static_cast<uint32_t>(m_attrArgs.size());
    hdr.attr_off              = layoutTable(cur, m_attrs.size(), sizeof(fmt::Attribute));
    hdr.attr_count            = static_cast<uint32_t>(m_attrs.size());
    hdr.modifier_off          = layoutTable(cur, m_modifierPool.size(), sizeof(uint32_t));
    hdr.modifier_count        = static_cast<uint32_t>(m_modifierPool.size());
    hdr.type_off              = layoutTable(cur, m_types.size(), sizeof(fmt::Type));
    hdr.type_count            = static_cast<uint32_t>(m_types.size());
    hdr.typeLayout_off        = layoutTable(cur, m_typeLayouts.size(), sizeof(fmt::TypeLayout));
    hdr.typeLayout_count      = static_cast<uint32_t>(m_typeLayouts.size());
    hdr.var_off               = layoutTable(cur, m_vars.size(), sizeof(fmt::Variable));
    hdr.var_count             = static_cast<uint32_t>(m_vars.size());
    hdr.varLayout_off         = layoutTable(cur, m_varLayouts.size(), sizeof(fmt::VarLayout));
    hdr.varLayout_count       = static_cast<uint32_t>(m_varLayouts.size());
    hdr.func_off              = layoutTable(cur, m_funcs.size(), sizeof(fmt::Function));
    hdr.func_count            = static_cast<uint32_t>(m_funcs.size());
    hdr.generic_off           = layoutTable(cur, m_generics.size(), sizeof(fmt::Generic));
    hdr.generic_count         = static_cast<uint32_t>(m_generics.size());
    hdr.decl_off              = layoutTable(cur, m_decls.size(), sizeof(fmt::Decl));
    hdr.decl_count            = static_cast<uint32_t>(m_decls.size());
    hdr.entryPoint_off        = layoutTable(cur, m_entryPoints.size(), sizeof(fmt::EntryPoint));
    hdr.entryPoint_count      = static_cast<uint32_t>(m_entryPoints.size());
    hdr.bindingRange_off      = layoutTable(cur, m_bindingRanges.size(), sizeof(fmt::BindingRange));
    hdr.bindingRange_count    = static_cast<uint32_t>(m_bindingRanges.size());
    hdr.descriptorSet_off     = layoutTable(cur, m_descriptorSets.size(), sizeof(fmt::DescriptorSet));
    hdr.descriptorSet_count   = static_cast<uint32_t>(m_descriptorSets.size());
    hdr.descriptorRange_off   = layoutTable(cur, m_descriptorRanges.size(), sizeof(fmt::DescriptorRange));
    hdr.descriptorRange_count = static_cast<uint32_t>(m_descriptorRanges.size());
    hdr.subObjectRange_off    = layoutTable(cur, m_subObjectRanges.size(), sizeof(fmt::SubObjectRange));
    hdr.subObjectRange_count  = static_cast<uint32_t>(m_subObjectRanges.size());
    hdr.u32Pool_off           = layoutTable(cur, m_u32Pool.size(), sizeof(uint32_t));
    hdr.u32Pool_count         = static_cast<uint32_t>(m_u32Pool.size());

    hdr.size = static_cast<uint32_t>(cur);

    std::vector<uint8_t> out;
    out.reserve(cur);
    appendBytes(out, hdr);

    auto writeTable = [&](uint32_t off, const void* data, size_t bytes)
    {
        while (out.size() < off)
            out.push_back(0);
        appendBytes(out, data, bytes);
    };

    writeTable(hdr.strings_off, m_strings.data(), m_strings.size());
    writeTable(hdr.hashedStr_off, m_hashedStrs.data(), m_hashedStrs.size() * sizeof(fmt::HashedStr));
    writeTable(hdr.attrArg_off, m_attrArgs.data(), m_attrArgs.size() * sizeof(fmt::AttrArg));
    writeTable(hdr.attr_off, m_attrs.data(), m_attrs.size() * sizeof(fmt::Attribute));
    writeTable(hdr.modifier_off, m_modifierPool.data(), m_modifierPool.size() * sizeof(uint32_t));
    writeTable(hdr.type_off, m_types.data(), m_types.size() * sizeof(fmt::Type));
    writeTable(hdr.typeLayout_off, m_typeLayouts.data(), m_typeLayouts.size() * sizeof(fmt::TypeLayout));
    writeTable(hdr.var_off, m_vars.data(), m_vars.size() * sizeof(fmt::Variable));
    writeTable(hdr.varLayout_off, m_varLayouts.data(), m_varLayouts.size() * sizeof(fmt::VarLayout));
    writeTable(hdr.func_off, m_funcs.data(), m_funcs.size() * sizeof(fmt::Function));
    writeTable(hdr.generic_off, m_generics.data(), m_generics.size() * sizeof(fmt::Generic));
    writeTable(hdr.decl_off, m_decls.data(), m_decls.size() * sizeof(fmt::Decl));
    writeTable(hdr.entryPoint_off, m_entryPoints.data(), m_entryPoints.size() * sizeof(fmt::EntryPoint));
    writeTable(hdr.bindingRange_off, m_bindingRanges.data(), m_bindingRanges.size() * sizeof(fmt::BindingRange));
    writeTable(hdr.descriptorSet_off, m_descriptorSets.data(), m_descriptorSets.size() * sizeof(fmt::DescriptorSet));
    writeTable(hdr.descriptorRange_off, m_descriptorRanges.data(),
               m_descriptorRanges.size() * sizeof(fmt::DescriptorRange));
    writeTable(hdr.subObjectRange_off, m_subObjectRanges.data(),
               m_subObjectRanges.size() * sizeof(fmt::SubObjectRange));
    writeTable(hdr.u32Pool_off, m_u32Pool.data(), m_u32Pool.size() * sizeof(uint32_t));

    while (out.size() < cur)
        out.push_back(0);

    std::memcpy(out.data(), &hdr, sizeof(hdr));
    return out;
}

} // namespace

namespace detail
{

std::vector<uint8_t> serializeReflection(slang::IGlobalSession* gs, slang::IComponentType* program,
                                         slang::IModule* module, int targetIndex)
{
    ReflectionSerializer ser(gs);
    return ser.serialize(program, module, targetIndex);
}

} // namespace detail

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

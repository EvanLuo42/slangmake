#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "slangmake_internal.h"

namespace slangmake
{

SlangCompileTarget toSlangCompileTarget(Target t)
{
    switch (t)
    {
    case Target::SPIRV:
        return SLANG_SPIRV;
    case Target::DXIL:
        return SLANG_DXIL;
    case Target::DXBC:
        return SLANG_DXBC;
    case Target::HLSL:
        return SLANG_HLSL;
    case Target::GLSL:
        return SLANG_GLSL;
    case Target::Metal:
        return SLANG_METAL;
    case Target::MetalLib:
        return SLANG_METAL_LIB;
    case Target::WGSL:
        return SLANG_WGSL;
    }
    return SLANG_TARGET_UNKNOWN;
}

class Compiler::Impl
{
public:
    Slang::ComPtr<slang::IGlobalSession> globalSession;
};

Compiler::Compiler()
    : m_impl(std::make_unique<Impl>())
{
    if (const SlangResult rs = slang::createGlobalSession(m_impl->globalSession.writeRef()); SLANG_FAILED(rs))
        throw std::runtime_error("slang::createGlobalSession failed");
}

Compiler::~Compiler() = default;

namespace
{

slang::CompilerOptionEntry makeIntOpt(const slang::CompilerOptionName n, int v)
{
    slang::CompilerOptionEntry e{};
    e.name            = n;
    e.value.kind      = slang::CompilerOptionValueKind::Int;
    e.value.intValue0 = v;
    return e;
}
slang::CompilerOptionEntry makeIntInt(slang::CompilerOptionName n, int v0, int v1)
{
    slang::CompilerOptionEntry e{};
    e.name            = n;
    e.value.kind      = slang::CompilerOptionValueKind::Int;
    e.value.intValue0 = v0;
    e.value.intValue1 = v1;
    return e;
}
slang::CompilerOptionEntry makeBoolOpt(slang::CompilerOptionName n, const bool v) { return makeIntOpt(n, v ? 1 : 0); }
slang::CompilerOptionEntry makeStrOpt(slang::CompilerOptionName n, const char* s0, const char* s1 = nullptr)
{
    slang::CompilerOptionEntry e{};
    e.name               = n;
    e.value.kind         = slang::CompilerOptionValueKind::String;
    e.value.stringValue0 = s0;
    e.value.stringValue1 = s1;
    return e;
}

std::vector<slang::CompilerOptionEntry> buildOptionEntries(const CompileOptions&     opts,
                                                           std::vector<std::string>& storage)
{
    std::vector<slang::CompilerOptionEntry> entries;
    auto                                    store = [&](std::string s) -> const char*
    {
        storage.push_back(std::move(s));
        return storage.back().c_str();
    };

    int ol = static_cast<int>(opts.optimization);
    entries.push_back(makeIntOpt(slang::CompilerOptionName::Optimization, ol));

    if (opts.matrixLayout == MatrixLayout::RowMajor)
        entries.push_back(makeBoolOpt(slang::CompilerOptionName::MatrixLayoutRow, true));
    else
        entries.push_back(makeBoolOpt(slang::CompilerOptionName::MatrixLayoutColumn, true));

    SlangFloatingPointMode fpm = SLANG_FLOATING_POINT_MODE_DEFAULT;
    switch (opts.fpMode)
    {
    case FloatingPointMode::Fast:
        fpm = SLANG_FLOATING_POINT_MODE_FAST;
        break;
    case FloatingPointMode::Precise:
        fpm = SLANG_FLOATING_POINT_MODE_PRECISE;
        break;
    default:
        break;
    }
    entries.push_back(makeIntOpt(slang::CompilerOptionName::FloatingPointMode, fpm));

    if (opts.debugInfo)
        entries.push_back(makeIntOpt(slang::CompilerOptionName::DebugInformation, SLANG_DEBUG_INFO_LEVEL_STANDARD));
    if (opts.warningsAsErrors)
        entries.push_back(makeStrOpt(slang::CompilerOptionName::WarningsAsErrors, "all"));
    if (opts.glslScalarLayout)
        entries.push_back(makeBoolOpt(slang::CompilerOptionName::GLSLForceScalarLayout, true));
    if (opts.dumpIntermediates)
        entries.push_back(makeBoolOpt(slang::CompilerOptionName::DumpIntermediates, true));
    if (opts.target == Target::SPIRV)
        entries.push_back(makeBoolOpt(opts.emitSpirvDirectly ? slang::CompilerOptionName::EmitSpirvDirectly
                                                             : slang::CompilerOptionName::EmitSpirvViaGLSL,
                                      true));

    for (const auto& bs : opts.vulkanBindShifts)
    {
        int v0 = static_cast<int>(((bs.kind & 0xFFu) << 24) | (bs.space & 0xFFFFFFu));
        entries.push_back(makeIntInt(slang::CompilerOptionName::VulkanBindShift, v0, static_cast<int>(bs.shift)));
    }

    if (!opts.languageVersion.empty())
    {
        try
        {
            int lv = std::stoi(opts.languageVersion);
            entries.push_back(makeIntOpt(slang::CompilerOptionName::LanguageVersion, lv));
        }
        catch (...)
        {
            // ignore: non-integer language version strings are silently dropped
        }
    }

    for (const auto& inc : opts.includePaths)
        entries.push_back(makeStrOpt(slang::CompilerOptionName::Include, store(inc.string())));

    return entries;
}

std::vector<std::string> findDuplicateBindingNames(std::span<const ShaderConstant> values)
{
    std::unordered_map<std::string, uint32_t> counts;
    counts.reserve(values.size());
    for (const auto& value : values)
        ++counts[value.name];

    std::vector<std::string> duplicates;
    duplicates.reserve(counts.size());
    for (const auto& [name, count] : counts)
        if (count > 1)
            duplicates.push_back(name);
    std::ranges::sort(duplicates);
    return duplicates;
}

} // namespace

Compiler::Result Compiler::compile(const CompileOptions& opts, const Permutation& perm) const
{
    Result result;
    if (!m_impl || !m_impl->globalSession)
    {
        result.diagnostics = "no global session";
        return result;
    }
    slang::IGlobalSession* gs = m_impl->globalSession;

    if (auto duplicateTypeArgs = findDuplicateBindingNames(perm.typeArgs); !duplicateTypeArgs.empty())
    {
        result.diagnostics = "duplicate typeArg name(s): ";
        for (size_t i = 0; i < duplicateTypeArgs.size(); ++i)
        {
            if (i)
                result.diagnostics += ", ";
            result.diagnostics += "'" + duplicateTypeArgs[i] + "'";
        }
        result.diagnostics += ". Each module-scope `type_param` may be specialized at most once per permutation.";
        return result;
    }

    // Per-permutation defines = global defines + permutation defines.
    std::vector<std::pair<std::string, std::string>> defineStorage;
    for (const auto& d : opts.defines)
        defineStorage.emplace_back(d.name, d.value);
    for (const auto& d : perm.constants)
        defineStorage.emplace_back(d.name, d.value);

    std::vector<slang::PreprocessorMacroDesc> macroDescs;
    macroDescs.reserve(defineStorage.size());
    for (const auto& d : defineStorage)
        macroDescs.push_back({d.first.c_str(), d.second.c_str()});

    std::vector<std::string> incStorage;
    incStorage.reserve(opts.includePaths.size());
    std::vector<const char*> incPtrs;
    incPtrs.reserve(opts.includePaths.size());
    for (const auto& p : opts.includePaths)
    {
        incStorage.push_back(p.string());
        incPtrs.push_back(incStorage.back().c_str());
    }

    SlangCompileTarget tgt     = toSlangCompileTarget(opts.target);
    SlangProfileID     profile = SLANG_PROFILE_UNKNOWN;
    if (!opts.profile.empty())
        profile = gs->findProfile(opts.profile.c_str());

    slang::TargetDesc targetDesc{};
    targetDesc.format                      = tgt;
    targetDesc.profile                     = profile;
    targetDesc.flags                       = 0;
    targetDesc.floatingPointMode           = SLANG_FLOATING_POINT_MODE_DEFAULT;
    targetDesc.lineDirectiveMode           = SLANG_LINE_DIRECTIVE_MODE_DEFAULT;
    targetDesc.forceGLSLScalarBufferLayout = opts.glslScalarLayout;

    std::vector<std::string>                storage;
    std::vector<slang::CompilerOptionEntry> optEntries = buildOptionEntries(opts, storage);

    slang::SessionDesc sessionDesc{};
    sessionDesc.targets                  = &targetDesc;
    sessionDesc.targetCount              = 1;
    sessionDesc.defaultMatrixLayoutMode  = (opts.matrixLayout == MatrixLayout::RowMajor)
                                               ? SLANG_MATRIX_LAYOUT_ROW_MAJOR
                                               : SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
    sessionDesc.searchPaths              = incPtrs.empty() ? nullptr : incPtrs.data();
    sessionDesc.searchPathCount          = static_cast<SlangInt>(incPtrs.size());
    sessionDesc.preprocessorMacros       = macroDescs.empty() ? nullptr : macroDescs.data();
    sessionDesc.preprocessorMacroCount   = static_cast<SlangInt>(macroDescs.size());
    sessionDesc.compilerOptionEntries    = optEntries.empty() ? nullptr : optEntries.data();
    sessionDesc.compilerOptionEntryCount = static_cast<uint32_t>(optEntries.size());

    Slang::ComPtr<slang::ISession> session;
    if (SLANG_FAILED(gs->createSession(sessionDesc, session.writeRef())))
    {
        result.diagnostics = "failed to create session";
        return result;
    }

    std::ifstream f(opts.inputFile, std::ios::binary);
    if (!f)
    {
        result.diagnostics = "cannot open input: " + opts.inputFile.string();
        return result;
    }
    std::string source((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    std::string                 moduleName = opts.inputFile.stem().string();
    Slang::ComPtr<slang::IBlob> diag;
    auto* module = session->loadModuleFromSourceString(moduleName.c_str(), opts.inputFile.string().c_str(),
                                                       source.c_str(), diag.writeRef());
    if (diag && diag->getBufferSize() > 0)
        result.diagnostics.append(static_cast<const char*>(diag->getBufferPointer()), diag->getBufferSize());
    if (!module)
        return result;

    // Record dependency paths so the batch layer can build a content-hash
    // manifest for incremental-rebuild decisions.
    {
        SlangInt32 depCount = module->getDependencyFileCount();
        result.dependencies.reserve(static_cast<size_t>(depCount));
        for (SlangInt32 i = 0; i < depCount; ++i)
        {
            const char* p = module->getDependencyFilePath(i);
            if (p)
                result.dependencies.emplace_back(p);
        }
    }

    std::vector<Slang::ComPtr<slang::IEntryPoint>> entryPoints;
    if (!opts.entryPoint.empty())
    {
        Slang::ComPtr<slang::IEntryPoint> ep;
        if (SLANG_FAILED(module->findEntryPointByName(opts.entryPoint.c_str(), ep.writeRef())) || !ep)
        {
            result.diagnostics += "\nentry point not found: " + opts.entryPoint;
            return result;
        }
        entryPoints.push_back(ep);
    }
    else
    {
        SlangInt32 count = module->getDefinedEntryPointCount();
        for (SlangInt32 i = 0; i < count; ++i)
        {
            Slang::ComPtr<slang::IEntryPoint> ep;
            if (SLANG_SUCCEEDED(module->getDefinedEntryPoint(i, ep.writeRef())) && ep)
                entryPoints.push_back(ep);
        }
        if (entryPoints.empty())
        {
            result.diagnostics += "\nno entry points defined in module";
            return result;
        }
    }

    std::vector<slang::IComponentType*> components;
    components.push_back(module);
    for (auto& ep : entryPoints)
        components.push_back(ep.get());

    Slang::ComPtr<slang::IComponentType> composite;
    Slang::ComPtr<slang::IBlob>          compDiag;
    if (SLANG_FAILED(session->createCompositeComponentType(components.data(), static_cast<SlangInt>(components.size()),
                                                           composite.writeRef(), compDiag.writeRef())))
    {
        if (compDiag)
            result.diagnostics.append(static_cast<const char*>(compDiag->getBufferPointer()),
                                      compDiag->getBufferSize());
        return result;
    }

    // If the permutation binds module-scope generic type parameters, resolve
    // them by name against the unspecialized program layout, build a
    // positional SpecializationArg array, and produce a specialized component
    // that the linker/codegen can consume.
    if (!perm.typeArgs.empty())
    {
        Slang::ComPtr<slang::IBlob> layoutDiag;
        slang::ProgramLayout*       preLayout = composite->getLayout(0, layoutDiag.writeRef());
        if (layoutDiag && layoutDiag->getBufferSize() > 0)
            result.diagnostics.append(static_cast<const char*>(layoutDiag->getBufferPointer()),
                                      layoutDiag->getBufferSize());
        if (!preLayout)
        {
            result.diagnostics += "\nfailed to obtain unspecialized program layout";
            return result;
        }

        const unsigned tpCount    = preLayout->getTypeParameterCount();
        const SlangInt totalSpecs = composite->getSpecializationParamCount();

        // slangmake only knows how to positionally bind module-scope `type_param`
        // declarations. Interface-typed globals (e.g. `IFoo gFoo;`), entry-point
        // generic parameters, and specialisation-gated existentials also show
        // up in getSpecializationParamCount() but cannot be reached through
        // ProgramLayout::getTypeParameterByIndex. Refuse the compile explicitly
        // rather than silently building a short/misaligned argument array.
        if (totalSpecs != static_cast<SlangInt>(tpCount))
        {
            result.diagnostics += "\nmodule has " + std::to_string(totalSpecs) +
                                  " specialization parameters but only " + std::to_string(tpCount) +
                                  " are declared as module-scope `type_param` — slangmake's typeArgs can only "
                                  "bind type_param axes. Refactor the remaining specialisation into type_param "
                                  "declarations or pre-bake them via permutation constants.";
            return result;
        }

        std::unordered_map<std::string, std::string> argByName;
        argByName.reserve(perm.typeArgs.size());
        for (const auto& ta : perm.typeArgs)
            argByName.emplace(ta.name, ta.value);

        // Collect declared names first so we can report strays against a
        // concrete list (and so the fast-path type lookup has a stable order).
        std::vector<std::string> declaredNames;
        declaredNames.reserve(tpCount);
        for (unsigned i = 0; i < tpCount; ++i)
        {
            auto* tp = preLayout->getTypeParameterByIndex(i);
            if (!tp)
            {
                result.diagnostics += "\ninternal: null type parameter at index " + std::to_string(i);
                return result;
            }
            const char* n = tp->getName();
            declaredNames.emplace_back(n ? n : "");
        }

        // Fail fast on stray typeArgs — almost always a typo. Do this before
        // invoking the (expensive, less-legible) slang type checker.
        {
            std::vector<std::string> strays;
            for (const auto& [n, _] : argByName)
            {
                if (std::find(declaredNames.begin(), declaredNames.end(), n) == declaredNames.end())
                    strays.push_back(n);
            }
            if (!strays.empty())
            {
                std::ranges::sort(strays);
                std::string msg = "\nunknown typeArg(s) with no matching module-scope `type_param`: ";
                for (size_t i = 0; i < strays.size(); ++i)
                {
                    if (i)
                        msg += ", ";
                    msg += "'" + strays[i] + "'";
                }
                msg += ". Declared parameters: ";
                for (size_t i = 0; i < declaredNames.size(); ++i)
                {
                    if (i)
                        msg += ", ";
                    msg += "'" + declaredNames[i] + "'";
                }
                result.diagnostics += msg;
                return result;
            }
        }

        // Stable storage for any expression strings we hand to Slang as
        // `SpecializationArg::fromExpr`. fromType args reference TypeReflection
        // pointers owned by the session, so they don't need backing.
        std::vector<std::string> exprStorage;
        exprStorage.reserve(tpCount);
        std::vector<slang::SpecializationArg> specArgs;
        specArgs.reserve(tpCount);

        for (unsigned i = 0; i < tpCount; ++i)
        {
            auto it = argByName.find(declaredNames[i]);
            if (it == argByName.end())
            {
                result.diagnostics += "\nmissing typeArg for generic parameter '" + declaredNames[i] + "'";
                return result;
            }
            const std::string& value = it->second;

            // Fast path: plain type name. Resolving via reflection surfaces
            // clean "unknown type X" errors from the type checker rather than
            // from the deep specializer, and reuses a parsed TypeReflection*.
            slang::TypeReflection* resolved = preLayout->findTypeByName(value.c_str());
            if (resolved)
            {
                specArgs.push_back(slang::SpecializationArg::fromType(resolved));
            }
            else
            {
                // Fallback: Slang expression (e.g. `Array<Metal, 4>`, `Gen<Float3>`).
                // Slang will parse and check it as part of specialize().
                exprStorage.push_back(value);
                specArgs.push_back(slang::SpecializationArg::fromExpr(exprStorage.back().c_str()));
            }
        }

        Slang::ComPtr<slang::IComponentType> specialized;
        Slang::ComPtr<slang::IBlob>          specDiag;
        if (SLANG_FAILED(composite->specialize(specArgs.data(), static_cast<SlangInt>(specArgs.size()),
                                               specialized.writeRef(), specDiag.writeRef())))
        {
            if (specDiag)
                result.diagnostics.append(static_cast<const char*>(specDiag->getBufferPointer()),
                                          specDiag->getBufferSize());
            return result;
        }
        composite = specialized;
    }

    Slang::ComPtr<slang::IComponentType> linked;
    Slang::ComPtr<slang::IBlob>          linkDiag;
    if (SLANG_FAILED(composite->link(linked.writeRef(), linkDiag.writeRef())))
    {
        if (linkDiag)
            result.diagnostics.append(static_cast<const char*>(linkDiag->getBufferPointer()),
                                      linkDiag->getBufferSize());
        return result;
    }

    Slang::ComPtr<slang::IBlob> codeBlob;
    Slang::ComPtr<slang::IBlob> codeDiag;
    if (SLANG_FAILED(linked->getTargetCode(0, codeBlob.writeRef(), codeDiag.writeRef())))
    {
        if (codeDiag)
            result.diagnostics.append(static_cast<const char*>(codeDiag->getBufferPointer()),
                                      codeDiag->getBufferSize());
        return result;
    }
    if (codeBlob && codeBlob->getBufferSize() > 0)
    {
        const auto* p = static_cast<const uint8_t*>(codeBlob->getBufferPointer());
        result.code.assign(p, p + codeBlob->getBufferSize());
    }

    if (opts.emitReflection)
        result.reflection = detail::serializeReflection(gs, linked, module, 0);
    result.success = true;
    return result;
}

} // namespace slangmake

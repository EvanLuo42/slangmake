#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "slangmake_internal.h"

namespace slangmake
{

Compiler::Compiler()
{
    if (const SlangResult rs = slang::createGlobalSession(m_globalSession.writeRef()); SLANG_FAILED(rs))
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

} // namespace

Compiler::Result Compiler::compile(const CompileOptions& opts, const Permutation& perm) const
{
    Result result;
    if (!m_globalSession)
    {
        result.diagnostics = "no global session";
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
        profile = m_globalSession->findProfile(opts.profile.c_str());

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
    if (SLANG_FAILED(m_globalSession->createSession(sessionDesc, session.writeRef())))
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
        result.reflection = detail::serializeReflection(m_globalSession, linked, module, 0);
    result.success = true;
    return result;
}

} // namespace slangmake

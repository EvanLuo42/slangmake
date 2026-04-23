#include <doctest/doctest.h>
#include <slang-com-ptr.h>
#include <slang.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "slangmake.h"

using namespace slangmake;
namespace fs = std::filesystem;

namespace
{

// Compile the same shader straight against the Slang API the way slangmake does
// internally — minus reflection serialisation. The bytecode produced here is the
// reference our blob-extracted bytecode must match exactly.
std::vector<uint8_t> compileViaRawSlang(const fs::path&                  shaderPath,
                                        const char*                      profileName,
                                        const std::vector<ShaderConstant>& defines = {})
{
    Slang::ComPtr<slang::IGlobalSession> globalSession;
    REQUIRE(SLANG_SUCCEEDED(slang::createGlobalSession(globalSession.writeRef())));

    slang::TargetDesc targetDesc{};
    targetDesc.format  = SLANG_SPIRV;
    targetDesc.profile = globalSession->findProfile(profileName);
    targetDesc.flags   = 0;

    // Match the option entries Compiler::compile sets when defaults are used.
    std::vector<slang::CompilerOptionEntry> entries;
    {
        slang::CompilerOptionEntry e{};
        e.name            = slang::CompilerOptionName::Optimization;
        e.value.kind      = slang::CompilerOptionValueKind::Int;
        e.value.intValue0 = static_cast<int>(Optimization::High);
        entries.push_back(e);
    }
    {
        slang::CompilerOptionEntry e{};
        e.name            = slang::CompilerOptionName::MatrixLayoutRow;
        e.value.kind      = slang::CompilerOptionValueKind::Int;
        e.value.intValue0 = 1;
        entries.push_back(e);
    }
    {
        slang::CompilerOptionEntry e{};
        e.name            = slang::CompilerOptionName::FloatingPointMode;
        e.value.kind      = slang::CompilerOptionValueKind::Int;
        e.value.intValue0 = SLANG_FLOATING_POINT_MODE_DEFAULT;
        entries.push_back(e);
    }
    {
        slang::CompilerOptionEntry e{};
        e.name            = slang::CompilerOptionName::EmitSpirvDirectly;
        e.value.kind      = slang::CompilerOptionValueKind::Int;
        e.value.intValue0 = 1;
        entries.push_back(e);
    }

    std::vector<slang::PreprocessorMacroDesc> macroDescs;
    macroDescs.reserve(defines.size());
    for (const auto& d : defines)
        macroDescs.push_back({d.name.c_str(), d.value.c_str()});

    slang::SessionDesc sessionDesc{};
    sessionDesc.targets                  = &targetDesc;
    sessionDesc.targetCount              = 1;
    sessionDesc.defaultMatrixLayoutMode  = SLANG_MATRIX_LAYOUT_ROW_MAJOR;
    sessionDesc.preprocessorMacros       = macroDescs.empty() ? nullptr : macroDescs.data();
    sessionDesc.preprocessorMacroCount   = static_cast<SlangInt>(macroDescs.size());
    sessionDesc.compilerOptionEntries    = entries.data();
    sessionDesc.compilerOptionEntryCount = static_cast<uint32_t>(entries.size());

    Slang::ComPtr<slang::ISession> session;
    REQUIRE(SLANG_SUCCEEDED(globalSession->createSession(sessionDesc, session.writeRef())));

    std::ifstream f(shaderPath, std::ios::binary);
    REQUIRE(f.good());
    std::string source((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    std::string                 moduleName = shaderPath.stem().string();
    Slang::ComPtr<slang::IBlob> diag;
    auto* module = session->loadModuleFromSourceString(moduleName.c_str(), shaderPath.string().c_str(), source.c_str(),
                                                       diag.writeRef());
    if (diag && diag->getBufferSize() > 0)
    {
        INFO("raw load diag: " << std::string_view(static_cast<const char*>(diag->getBufferPointer()),
                                                   diag->getBufferSize()));
    }
    REQUIRE(module != nullptr);

    std::vector<Slang::ComPtr<slang::IEntryPoint>> eps;
    SlangInt32                                     epc = module->getDefinedEntryPointCount();
    for (SlangInt32 i = 0; i < epc; ++i)
    {
        Slang::ComPtr<slang::IEntryPoint> ep;
        REQUIRE(SLANG_SUCCEEDED(module->getDefinedEntryPoint(i, ep.writeRef())));
        eps.push_back(ep);
    }
    REQUIRE_FALSE(eps.empty());

    std::vector<slang::IComponentType*> components;
    components.push_back(static_cast<slang::IComponentType*>(module));
    for (auto& ep : eps)
        components.push_back(static_cast<slang::IComponentType*>(ep.get()));

    Slang::ComPtr<slang::IComponentType> composite;
    Slang::ComPtr<slang::IBlob>          compDiag;
    REQUIRE(SLANG_SUCCEEDED(session->createCompositeComponentType(
        components.data(), static_cast<SlangInt>(components.size()), composite.writeRef(), compDiag.writeRef())));

    Slang::ComPtr<slang::IComponentType> linked;
    Slang::ComPtr<slang::IBlob>          linkDiag;
    REQUIRE(SLANG_SUCCEEDED(composite->link(linked.writeRef(), linkDiag.writeRef())));

    Slang::ComPtr<slang::IBlob> codeBlob;
    Slang::ComPtr<slang::IBlob> codeDiag;
    REQUIRE(SLANG_SUCCEEDED(linked->getTargetCode(0, codeBlob.writeRef(), codeDiag.writeRef())));
    REQUIRE(codeBlob != nullptr);

    const auto* p = static_cast<const uint8_t*>(codeBlob->getBufferPointer());
    return std::vector<uint8_t>(p, p + codeBlob->getBufferSize());
}

} // namespace

TEST_CASE("SPIRV bytes round-trip through slangmake.bin == raw slang output")
{
    auto shader = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "compute.slang";

    auto reference = compileViaRawSlang(shader, "sm_6_5");
    REQUIRE(reference.size() >= 4);
    uint32_t magic;
    std::memcpy(&magic, reference.data(), 4);
    CHECK(magic == 0x07230203u);

    // Now go through the slangmake stack: compile → blob → read → extract.
    Compiler      c;
    BatchCompiler bc(c);
    bc.setQuiet(true);

    BatchCompiler::Input in;
    in.file            = shader;
    in.options.target  = Target::SPIRV;
    in.options.profile = "sm_6_5";

    auto outDir = fs::temp_directory_path() / "slangmake_spirv_match";
    fs::remove_all(outDir);
    fs::create_directories(outDir);
    auto outPath = outDir / "compute.bin";

    auto out = bc.compileFile(in, outPath);
    REQUIRE(out.failures.empty());
    REQUIRE(out.compiled.size() == 1);

    auto reader = BlobReader::openFile(outPath);
    REQUIRE(reader.has_value());
    REQUIRE(reader->valid());
    REQUIRE(reader->entryCount() == 1);

    auto                 entry = reader->at(0);
    std::vector<uint8_t> extracted(entry.code.begin(), entry.code.end());

    CHECK(extracted.size() == reference.size());
    CHECK(extracted == reference);

    // Reflection section should also be present and well-formed.
    CHECK_FALSE(entry.reflection.empty());
    ReflectionView rv(entry.reflection);
    CHECK(rv.valid());

    fs::remove_all(outDir);
}

TEST_CASE("Every permutation in the blob matches raw-slang output byte-for-byte")
{
    auto shader = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "permuted.slang";

    // Run the full slangmake pipeline — parse directives, expand, compile, pack.
    Compiler      c;
    BatchCompiler bc(c);
    bc.setQuiet(true);

    BatchCompiler::Input in;
    in.file            = shader;
    in.options.target  = Target::SPIRV;
    in.options.profile = "sm_6_5";

    auto outDir = fs::temp_directory_path() / "slangmake_spirv_match_all";
    fs::remove_all(outDir);
    fs::create_directories(outDir);
    auto outPath = outDir / "permuted.bin";

    auto out = bc.compileFile(in, outPath);
    REQUIRE(out.failures.empty());
    // permuted.slang has USE_SHADOW={0,1} × QUALITY={0,1,2}
    REQUIRE(out.compiled.size() == 6);

    auto reader = BlobReader::openFile(outPath);
    REQUIRE(reader.has_value());
    REQUIRE(reader->valid());
    REQUIRE(reader->entryCount() == 6);

    // For each permutation in the blob, re-compile the shader via the raw Slang
    // API with the same defines and assert byte-equal SPIRV.
    for (const auto& perm : out.compiled) {
        auto entry = reader->find(std::span<const ShaderConstant>(perm.constants));
        REQUIRE_MESSAGE(entry.has_value(), "missing blob entry for " << perm.key());

        auto reference = compileViaRawSlang(shader, "sm_6_5", perm.constants);
        REQUIRE(reference.size() >= 4);

        std::vector<uint8_t> extracted(entry->code.begin(), entry->code.end());
        CHECK_MESSAGE(extracted.size() == reference.size(),
                      "size mismatch for " << perm.key());
        CHECK_MESSAGE(extracted == reference,
                      "byte mismatch for " << perm.key());

        // Reflection section per permutation is well-formed too.
        CHECK_FALSE(entry->reflection.empty());
        ReflectionView rv(entry->reflection);
        CHECK(rv.valid());
    }

    fs::remove_all(outDir);
}

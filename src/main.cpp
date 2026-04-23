#include <CLI/CLI.hpp>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "slangmake_internal.h"
#include "slangmake_version.h"

namespace fs = std::filesystem;
using namespace slangmake;

namespace
{

bool parseDefine(const std::string& s, ShaderConstant& out)
{
    auto eq = s.find('=');
    if (eq == std::string::npos)
    {
        out.name  = s;
        out.value = "1";
    }
    else
    {
        out.name  = s.substr(0, eq);
        out.value = s.substr(eq + 1);
    }
    return !out.name.empty();
}

bool parsePermutation(const std::string& s, PermutationDefine& out, bool allowAngleNesting)
{
    // NAME={a,b,c}
    auto trimCopy = [](std::string_view sv) -> std::string
    {
        size_t a = 0, b = sv.size();
        while (a < b && std::isspace(static_cast<unsigned char>(sv[a])))
            ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(sv[b - 1])))
            --b;
        return std::string(sv.substr(a, b - a));
    };

    auto eq = s.find('=');
    if (eq == std::string::npos)
        return false;
    out.name  = trimCopy(std::string_view(s).substr(0, eq));
    auto rest = trimCopy(std::string_view(s).substr(eq + 1));
    if (rest.size() < 2 || rest.front() != '{' || rest.back() != '}')
        return false;
    auto values =
        detail::parsePermutationValueList(std::string_view(rest).substr(1, rest.size() - 2), allowAngleNesting);
    if (!values.has_value())
        return false;
    out.values = std::move(*values);
    return !out.name.empty() && !out.values.empty();
}

bool parseBindShift(const std::string& s, VulkanBindShift& out)
{
    // kind:space:shift  e.g. t:0:16
    if (s.size() < 5 || s[1] != ':')
        return false;
    char kindChar = s[0];
    out.kind      = static_cast<uint32_t>((unsigned char)kindChar);
    auto rest     = s.substr(2);
    auto colon    = rest.find(':');
    if (colon == std::string::npos)
        return false;
    try
    {
        out.space = std::stoul(rest.substr(0, colon));
        out.shift = std::stoul(rest.substr(colon + 1));
    }
    catch (...)
    {
        return false;
    }
    return true;
}

int runDump(const fs::path& path, bool includeReflection)
{
    auto reader = BlobReader::openFile(path);
    if (!reader)
    {
        std::fprintf(stderr, "error: cannot open blob: %s\n", path.string().c_str());
        return 1;
    }
    if (!reader->valid())
    {
        std::fprintf(stderr, "error: invalid or corrupt blob: %s\n", path.string().c_str());
        return 1;
    }

    std::error_code ec;
    auto            fileSize = fs::file_size(path, ec);

    std::printf("blob: %s\n", path.string().c_str());
    if (!ec)
        std::printf("  file size      : %llu bytes\n", static_cast<unsigned long long>(fileSize));
    std::printf("  target         : %s\n", targetToString(reader->target()));
    std::printf("  compression    : %s\n", codecToString(reader->compression()));
    std::printf("  options hash   : 0x%016llx\n", static_cast<unsigned long long>(reader->optionsHash()));
    std::printf("  entry count    : %zu\n", reader->entryCount());

    auto deps = reader->dependencies();
    std::printf("\ndependencies (%zu):\n", deps.size());
    for (size_t i = 0; i < deps.size(); ++i)
    {
        std::printf("  [%zu] %.*s  (hash 0x%016llx)\n", i, static_cast<int>(deps[i].path.size()), deps[i].path.data(),
                    static_cast<unsigned long long>(deps[i].contentHash));
    }

    std::printf("\nentries:\n");
    for (size_t i = 0; i < reader->entryCount(); ++i)
    {
        auto e = reader->at(i);
        std::printf("  [%zu] \"%.*s\"\n", i, static_cast<int>(e.key.size()), e.key.data());
        std::printf("        code size      : %zu bytes\n", e.code.size());
        std::printf("        reflection size: %zu bytes\n", e.reflection.size());
        std::printf("        dep indices    : [");
        for (size_t k = 0; k < e.depIndices.size(); ++k)
        {
            if (k)
                std::printf(", ");
            std::printf("%u", e.depIndices[k]);
        }
        std::printf("]\n");

        if (includeReflection && !e.reflection.empty())
        {
            ReflectionView rv(e.reflection);
            if (!rv.valid())
            {
                std::printf("        reflection     : <invalid>\n");
                continue;
            }
            auto eps = rv.decodedEntryPoints();
            std::printf("        entry points (%zu):\n", eps.size());
            for (auto& ep : eps)
            {
                std::printf("          - %.*s  (stage=%u, threads=[%u,%u,%u], wave=%u)\n",
                            static_cast<int>(ep.name.size()), ep.name.data(), ep.stage, ep.threadGroupSize[0],
                            ep.threadGroupSize[1], ep.threadGroupSize[2], ep.waveSize);
                for (auto& p : ep.parameters)
                {
                    std::printf("              param %.*s: category=%u, set=%u, binding=%u, offset=%u, size=%u\n",
                                static_cast<int>(p.name.size()), p.name.data(), p.category, p.space, p.binding,
                                p.byteOffset, p.byteSize);
                }
                for (auto& [attrName, attrArgs] : ep.attributes)
                {
                    std::printf("              attribute %.*s (%zu args)\n", static_cast<int>(attrName.size()),
                                attrName.data(), attrArgs.size());
                }
            }
            auto globals = rv.decodedGlobalParameters();
            if (!globals.empty())
            {
                std::printf("        global params (%zu):\n", globals.size());
                for (auto& p : globals)
                {
                    std::printf("          - %.*s: category=%u, set=%u, binding=%u, offset=%u, size=%u\n",
                                static_cast<int>(p.name.size()), p.name.data(), p.category, p.space, p.binding,
                                p.byteOffset, p.byteSize);
                }
            }
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"slangmake — compile Slang shader permutations into a single .bin blob"};
    app.set_version_flag("--version", SLANGMAKE_VERSION_STR);

    std::string dumpPath;
    bool        dumpReflection = false;
    app.add_option("--dump", dumpPath, "Print info about an existing .bin blob and exit");
    app.add_flag("--dump-reflection", dumpReflection, "Also decode and print reflection contents when --dump is used");

    // Inputs (not CLI-required so --dump can be used standalone; checked manually below)
    std::string inputPath;
    std::string outputPath;
    std::string targetStr;
    std::string profileStr;

    app.add_option("-i,--input", inputPath, "Input .slang file or directory");
    app.add_option("-o,--output", outputPath, "Output .bin file (single input) or directory");
    app.add_option("-t,--target", targetStr, "Target: SPIRV|DXIL|DXBC|HLSL|GLSL|Metal|MetalLib|WGSL");
    app.add_option("-p,--profile", profileStr, "Slang profile (e.g. sm_6_5, glsl_450)");

    std::vector<std::string> entries;
    app.add_option("-e,--entry", entries, "Entry point name (default: every defined). Repeatable");

    std::vector<std::string> includeDirs;
    app.add_option("-I,--include", includeDirs, "Include search path. Repeatable");

    std::vector<std::string> defineStrs;
    app.add_option("-D,--define", defineStrs, "NAME[=VAL] preprocessor define. Repeatable");

    std::vector<std::string> permStrs;
    app.add_option("-P,--permutation", permStrs,
                   "NAME={a,b,c} macro permutation override (CLI wins over file directives). Repeatable");

    std::vector<std::string> permTypeStrs;
    app.add_option("--permutation-type", permTypeStrs,
                   "NAME={T1,T2} generic type-parameter permutation override. Repeatable");

    int optLevel = 3;
    app.add_option("-O,--optimization", optLevel, "Optimization level 0..3")->check(CLI::Range(0, 3));

    bool debugInfo = false;
    app.add_flag("-g,--debug", debugInfo, "Emit debug info");

    bool warningsAsErrors = false;
    app.add_flag("-W,--warnings-as-errors", warningsAsErrors, "Treat warnings as errors");

    std::string matrixLayoutStr = "row";
    app.add_option("--matrix-layout", matrixLayoutStr, "Matrix layout: row|column")
        ->check(CLI::IsMember({"row", "column"}));

    std::string fpModeStr = "default";
    app.add_option("--fp-mode", fpModeStr, "Floating point mode: default|fast|precise")
        ->check(CLI::IsMember({"default", "fast", "precise"}));

    int vulkanVersion = 0;
    app.add_option("--vulkan-version", vulkanVersion, "Target Vulkan version e.g. 12 for 1.2");

    std::vector<std::string> bindShifts;
    app.add_option("--vulkan-bind-shift", bindShifts, "kind:space:shift, e.g. t:0:16. Repeatable");

    bool glslScalarLayout = false;
    app.add_flag("--glsl-scalar-layout", glslScalarLayout, "Force GLSL scalar buffer layout");

    bool emitSpirvViaGlsl = false;
    app.add_flag("--emit-spirv-via-glsl", emitSpirvViaGlsl, "Use GLSL backend for SPIRV (default: direct)");

    bool dumpIntermediates = false;
    app.add_flag("--dump-intermediates", dumpIntermediates, "Dump intermediate compilation outputs");

    std::string languageVersion;
    app.add_option("--language-version", languageVersion, "Slang language version override");

    bool noReflection = false;
    app.add_flag("--no-reflection", noReflection, "Skip reflection serialisation");

    bool keepGoing = false;
    app.add_flag("--keep-going", keepGoing, "Continue after a permutation fails");

    bool noIncremental = false;
    app.add_flag("--no-incremental", noIncremental, "Force a full rebuild; ignore any existing output blob");

    std::string compressionStr = "none";
    app.add_option("--compress", compressionStr, "Compress the blob payload: none|lz4|zstd")
        ->check(CLI::IsMember({"none", "lz4", "zstd"}));

    int jobs = 1;
    app.add_option("-j,--jobs", jobs, "Parallel permutation compilation worker count");

    bool quiet   = false;
    bool verbose = false;
    app.add_flag("-q,--quiet", quiet, "Suppress non-error output");
    app.add_flag("-v,--verbose", verbose, "Verbose output");

    CLI11_PARSE(app, argc, argv);

    if (!dumpPath.empty())
        return runDump(dumpPath, dumpReflection);

    if (inputPath.empty() || outputPath.empty() || targetStr.empty() || profileStr.empty())
    {
        std::fprintf(stderr, "error: -i, -o, -t, -p are required for compilation (use --dump to inspect a blob)\n");
        return 2;
    }

    // Translate to CompileOptions.
    auto target = parseTarget(targetStr);
    if (!target)
    {
        std::fprintf(stderr, "error: unknown target '%s'\n", targetStr.c_str());
        return 2;
    }

    CompileOptions base;
    base.target  = *target;
    base.profile = profileStr;
    if (!entries.empty())
        base.entryPoint = entries.front();
    for (auto& s : includeDirs)
        base.includePaths.emplace_back(s);
    for (auto& s : defineStrs)
    {
        ShaderConstant c;
        if (!parseDefine(s, c))
        {
            std::fprintf(stderr, "error: invalid -D '%s'\n", s.c_str());
            return 2;
        }
        base.defines.push_back(std::move(c));
    }
    base.optimization     = static_cast<Optimization>(optLevel);
    base.debugInfo        = debugInfo;
    base.warningsAsErrors = warningsAsErrors;
    base.matrixLayout     = (matrixLayoutStr == "column") ? MatrixLayout::ColumnMajor : MatrixLayout::RowMajor;
    if (fpModeStr == "fast")
        base.fpMode = FloatingPointMode::Fast;
    if (fpModeStr == "precise")
        base.fpMode = FloatingPointMode::Precise;
    base.vulkanVersion = vulkanVersion;
    for (auto& s : bindShifts)
    {
        VulkanBindShift bs;
        if (!parseBindShift(s, bs))
        {
            std::fprintf(stderr, "error: invalid --vulkan-bind-shift '%s'\n", s.c_str());
            return 2;
        }
        base.vulkanBindShifts.push_back(bs);
    }
    base.glslScalarLayout  = glslScalarLayout;
    base.emitSpirvDirectly = !emitSpirvViaGlsl;
    base.dumpIntermediates = dumpIntermediates;
    base.languageVersion   = languageVersion;
    base.emitReflection    = !noReflection;

    std::vector<PermutationDefine> cliPerms;
    for (auto& s : permStrs)
    {
        PermutationDefine p;
        if (!parsePermutation(s, p, false))
        {
            std::fprintf(stderr, "error: invalid -P '%s', expected NAME={a,b,c}\n", s.c_str());
            return 2;
        }
        p.kind = PermutationDefine::Kind::Constant;
        cliPerms.push_back(std::move(p));
    }
    for (auto& s : permTypeStrs)
    {
        PermutationDefine p;
        if (!parsePermutation(s, p, true))
        {
            std::fprintf(stderr, "error: invalid --permutation-type '%s', expected NAME={T1,T2}\n", s.c_str());
            return 2;
        }
        p.kind = PermutationDefine::Kind::Type;
        cliPerms.push_back(std::move(p));
    }

    Compiler      compiler;
    BatchCompiler bc(compiler);
    bc.setKeepGoing(keepGoing);
    bc.setVerbose(verbose);
    bc.setQuiet(quiet);
    bc.setJobs(jobs);
    bc.setIncremental(!noIncremental);
    if (auto c = parseCodec(compressionStr))
        bc.setCompression(*c);

    fs::path inPath(inputPath);
    fs::path outPath(outputPath);

    int    exitCode     = 0;
    size_t totalShaders = 0, totalPerms = 0, totalFailures = 0;
    if (fs::is_directory(inPath))
    {
        if (fs::exists(outPath) && !fs::is_directory(outPath))
        {
            std::fprintf(stderr, "error: --output must be a directory when --input is a directory\n");
            return 2;
        }
        auto outputs = bc.compileDirectory(inPath, base, outPath, cliPerms);
        for (const auto& o : outputs)
        {
            ++totalShaders;
            totalPerms += o.compiled.size();
            totalFailures += o.failures.size();
            if (!quiet)
            {
                std::fprintf(stdout, "%s: %zu permutation(s), %zu failure(s)\n", o.outputFile.string().c_str(),
                             o.compiled.size(), o.failures.size());
            }
            for (const auto& f : o.failures)
                std::fprintf(stderr, "  %s\n", f.c_str());
            if (!o.failures.empty())
                exitCode = 1;
        }
    }
    else
    {
        BatchCompiler::Input in;
        in.file        = inPath;
        in.options     = base;
        in.cliOverride = cliPerms;
        auto out       = bc.compileFile(in, outPath);
        ++totalShaders;
        totalPerms    = out.compiled.size();
        totalFailures = out.failures.size();
        if (!quiet)
        {
            std::fprintf(stdout, "%s: %zu permutation(s), %zu failure(s)\n", out.outputFile.string().c_str(),
                         out.compiled.size(), out.failures.size());
        }
        for (const auto& f : out.failures)
            std::fprintf(stderr, "  %s\n", f.c_str());
        if (!out.failures.empty())
            exitCode = 1;
    }

    if (!quiet)
    {
        std::fprintf(stdout, "summary: %zu shader(s), %zu permutation(s), %zu failure(s)\n", totalShaders, totalPerms,
                     totalFailures);
    }
    return exitCode;
}

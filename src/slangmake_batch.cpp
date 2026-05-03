#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "slangmake_runtime_internal.h"

namespace slangmake
{

namespace
{

std::vector<std::string> findDuplicateAxisNames(std::span<const PermutationDefine> defs)
{
    std::unordered_map<std::string, uint32_t> counts;
    counts.reserve(defs.size());
    for (const auto& def : defs)
        ++counts[def.name];

    std::vector<std::string> duplicates;
    duplicates.reserve(counts.size());
    for (const auto& [name, count] : counts)
        if (count > 1)
            duplicates.push_back(name);
    std::ranges::sort(duplicates);
    return duplicates;
}

} // namespace

BatchCompiler::BatchCompiler(Compiler& compiler)
    : m_compiler(compiler)
{
}

BatchCompiler::Output BatchCompiler::compileFile(const Input& in, const std::filesystem::path& outPath)
{
    Output out;
    out.outputFile = outPath;
    m_stats        = {};

    auto fileDefs = PermutationParser::parseFile(in.file);
    auto merged   = mergePermutationDefines(fileDefs, in.cliOverride);
    if (auto duplicateAxes = findDuplicateAxisNames(merged); !duplicateAxes.empty())
    {
        std::string msg = "duplicate permutation axis name(s): ";
        for (size_t i = 0; i < duplicateAxes.size(); ++i)
        {
            if (i)
                msg += ", ";
            msg += "'" + duplicateAxes[i] + "'";
        }
        msg += ". Each permutation axis name must be unique after CLI overrides are merged.";
        out.failures.push_back(std::move(msg));
        return out;
    }
    auto perms = PermutationExpander::expand(merged);

    CompileOptions opts = in.options;
    opts.inputFile      = in.file;
    // Per-file "// [noreflection]" opt-out; AND-ed with the global setting
    // so the CLI --no-reflection can only strengthen, not loosen, the choice.
    if (fileHasNoReflection(in.file))
        opts.emitReflection = false;

    const uint64_t optionsHash = detail::hashCompileOptions(opts);

    // Load the previous blob (if any). Reuse is decided per-entry below: each
    // entry's recorded dependency files must still hash identically. Entries
    // whose deps changed are recompiled; unrelated entries are still reused.
    std::unordered_map<std::string, size_t> reuseKeyToIdx;
    std::vector<uint64_t>                   prevDepContentHashes; // cache, indexed by DepEntry idx
    std::optional<BlobReader>               prevReader;
    if (m_incremental && std::filesystem::exists(outPath))
    {
        prevReader = BlobReader::openFile(outPath);
        if (prevReader && prevReader->target() == in.options.target && prevReader->optionsHash() == optionsHash)
        {
            auto allDeps = prevReader->dependencies();
            prevDepContentHashes.resize(allDeps.size());
            for (size_t i = 0; i < allDeps.size(); ++i)
                prevDepContentHashes[i] = allDeps[i].contentHash;

            for (size_t i = 0; i < prevReader->entryCount(); ++i)
            {
                auto e = prevReader->at(i);
                reuseKeyToIdx.emplace(std::string(e.key), i);
            }
        }
        else
        {
            prevReader.reset();
        }
    }

    // Hash each previous dep file at most once per compileFile() — expensive
    // enough that we don't want to redo it for every entry that references it.
    std::vector<std::optional<uint64_t>> prevDepCurrentHashes;
    if (prevReader)
        prevDepCurrentHashes.resize(prevDepContentHashes.size());

    auto currentHashOfPrevDep = [&](uint32_t idx) -> uint64_t
    {
        if (idx >= prevDepCurrentHashes.size())
            return 0;
        auto& slot = prevDepCurrentHashes[idx];
        if (slot.has_value())
            return *slot;
        auto                  deps = prevReader->dependencies();
        std::filesystem::path p(deps[idx].path);
        uint64_t              h = std::filesystem::exists(p) ? detail::hashFileContents(p) : 0;
        slot                    = h;
        return h;
    };

    BlobWriter writer(in.options.target);
    writer.setOptionsHash(optionsHash);
    writer.setCompression(m_compression);

    std::vector<Compiler::Result> results(perms.size());
    std::vector<bool>             reused(perms.size(), false);

    auto logPerm = [&](const Permutation& perm, const char* tag)
    {
        if (!m_verbose)
            return;
        std::string                 msg = std::string(tag) + ": " + in.file.string() + " [" + perm.key() + "]\n";
        static std::mutex           logMutex;
        std::lock_guard<std::mutex> lk(logMutex);
        // Progress messages go to stdout; only real errors and slang
        // diagnostics are routed to stderr elsewhere.
        std::fwrite(msg.data(), 1, msg.size(), stdout);
    };

    // Per-entry deps to carry through to the writer (indices into the final
    // dep list; resolved to global-list indices after we pick the final deps).
    std::vector<std::vector<std::string>> entryDepPaths(perms.size());

    // Mark reusable entries up-front; only non-reused indices go to compile.
    std::vector<int> toCompile;
    toCompile.reserve(perms.size());
    for (size_t i = 0; i < perms.size(); ++i)
    {
        if (prevReader)
        {
            auto it = reuseKeyToIdx.find(perms[i].key());
            if (it != reuseKeyToIdx.end())
            {
                auto e      = prevReader->at(it->second);
                bool depsOk = true;
                for (uint32_t depIdx : e.depIndices)
                {
                    if (depIdx >= prevDepContentHashes.size() ||
                        currentHashOfPrevDep(depIdx) != prevDepContentHashes[depIdx])
                    {
                        depsOk = false;
                        break;
                    }
                }
                if (depsOk)
                {
                    Compiler::Result r;
                    r.success = true;
                    r.code.assign(e.code.begin(), e.code.end());
                    r.reflection.assign(e.reflection.begin(), e.reflection.end());
                    // Preserve this entry's dep paths from the previous blob.
                    auto allDeps = prevReader->dependencies();
                    entryDepPaths[i].reserve(e.depIndices.size());
                    for (uint32_t depIdx : e.depIndices)
                        if (depIdx < allDeps.size())
                            entryDepPaths[i].emplace_back(allDeps[depIdx].path);
                    results[i] = std::move(r);
                    reused[i]  = true;
                    logPerm(perms[i], "reusing");
                    continue;
                }
            }
        }
        toCompile.push_back(static_cast<int>(i));
    }

    const int nCompile = static_cast<int>(toCompile.size());
    const int nThreads = std::max(1, std::min(m_jobs, nCompile));

    if (nThreads <= 1)
    {
        for (int i : toCompile)
        {
            logPerm(perms[i], "compiling");
            results[i] = m_compiler.compile(opts, perms[i]);
        }
    }
    else
    {
        std::atomic<int> nextIdx{0};
        auto             worker = [&]()
        {
            // Each thread owns its own Compiler (and IGlobalSession) because
            // Slang's front-end — loadModule / link — is not documented as
            // thread-safe when a session is shared across workers.
            Compiler localCompiler;
            while (true)
            {
                int k = nextIdx.fetch_add(1, std::memory_order_relaxed);
                if (k >= nCompile)
                    break;
                int i = toCompile[static_cast<size_t>(k)];
                logPerm(perms[i], "compiling");
                results[i] = localCompiler.compile(opts, perms[i]);
            }
        };

        std::vector<std::thread> threads;
        threads.reserve(nThreads);
        for (int t = 0; t < nThreads; ++t)
            threads.emplace_back(worker);
        for (auto& t : threads)
            t.join();
    }

    // Capture fresh compiles' dep lists too.
    for (size_t i = 0; i < perms.size(); ++i)
        if (!reused[i] && results[i].success)
            entryDepPaths[i] = results[i].dependencies;

    // Build the final blob-level dep list from the union of every surviving
    // entry's paths, plus a path→index map so each entry can store its own
    // u32 indices.
    std::unordered_map<std::string, uint32_t> pathToIdx;
    std::vector<BlobWriter::DepInfo>          depVec;
    auto                                      internDep = [&](const std::string& p) -> uint32_t
    {
        auto it = pathToIdx.find(p);
        if (it != pathToIdx.end())
            return it->second;
        auto idx = static_cast<uint32_t>(depVec.size());
        depVec.push_back({p, detail::hashFileContents(p)});
        pathToIdx.emplace(p, idx);
        return idx;
    };

    // Hand out indices first so we preserve insertion order; sort afterwards
    // would invalidate them.
    std::vector<std::vector<uint32_t>> entryDepIndices(perms.size());
    for (size_t i = 0; i < perms.size(); ++i)
    {
        if (!results[i].success)
            continue;
        entryDepIndices[i].reserve(entryDepPaths[i].size());
        for (const auto& p : entryDepPaths[i])
            entryDepIndices[i].push_back(internDep(p));
    }

    for (size_t i = 0; i < perms.size(); ++i)
    {
        auto& r = results[i];
        if (!r.success)
        {
            out.failures.push_back(perms[i].key() + ": " + r.diagnostics);
            if (!m_keepGoing)
            {
                out.blob = writer.finalize();
                return out;
            }
            continue;
        }
        if (!m_quiet && !reused[i] && !r.diagnostics.empty())
            std::fwrite(r.diagnostics.c_str(), 1, r.diagnostics.size(), stderr);
        writer.addEntry(perms[i], r.code, r.reflection, entryDepIndices[i]);
        out.compiled.push_back(perms[i]);
        if (reused[i])
            ++m_stats.reused;
        else
            ++m_stats.compiled;
    }

    writer.setDependencies(std::move(depVec));

    out.blob = writer.finalize();
    writer.writeToFile(outPath);
    return out;
}

std::vector<BatchCompiler::Output> BatchCompiler::compileDirectory(const std::filesystem::path&          root,
                                                                   const CompileOptions&                 base,
                                                                   const std::filesystem::path&          outDir,
                                                                   const std::vector<PermutationDefine>& cliOverride)
{
    std::vector<Output> outputs;
    std::filesystem::create_directories(outDir);
    for (auto& entry : std::filesystem::recursive_directory_iterator(root))
    {
        if (!entry.is_regular_file())
            continue;
        if (entry.path().extension() != ".slang")
            continue;

        Input in;
        in.file        = entry.path();
        in.options     = base;
        in.cliOverride = cliOverride;

        auto rel = std::filesystem::relative(entry.path(), root);
        rel.replace_extension(".bin");
        auto outPath = outDir / rel;
        outputs.push_back(compileFile(in, outPath));
    }
    return outputs;
}

} // namespace slangmake

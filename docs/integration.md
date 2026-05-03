# Integration

How to wire a slangmake blob into a shipping engine: loading, lookup, lifetime, pipeline-cache story, threading, memory. Companion to [permutations.md](permutations.md) (which covers *how* to pick the right entry) and [reflection.md](reflection.md) (which covers the raw reflection tables your bind path consumes).

## Shipping surface

A slangmake-based runtime pipeline needs shader blobs plus the small runtime
reader library:

| What | Where | Typical size |
| --- | --- | --- |
| `*.bin` blobs | Asset directory | dominated by bytecode (SPIR-V etc.); reflection is a few KB per entry, deduplicated |
| — | — | — |
| `slangmake-rt` | Linked into your engine (`.lib` static or `.dll` dynamic) | small; blob reader, reflection view, LZ4/zstd only |
| Nothing from Slang/DXC | — | No `slang.dll`, `slang-compiler.dll`, DXC DLLs, `.slang` source, or runtime JSON |

Compared with the "ship Slang and compile at runtime" approach, you save ~38 MB of DLLs (`slang-compiler.dll` alone is ~32 MB on Windows) and several tens to hundreds of milliseconds of cold-start time — see the summary at the bottom of this doc.

## Loading

Three flavours depending on who owns the buffer:

```cpp
// 1. Own the buffer — let slangmake mmap or read it, then hand ownership.
auto reader = BlobReader::openFile("shaders/tonemap.bin");
if (!reader || !reader->valid()) { /* I/O or parse error */ }

// 2. Caller provides the bytes — non-owning view.
BlobReader reader(std::span<const uint8_t>{myMappedBytes, mySize});

// 3. Caller provides an rvalue buffer — reader takes ownership.
std::vector<uint8_t> bytes = loadMyAsset("shaders/tonemap.bin");
BlobReader reader(std::move(bytes));
```

In engines with an asset system already doing mmap, wrap the mapped region with overload (2) and keep the mapping alive for as long as the reader exists. In simpler engines, `openFile` is enough — it reads the file once and holds the bytes internally.

If `compression` in the blob header is not `None`, the reader decompresses into an owned buffer once at construction; every subsequent `Entry` access is zero-copy into that buffer.

## Lookup and iteration

```cpp
// Single variant.
Permutation q;
q.constants = {{"USE_TAA", "1"}, {"QUALITY", "high"}};
q.typeArgs  = {{"MAT", "Wood"}};
auto entry = reader->find(q);
if (!entry) { /* permutation not compiled */ }

// entry->code        — bytecode for vkCreateShaderModule / D3D12 PSO
// entry->reflection  — feed into ReflectionView
// entry->depIndices  — source files this permutation actually depends on
// entry->key         — canonical key string, same format as Permutation::key()

// Iterate every variant (e.g. to warm a pipeline cache).
for (size_t i = 0; i < reader->entryCount(); ++i) {
    auto e = reader->at(i);
    buildPipelineFor(e.key, e.code);
}

// Or just the keys, to drive a batch preload.
for (const auto& key : reader->enumerate()) { /* ... */ }
```

## Lifetime — hold the blob

**Do not** drop the `BlobReader` or its underlying buffer after you've built your pipelines. You'll want the bytecode again on:

- **Device lost.** GPU crashes, driver updates, Windows `TDR` timeouts, laptop GPU switching — all invalidate existing `VkPipeline` / PSO objects. You rebuild them by calling `vkCreatePipeline` again, which needs the SPIR-V. If you've freed it, you can only hard-restart.
- **Pipeline recreation for validation layers / RenderDoc.** Some capture tools reflectively rebuild pipelines to re-record state.
- **Lazy pipeline creation.** Many engines create pipelines on first use, not at startup. If the blob isn't live, the first-touch materialisation path breaks.

The cheap idiom is: `mmap` the blob once, leave it mapped for the lifetime of the device. Physical memory follows demand-paging — warm pages stay resident, cold ones drift back to disk. Measured cost is close to zero.

## Relation to the graphics API's pipeline cache

`VkPipelineCache` / `ID3D12PipelineLibrary` accelerates *repeat* `vkCreatePipeline` calls by caching the driver's compile from SPIR-V/DXIL to GPU-native instructions. It does **not** replace the source bytecode — on cache miss (first launch, new driver, new hardware) you still need the SPIR-V/DXIL in hand.

The two caches are complementary:

- slangmake's blob holds the front-end work: source → target bytecode, plus reflection.
- The graphics API's pipeline cache holds the back-end work: target bytecode → GPU instructions.

Typical shipping flow: slangmake blob ships with the game, `VkPipelineCache` persists across runs in `%APPDATA%`. On a fresh install with a new driver, both come into play — the blob feeds bytecode to `vkCreatePipeline`, the cache miss triggers the driver to do GPU codegen once, and subsequent launches hit the persistent cache.

## Threading

`BlobReader` and `ReflectionView` are read-only after construction. Every public method is a `const` method that touches only `const`-reachable state, so:

- Multiple threads may call `find`, `at`, `enumerate`, `dependencies`, `optionsHash`, etc. on the same reader concurrently without synchronisation.
- `ReflectionView` shares this guarantee — multiple threads may each build their own `ReflectionView` over the same reflection bytes, or share one.

Writers (`BlobWriter`, `BatchCompiler`) are not thread-safe — but you should only be using those at build time, not in the shipping runtime.

## Size + startup budget

Concrete comparison on a typical mid-size renderer (~300 shader variants, SPIR-V target):

|                             | slangmake blob | slang session |
| --- | --- | --- |
| Shipped DLLs                | 0              | `slang.dll` (155 KB) + `slang-compiler.dll` (~32 MB) + `slang-rt.dll` (1.4 MB) + stdlib module dir (~5 MB) ≈ **38 MB** |
| Shipped source              | 0              | `.slang` files or `.slang-module` precompiled modules — several hundred KB to a few MB |
| Cold load of one shader     | `mmap` + `find` → sub-microsecond | `session->loadModuleFromSource` + parse + typecheck → 10–200 ms per module |
| Runtime specialisation      | Not applicable (baked at compile time via permutations) | `composite->specialize()` → seconds on complex programs |
| Device-lost rebuild         | `vkCreatePipeline` from blob bytecode → same cost as first build | Same, but requires the session + the source to still be loadable |

The slang-session path is right when you genuinely need runtime specialisation (editor, shader hot-reload, user-authored shaders). Most shipping builds of most games don't — hence slangmake.

## Handling missing variants

`reader->find(perm)` returns `std::nullopt` when the blob has no entry matching that key. Common causes and mitigations:

1. **Asked for an axis combination that was never compiled.** Decide whether to fail hard (assert), fall back to a default permutation, or recompile that cell on the fly (rare — would require shipping Slang).
2. **Spelling mismatch.** CLI override used `USE_SHADOW={1}` but runtime looks up `{"UseShadow","1"}`. Use `reader->enumerate()` during engine boot in debug builds to cross-check.
3. **Type-axis value missing leading `|`.** The `Permutation::key()` helper handles the prefix — build your lookup `Permutation` with `typeArgs` populated, not by hand-constructing a key string.

## Entry-point discovery

A single blob entry may contain multiple entry points (vs / ps / cs in one module, ray-tracing hit groups, etc.). The reflection tells you which:

```cpp
ReflectionView rv(entry->reflection);
for (const auto& ep : rv.decodedEntryPoints()) {
    // ep.name, ep.stage (SLANG_STAGE_*), ep.threadGroupSize, ep.hash,
    // ep.parameters (pre-decoded Param list)
}
```

`ep.hash` is a stable 64-bit per-entry-point signature you can use as a cache key for API-level pipeline state objects.

## Picking a compression codec

| Setting | Best for |
| --- | --- |
| `Codec::None` | Dev builds (faster compile, trivial debugging) |
| `Codec::LZ4`  | Ship builds where load time matters more than size |
| `Codec::Zstd` | Ship builds where package size matters more than load time |

The reader decompresses once at open time into an owned buffer — so the choice is a startup-time vs on-disk-size trade. LZ4 decompresses at GB/s, Zstd is slower but compresses blobs noticeably smaller, often 2–3×.

## Hot-reload (dev only)

slangmake doesn't do hot-reload directly — the blob is a frozen artefact. Typical dev-build pattern:

1. **Editor build**: link the Slang runtime directly, use its live `ISession` + reflection APIs. Hot-reload re-parses source.
2. **Ship build**: link `slangmake-rt`, load the pre-built blob.

The bind path can share a common interface across both modes (both produce `(space, slot, offset, size)` tuples). The difference is the source of those numbers: live `ProgramLayout` at dev time, serialised `ReflectionView` at ship time.

## See also

- [permutations.md](permutations.md) for how to build the `Permutation` you pass to `find`.
- [reflection.md](reflection.md) for the raw reflection tables your RHI consumes.

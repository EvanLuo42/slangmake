# slangmake

Compiles [Slang](https://github.com/shader-slang/slang) shader permutations
into a single `.bin` blob that holds every variant's bytecode **and** full
reflection data. Think ShaderMake, but for Slang, with reflection, incremental
rebuilds, parallel compilation and optional LZ4 / zstd compression.

## Features

- **Permutation expansion** — declare variants inline in the `.slang` source
  with `// [permutation] NAME={a,b,c}` comments; slangmake takes the Cartesian
  product and emits one entry per cell. Generic `type_param` axes are also
  supported via `// [permutation type] NAME={T1,T2}` — each combination bakes
  one fully specialised variant at compile time.
- **Exhaustive reflection** — every `slang::ProgramLayout` table (types, type
  layouts, variables, varlayouts, functions, generics, decl tree, entry
  points, attributes, modifiers, descriptor sets, binding ranges, hashed
  strings, …) is serialised into a compact binary section inside the blob.
  Permutations whose reflection bytes turn out identical share a single
  payload in the file — no on-disk duplication.
- **Zero-copy reader** — `BlobReader` and `ReflectionView` expose every
  section as a `std::span` into an in-memory buffer; no JSON parse at load.
- **Incremental rebuilds** — second runs reuse existing entries when the
  options fingerprint and every dependency file's content hash still match.
- **Parallel compile** — `-j N` spawns `N` workers, each with its own Slang
  global session.
- **Optional compression** — LZ4 or zstd on the blob payload, transparently
  decompressed when the reader opens the file.
- **One target per blob** — SPIRV, DXIL, DXBC, HLSL, GLSL, Metal, MetalLib or
  WGSL, selected per invocation.

## Build

Requires CMake ≥ 4.2 and a C++20 compiler (tested on MSVC 19.44). Dependencies
(Slang, CLI11, doctest, LZ4, zstd) are fetched automatically via
`FetchContent` when you configure.

```bash
cmake -S . -B build
cmake --build build
```

By default this builds:

- `slangmake-lib` — the static library (object name `slangmake`).
- `slangmake` — the CLI executable.
- `slangmake_tests` — doctest suite (disable with `-DSLANG_MAKE_BUILD_TESTS=OFF`).

Build as a shared library with `-DSLANG_MAKE_BUILD_SHARED=ON`.

### Run the tests

```bash
ctest --test-dir build --output-on-failure
```

### Install

`cmake --install` stages a ready-to-ship tree: the CLI, the slang runtime
shared libraries, the static library, the public header, and the license.

```bash
cmake --install build --prefix ./install --config Release
```

Layout on Windows:

```
install/
  bin/     slangmake.exe, slang*.dll
  lib/     slangmake.lib
  include/ slangmake.h, slangmake.hpp
  share/slangmake/ LICENSE, README.md
```

## CLI usage

```bash
slangmake -i <input> -o <output> -t <target> -p <profile> [options]
```

`<input>` is either a single `.slang` file or a directory that will be walked
recursively for `*.slang`; `<output>` is a `.bin` file (single-input mode) or a
directory (directory-input mode, produces a parallel tree of `.bin`s).

### Required flags

| Flag | Meaning |
| --- | --- |
| `-i, --input <path>` | Single `.slang` file or directory. |
| `-o, --output <path>` | Output `.bin` file or directory. |
| `-t, --target <name>` | `SPIRV\|DXIL\|DXBC\|HLSL\|GLSL\|Metal\|MetalLib\|WGSL`. |
| `-p, --profile <name>` | Slang profile, e.g. `sm_6_5`, `glsl_450`. |

### Optional flags

| Flag | Default | Purpose |
| --- | --- | --- |
| `-e, --entry <name>` | all defined | Compile only this entry point. Repeatable. |
| `-I, --include <dir>` | — | Add include search path. Repeatable. |
| `-D, --define <N[=V]>` | — | Global preprocessor define. Repeatable. |
| `-P, --permutation <N={a,b}>` | — | CLI permutation override (beats file directives). Repeatable. |
| `--permutation-type <N={T1,T2}>` | — | CLI type-parameter permutation override. Repeatable. |
| `-O, --optimization 0..3` | `3` | Slang optimisation level. |
| `-g, --debug` | off | Emit debug info. |
| `-W, --warnings-as-errors` | off | Promote warnings. |
| `--matrix-layout row\|column` | `row` | Matrix memory layout. |
| `--fp-mode default\|fast\|precise` | `default` | Floating-point mode. |
| `--vulkan-version <maj.min>` | — | Vulkan version for SPIRV. |
| `--vulkan-bind-shift <k:s:shift>` | — | Register-shift, e.g. `t:0:16`. Repeatable. |
| `--glsl-scalar-layout` | off | Scalar buffer layout for Vulkan. |
| `--emit-spirv-via-glsl` | direct | Route SPIRV through GLSL backend. |
| `--dump-intermediates` | off | Dump intermediates. |
| `--language-version <ver>` | — | Slang language version. |
| `--no-reflection` | off | Skip reflection section. |
| `--keep-going` | off | Don't abort on first permutation failure. |
| `--no-incremental` | off | Force a full rebuild. |
| `--compress none\|lz4\|zstd` | `none` | Compress the blob payload. |
| `-j, --jobs <N>` | `1` | Parallel permutation workers. |
| `-q, --quiet` / `-v, --verbose` | off | Log level. |

### Inspection flags (standalone, don't require `-i/-o/-t/-p`)

| Flag | Default | Purpose |
| --- | --- | --- |
| `--dump <path>` | — | Print header, dep list, and per-entry sizes / dep indices for an existing `.bin`, then exit. |
| `--dump-reflection` | off | When combined with `--dump`, also decode each entry's reflection and print entry points, their parameters, bindings and user attributes. |

### Examples

```bash
# Single file → single blob
slangmake -i shaders/tonemap.slang -o build/tonemap.bin -t SPIRV -p sm_6_5

# Whole tree → mirror tree under out/
slangmake -i shaders/ -o build/shaders -t SPIRV -p sm_6_5 -j 8

# Force a specific permutation set from the command line
slangmake -i tonemap.slang -o tonemap.bin -t SPIRV -p sm_6_5 \
          -P USE_TAA={0,1} -P QUALITY={low,high}

# Parallel + zstd
slangmake -i shaders/ -o out/ -t DXIL -p sm_6_5 -j 16 --compress zstd

# Inspect an existing blob (header, dep list, per-entry sizes + dep indices)
slangmake --dump build/tonemap.bin

# Same, plus decoded reflection per entry (entry points, parameters, bindings)
slangmake --dump build/tonemap.bin --dump-reflection
```

## Declaring permutations inside a shader

```slang
// [permutation] USE_SHADOW={0,1}
// [permutation] QUALITY={0,1,2}

#ifndef USE_SHADOW
#define USE_SHADOW 0
#endif
#ifndef QUALITY
#define QUALITY 0
#endif

RWStructuredBuffer<uint> uOutput;

[shader("compute")]
[numthreads(8, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) { /* ... */ }
```

This produces 2 × 3 = 6 entries in the output blob, one per
`(USE_SHADOW, QUALITY)` pair. CLI `-P NAME={...}` overrides a file-level
directive entirely when names collide.

### Type-parameter permutations

Module-scope `type_param` declarations can be varied the same way:

```slang
// [permutation type] MAT={Metal,Wood}

interface IBrdf { float3 shade(); }
struct Metal : IBrdf { float4 tint; float3 shade() { return tint.rgb; } }
struct Wood  : IBrdf { Texture2D<float4> albedo; SamplerState samp; /* ... */ }

type_param MAT : IBrdf;
ParameterBlock<MAT> gMat;

[shader("compute")] [numthreads(8, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) { /* uses gMat.shade() */ }
```

Each combination is specialised at compile time into its own fully concrete
variant — different reflection, different bindings, different bytecode — and
keyed in the blob as `|MAT=Metal`, `|MAT=Wood`, etc. (A leading `|` marks
type-only axes so the key never collides with a constant axis of the same
name.) Look up at runtime with `reader->find(perm)` passing a `Permutation`
with `typeArgs` populated.

Runtime constructs that require a live Slang session — `IShaderObject::
setObject`, `IComponentType::specialize`, dynamic `T` selection — are **not**
supported. The whole point of this model is to bake the type axis at compile
time so the ship build needs neither `slang.dll` nor shader source.

### `[noreflection]`

`// [noreflection]` opts a shader file out of reflection serialisation
(bytecode is still emitted). Per-file equivalent of `--no-reflection` —
useful for large utility shaders whose reflection you don't need at runtime.

## Library usage

The CLI is a thin wrapper over the public API. C++ users link `slangmake-lib`
and `#include "slangmake.hpp"`. C / FFI consumers (Rust, Python, …) get a
matching opaque-handle ABI from `#include "slangmake.h"`.

### Compile once

```cpp
#include "slangmake.hpp"
using namespace slangmake;

Compiler       c;
CompileOptions opts;
opts.inputFile = "tonemap.slang";
opts.target    = Target::SPIRV;
opts.profile   = "sm_6_5";

auto r = c.compile(opts, Permutation{{{"USE_TAA", "1"}}});
if (r.success) {
    // r.code       — SPIR-V bytes
    // r.reflection — packed reflection section
} else {
    std::fprintf(stderr, "%s\n", r.diagnostics.c_str());
}
```

### Batch → blob

```cpp
Compiler      c;
BatchCompiler bc(c);
bc.setJobs(std::thread::hardware_concurrency());
bc.setCompression(Codec::Zstd);

BatchCompiler::Input in;
in.file            = "shaders/tonemap.slang";
in.options.target  = Target::SPIRV;
in.options.profile = "sm_6_5";

auto out = bc.compileFile(in, "build/tonemap.bin");
std::printf("%zu compiled, %zu reused, %zu failed\n",
            bc.lastStats().compiled, bc.lastStats().reused, out.failures.size());
```

### Read a blob at runtime

```cpp
auto reader = BlobReader::openFile("build/tonemap.bin");
if (!reader || !reader->valid()) return;

std::array<ShaderConstant, 1> q{ShaderConstant{"USE_TAA", "1"}};
if (auto entry = reader->find(q)) {
    // entry->code — SPIR-V bytes to feed into vkCreateShaderModule / D3D PSO
    ReflectionView rv(entry->reflection);
    for (auto& ep : rv.decodedEntryPoints()) {
        // ep.name, ep.stage, ep.threadGroupSize, ep.parameters, ...
    }
}
```

`ReflectionView` also exposes every raw table (`types()`, `typeLayouts()`,
`variables()`, `varLayouts()`, `entryPoints()`, `bindingRanges()`,
`descriptorSets()`, `descriptorRanges()`, `decls()`, …) as `std::span`s into
the decoded buffer, so you can walk the full reflection graph without
allocating.

### Where binding-table walking lives

slangmake stops at the raw reflection tables. Computing `(set, slot,
byte-offset)` tuples for `vkUpdateDescriptorSets` / D3D12 root-parameter
hookup is the engine RHI's job — feed it `ReflectionView::variables() /
varLayouts() / bindingRanges() / descriptorSets() / descriptorRanges()` and
walk them in whatever way matches your bind path. See
[docs/reflection.md](docs/reflection.md) for the table cross-reference.

## Blob format (v1)

Little-endian throughout. Multi-byte integer fields are 4-byte aligned; code
and reflection payload chunks are 8-byte aligned; the per-entry dep-index
pool is 4-byte aligned.

### Top-level layout

```
+--------------------------------------------+-------------------+
| BlobHeader                                 |        56 B       |
+--------------------------------------------+-------------------+
| EntryRecord[entryCount]                    |  32 B per entry   |
+--------------------------------------------+-------------------+
| padding to 8                               |         ...       |
+--------------------------------------------+-------------------+
| entry 0 key '\0' + pad8                    |                   |
| entry 0 code           + pad8              |                   |
| entry 0 reflection     + pad8              |                   |
| entry 1 key '\0' + pad8                    |        ...        |
|   ...                                      |                   |
+--------------------------------------------+-------------------+
| deps string pool   (concat'd UTF-8 paths)  |     variable      |
+--------------------------------------------+-------------------+
| padding to 8                               |                   |
+--------------------------------------------+-------------------+
| DepEntry[depsCount]                        |  16 B per dep     |
+--------------------------------------------+-------------------+
| padding to 4                               |                   |
+--------------------------------------------+-------------------+
| entryDepsIdx u32[]   (flat pool of indices |                   |
|   into DepEntry[], referenced per entry)   |  4 B per index    |
+--------------------------------------------+-------------------+
```

If `BlobHeader.compression != 0`, every byte past the `BlobHeader` is replaced
by the codec-compressed form of the above. `BlobReader` decompresses into an
owned buffer once at open time so all offsets below stay valid.

### BlobHeader — 56 B

```
  offset  size  field
  ------  ----  --------------------------------------------
    0      4    magic                  = 'SLNG' (0x474E4C53)
    4      4    version                = 1
    8      4    target                 (SlangCompileTarget)
   12      4    entryCount
   16      4    depsOffset             -> DepEntry[]
   20      4    depsCount
   24      4    depsStringsOffset      -> UTF-8 path bytes
   28      4    depsStringsSize
   32      4    entryDepsIdxOffset     -> u32[]
   36      4    entryDepsIdxCount
   40      4    compression            0=None, 1=LZ4, 2=Zstd
   44      4    uncompressedPayloadSize
   48      8    optionsHash            (FNV-1a 64)
  ------
   56  total
```

### EntryRecord — 32 B

```
  offset  size  field
  ------  ----  --------------------------------------------
    0      4    keyOffset              -> key bytes
    4      4    keySize                (includes trailing '\0')
    8      4    codeOffset             -> target bytecode
   12      4    codeSize
   16      4    reflOffset             -> reflection section ('SLRF')
   20      4    reflSize               (0 if --no-reflection)
   24      4    depsIdxOff             -> entryDepsIdx[]
   28      4    depsIdxCount           (0 = unknown / no granular reuse)
  ------
   32  total
```

`depsIdxOff/Count` index into the blob-level `entryDepsIdx u32[]` pool.
Each u32 in that pool is itself an index into `DepEntry[]`, identifying a
dependency file that **this specific permutation** actually read. Per-entry
reuse during incremental rebuilds compares exactly this subset, so editing an
`.hlsli` that only one permutation includes invalidates just that permutation.

### DepEntry — 16 B

```
  offset  size  field
  ------  ----  --------------------------------------------
    0      4    pathOffset             -> deps string pool
    4      4    pathSize               (no NUL)
    8      8    contentHash            (FNV-1a 64 of file bytes)
  ------
   16  total
```

### Reflection section (referenced by each `EntryRecord.reflOffset`)

```
+--------------------------------------------+-------------------+
| ReflHeader                                 |        96 B       |
+--------------------------------------------+-------------------+
| strings pool   (NUL-separated UTF-8)       |     variable      |
+--------------------------------------------+-------------------+
| HashedStr[]  ... (14 fixed-stride tables)  |     variable      |
+--------------------------------------------+-------------------+
| u32Pool (shared list indices / children)   |     variable      |
+--------------------------------------------+-------------------+
```

The header starts with magic `'SLRF' (0x46524C53)` / version `1` and names
fixed-stride tables:

- `Type[]`, `TypeLayout[]`, `Variable[]`, `VarLayout[]`, `Function[]`,
  `Generic[]`, `Decl[]`, `EntryPoint[]`,
- `Attribute[]`, `AttrArg[]`, `HashedStr[]`,
- `BindingRange[]`, `DescriptorSet[]`, `DescriptorRange[]`, `SubObjectRange[]`,
- `modifierPool u32[]`, `u32Pool` (for arbitrary index lists).

Every cross-reference inside the reflection is a `u32` index into one of
these tables, so `ReflectionView` is pure zero-copy on top of the bytes.

### Invariants

- Permutation keys are joined as `NAME=VALUE_NAME=VALUE` with names sorted
  alphabetically — the same shader-constant set always produces the same key.
  Permutations that include `type_param` axes append `|NAME=VALUE...` after
  the constants section (leading `|` when there are no constants), so a key
  like `|MAT=Metal` is unambiguously all-type-args and never collides with a
  constant axis of the same name.
- `optionsHash` is an FNV-1a 64 over every compile option that is stable
  across permutations (target, profile, defines, optimisation, matrix
  layout, …). A mismatch invalidates the whole cache.
- `DepEntry.contentHash` is FNV-1a 64 over the raw file bytes. A mismatch
  invalidates any entry whose `entryDepsIdx` list references that dep.

## Project layout

```
cmake/                 FetchContent wrappers for slang/CLI11/doctest/lz4/zstd
include/
  slangmake.hpp        Public C++ API
  slangmake.h          Pure C ABI wrapper for FFI consumers (Rust, Python, …)
src/
  main.cpp             CLI
  slangmake_internal.h Private helpers shared across .cpps
  slangmake_util.cpp   Enum/parser/expander/hashing helpers
  slangmake_blob.cpp   BlobWriter / BlobReader / compression glue
  slangmake_reflection.cpp  ReflectionSerializer + ReflectionView
  slangmake_compiler.cpp    Compiler (owns IGlobalSession)
  slangmake_batch.cpp       BatchCompiler (parse → expand → compile → pack)
  slangmake_c.cpp           C ABI wrapper (matches include/slangmake.h)
tests/                 doctest suite + .slang / .hlsli fixtures
```


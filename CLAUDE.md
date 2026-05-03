# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & test

Configuration is driven by `CMakePresets.json`. All dependencies (Slang, CLI11, doctest, LZ4, zstd) are fetched via `FetchContent` at configure time — first configure is slow.

```bash
# Windows (primary dev target)
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug                      # all tests
ctest --preset windows-debug -R '<name>'          # filter at ctest level (single test target = slangmake_tests)

# Linux ASan/UBSan — mirrors the CI sanitizers job
cmake --preset linux-asan && cmake --build --preset linux-asan && ctest --preset linux-asan
```

All unit tests live in one doctest binary (`slangmake_tests`). To filter *inside* the binary:

```bash
build/windows-debug/bin/slangmake_tests --test-case='*reflection*'  # substring match on TEST_CASE names
build/windows-debug/bin/slangmake_tests --list-test-cases
```

The test binary depends on the CLI target — `tests/test_cli.cpp` shells out to `$<TARGET_FILE:slangmake>` via `std::system`, so changing CLI behaviour requires rebuilding both. Test fixtures live under `tests/shaders/`; the shader dir and CLI path are injected as `SLANG_MAKE_TESTS_SHADER_DIR` / `SLANG_MAKE_TESTS_CLI_EXE` defines (see `tests/CMakeLists.txt`).

Formatting: clang-format 18 (Allman, 4-space indent, 120 col). CI (`.github/workflows/format.yml`) gates on this — run `clang-format` before committing C++ changes.

Project version is centralised in the top-level `project(slangmake VERSION ...)` and flows into code via the generated `slangmake_version.h` (template in `cmake/slangmake_version.h.in`). Don't hand-edit version strings in `.cpp`.

## Architecture

slangmake is a one-target-per-blob Slang permutation compiler. The full library (`slangmake-lib`, output name `slangmake`) owns compiler / batch / C ABI code and links Slang + DXC; the runtime library (`slangmake-rt-lib`, output name `slangmake-rt`) is the C++ blob / reflection reader surface and must stay free of Slang / DXC dependencies. The CLI (`src/main.cpp`) is a thin CLI11 wrapper over the full library. The pipeline is:

```
parse directives → expand Cartesian product → compile each permutation → serialize reflection → pack into blob
```

Each stage maps to one `.cpp`:

- **`src/slangmake_util.cpp`** — enum ↔ string, `PermutationParser` (scans `// [permutation]` / `// [permutation type]` / `// [noreflection]` magic comments), `PermutationExpander` (Cartesian product), FNV-1a hashing helpers (`hashCompileOptions`, `hashFileContents`), value-list splitter respecting nested `()[]{}<>` and quotes.
- **`src/slangmake_compiler.cpp`** — `Compiler` owns one `slang::IGlobalSession`, applies `CompileOptions` + a `Permutation` (preprocessor defines **and** `IComponentType::specialize` type args), returns bytecode + serialized reflection + dep-file list.
- **`src/slangmake_reflection.cpp`** — walks Slang's `ProgramLayout` and serialises every table (Types, TypeLayouts, Variables, VarLayouts, Functions, Generics, Decls, EntryPoints, Attributes, HashedStrings, BindingRanges, DescriptorSets/Ranges, SubObjectRanges) into a fixed-stride binary section (`SLRF` magic). Every cross-ref is a `u32` index; strings are pool-offsets.
- **`src/slangmake_reflection_view.cpp`** — `ReflectionView`, the zero-copy reader over the serialised reflection section. This file is part of `slangmake-rt-lib`; do not include Slang headers here.
- **`src/slangmake_blob.cpp`** — `BlobWriter` packs entries into the `SLNG` blob (header → entry records → per-entry code/reflection → deps pool → per-entry dep-index pool), with FNV-1a dedup of both bytecode and reflection payloads so two permutations that compile to identical bytes share one on-disk copy. `BlobReader` is zero-copy over an in-memory buffer (owned or borrowed); LZ4/zstd decompression happens once at open.
- **`src/slangmake_batch.cpp`** — `BatchCompiler` orchestrates the pipeline: merges CLI permutation overrides on top of file directives, runs `-j N` workers each with their own `Compiler`, drives per-entry incremental reuse by comparing `optionsHash` + per-entry `contentHash`-of-deps against an existing blob.

### On-disk layout

The blob format (v1, little-endian, `SLNG` magic) is spec'd in `README.md` → "Blob format (v1)" and mirrored in `include/slangmake.hpp` under `namespace fmt` (with a C-side mirror of the same structs as `sm_fmt_*_t` in `include/slangmake.h`). Key invariants:

- **Permutation keys** are `NAME=VALUE_NAME=VALUE` sorted alphabetically, with type-axis args appended after `|` (e.g. `A=0|MAT=Metal`). A leading `|` marks all-type-axis keys so they never collide with a constant axis of the same name.
- **`optionsHash`** (FNV-1a 64) covers every `CompileOptions` field that is stable across permutations. Mismatch invalidates the whole cache.
- **Per-entry incremental reuse** compares the subset of deps each entry actually read (via `EntryRecord::depsIdxOff/Count` → `entryDepsIdx u32[]` → `DepEntry[]`). Editing an `.hlsli` only invalidates permutations that included it.
- **Reflection** is its own section (`SLRF` magic) embedded inside the blob at `EntryRecord::reflOffset`. Reflection-identical permutations already share a payload; bytecode-identical permutations now share one too.

### Permutation model

Two axis kinds, same syntax:

- `// [permutation] NAME={a,b,c}` — preprocessor constants; values go through Slang's `-D` and produce different preprocessed source.
- `// [permutation type] NAME={T1,T2}` — module-scope `type_param T : IFoo;` bindings; values are Slang type expressions passed to `IComponentType::specialize` as `Expr` `SpecializationArg`. Each combination bakes a fully concrete variant at compile time.

CLI `-P` / `--permutation-type` overrides **replace** a file-level directive with the same name entirely (see `mergePermutationDefines`). Runtime-dynamic type selection (`IShaderObject::setObject`, live `specialize`) is deliberately **not** supported — the point is that shipped binaries don't need `slang.dll` or source.

### Public API shape (`include/slangmake.hpp` for C++; `include/slangmake.h` is the parallel C ABI used by FFI consumers, implemented in `src/slangmake_c.cpp`)

Single header, ~1100 lines. Three layers:

1. **Data types** — `CompileOptions`, `Permutation`, `PermutationDefine`, `ShaderConstant`, enums (`Target`, `Codec`, `Optimization`, …), format POD structs under `namespace fmt` (packed with `#pragma pack(push, 4)`).
2. **Writers** — `Compiler::compile`, `BlobWriter::addEntry`/`finalize`/`writeToFile`, `BatchCompiler::compileFile`/`compileDirectory`.
3. **Readers** — `BlobReader` (entries + deps), `ReflectionView` (raw tables + decoded helpers).

Runtime-safe `detail::` helpers shared across `.cpp`s live in `src/slangmake_runtime_internal.h` (hashing, padding, compression glue, blob target encoding). Slang-dependent helpers live in `src/slangmake_internal.h` (`toSlangCompileTarget`, `serializeReflection`). Keep Slang headers out of files compiled into `slangmake-rt-lib`.

## Conventions worth knowing

- C++20, `namespace slangmake`, single public header. Prefer `std::span` / `std::string_view` at API boundaries; don't return owned vectors where a span suffices.
- Format POD structs under `fmt::` are serialized to disk — any field change is a **blob format break** and must bump `kBlobVersion` / `kReflectionVersion` in the same commit.
- `#pragma pack(push, 4)` is load-bearing for the `fmt::` structs (`static_assert`s on `sizeof` live next to the definitions — keep them).
- Tests are organised by subsystem (`test_blob.cpp`, `test_reflection.cpp`, `test_incremental.cpp`, `test_parallel.cpp`, `test_cli.cpp`, `test_capi.cpp`, …). When adding a feature, extend the matching file rather than creating a new one.
- `CHANGELOG` follows Keep-a-Changelog; add an `[Unreleased]` entry for user-visible changes.

# Permutations

A slangmake "permutation" is one specific instantiation of a shader: one combination of preprocessor defines and one binding of any module-scope generic `type_param`s. Declare the axes in the `.slang` source (or on the CLI), and the compiler bakes the Cartesian product into the output blob as one independent entry per cell.

```slang
// [permutation]      USE_SHADOW={0,1}
// [permutation]      QUALITY={low,med,high}
// [permutation type] MAT={Metal,Wood}

type_param MAT : IBrdf;
ParameterBlock<MAT> gMat;
// ...
```

This source produces 2 × 3 × 2 = 12 entries, each with its own bytecode, reflection, binding footprint and `depsIdx` record. At runtime you pick the cell you want:

```cpp
Permutation q;
q.constants = {{"USE_SHADOW", "1"}, {"QUALITY", "high"}};
q.typeArgs  = {{"MAT", "Wood"}};
auto entry  = reader->find(q);
```

## Two axis kinds

| Kind | Declaration | Emits as | Good for |
| --- | --- | --- | --- |
| **Constant** | `// [permutation] NAME={a,b,c}` | `#define NAME <value>` during compile | Toggle flags, quality levels, numeric knobs, target-specific paths |
| **Type** | `// [permutation type] NAME={T1,T2}` + `type_param NAME : IFoo;` | `IComponentType::specialize(SpecializationArg::fromType(T))` | Material / BRDF / lighting models, anything you'd express as a Slang interface impl |

Both show up in the same `PermutationDefine` vector with a `kind` field distinguishing them. The expander routes Constant values into `Permutation::constants` and Type values into `Permutation::typeArgs`.

### When to use Constant

A constant axis is essentially `#ifdef`. It changes the preprocessed source — so it can flip entire code paths, swap `Texture2D` for `Texture2DArray`, change `numthreads`, pick a different output format, etc. It cannot change the **static type** of a Slang variable in a way that the type system respects: `#if MAT==0 typedef Metal T; #else typedef Wood T; #endif` works, but you lose interface-polymorphism ergonomics.

### When to use Type

A type axis works through Slang's specialisation machinery, so:

- The code inside the shader can use `type_param T : IBrdf` exactly once and slangmake produces one fully concrete variant per `T`.
- Reflection reflects the **specialised** layout — `ParameterBlock<T>`'s binding footprint is correct for the concrete `T`, not for the generic placeholder. Bind-path walkers see the specialised tables directly.

Type axes are the right answer whenever you would otherwise write a macro-switched `typedef` chain.

## The parser

Magic comments are scanned line-by-line by `PermutationParser::parse` / `parseFile`. The rules:

- Must start with `//` (after optional leading whitespace).
- Followed by `[permutation]` or `[permutation type]` and the axis name.
- Axis name follows C identifier rules.
- Values are comma-separated, braces required: `NAME={a,b,c}`. Whitespace inside braces is stripped.
- Block-commented (`/* ... */`) directives are skipped entirely.
- Malformed lines drop silently — safer than failing noisy but wrong.

```slang
// [permutation] OK={0,1}           // parsed
/* // [permutation] IGNORED={0,1} */ // skipped (inside block comment)
// [permutation] BAD=missing_braces  // dropped
```

Alongside these, `sourceHasNoReflection` / `fileHasNoReflection` detects the related `// [noreflection]` directive that opts a single file out of reflection serialisation.

## CLI overrides

`-P NAME={a,b,c}` and `--permutation-type NAME={T1,T2}` on the command line produce `PermutationDefine` entries too. They flow through `mergePermutationDefines(file, cli)`:

- Any CLI entry whose `name` matches a file-level entry **replaces** that file entry entirely — values, kind, all of it. The merge key is just the name.
- Non-colliding file entries pass through unchanged, in their original order.
- Non-colliding CLI entries append at the end.

So:

```bash
slangmake -i shaders/ -o out -t SPIRV -p sm_6_5 \
          -P USE_SHADOW={1} \
          --permutation-type MAT={Wood}
```

...restricts the output to just those two values of those two axes, keeping any other file-declared axes intact. Handy for debugging one variant without editing the shader.

## The expander

`PermutationExpander::expand(defines)` walks the vector and produces the full Cartesian product:

```cpp
PermutationDefine shadow{ "USE_SHADOW", {"0", "1"} };
PermutationDefine qual{   "QUALITY",    {"low", "med", "high"} };
PermutationDefine mat{    "MAT",        {"Metal", "Wood"}, PermutationDefine::Kind::Type };

auto perms = PermutationExpander::expand({shadow, qual, mat});
// perms.size() == 12
```

Within each resulting `Permutation`, `constants` and `typeArgs` are sorted alphabetically by name so keys are canonical across runs. Empty value lists are skipped (no multiplicative effect).

The base case is important: `expand({})` returns exactly one `Permutation` with empty `constants` and `typeArgs`. A shader with no declared axes still compiles as a single-entry blob.

## Keys

Every `Permutation` has a canonical string key (see `Permutation::key()`) used as the blob entry identifier:

```
constants-part  |  typeArgs-part
─────────────────  ───────────────
NAME=VALUE _ NAME=VALUE  |  NAME=VALUE _ NAME=VALUE
```

- Both parts sort names alphabetically.
- The `|` separator only appears when `typeArgs` is non-empty.
- When a permutation has *only* type args, the key begins with a leading `|` (e.g. `|MAT=Wood`). This makes `MAT=Wood` (constant axis named MAT) and `|MAT=Wood` (type axis named MAT) unambiguous, so the same axis name can safely coexist as both kinds on disk (even if that is rarely useful in practice).

Runtime lookup:

```cpp
Permutation q;
q.constants = {{"USE_SHADOW", "1"}, {"QUALITY", "high"}};
q.typeArgs  = {{"MAT", "Wood"}};
auto e = reader->find(q);   // overload taking a Permutation
```

Or, if you only have constants:

```cpp
std::array<ShaderConstant, 2> c{{ {"USE_SHADOW", "1"}, {"QUALITY", "high"} }};
auto e = reader->find(c);   // overload taking std::span<const ShaderConstant>
```

## Interaction with global defines

`CompileOptions::defines` is applied to **every** permutation of a given compile — these are your global flags (`NDEBUG`, `PLATFORM_VK`, build-time quality ceiling, etc.). A `Permutation::constants` entry stacks on top and can override the value of the same name, per-variant.

Practically: put invariant build configuration in `CompileOptions::defines`, put per-variant axes in `[permutation]`.

## Scaling

Permutation count multiplies fast — three three-value axes and one five-value axis is 135 cells. slangmake mitigates the cost three ways:

1. **Parallel compile**: `BatchCompiler::setJobs(N)` spawns N workers with independent Slang global sessions. Most CPUs saturate around `-j <physical cores>` for our workload.
2. **Reflection deduplication**: permutations whose reflection payload turns out byte-identical share a single on-disk copy. Typical for axes that only flip behaviour inside a function without adding or removing bindings.
3. **Per-entry dependency tracking**: incremental rebuilds only recompile permutations whose specific dependency subgraph has changed. Edit an `.hlsli` that's `#include`d only from the shadow path → only the `USE_SHADOW=1` permutations rebuild.

If a shader's axis count is genuinely in the thousands, consider two things before chalking it up to "combinatorial explosion":

- **Collapse dependent axes.** If QUALITY controls only `TAP_COUNT` and `TAP_COUNT` is a uniform anyway, it does not need to be a permutation axis — just feed it via CB at runtime.
- **Split sensitive axes into separate shader files.** If MATERIAL has 50 values and QUALITY has 10, but only QUALITY actually changes the bytecode meaningfully while MATERIAL is just a CB difference, demote MATERIAL to a CB knob and keep QUALITY as the real permutation axis.

## Limits of the model

The permutation system is a **compile-time enumeration**. Anything that requires changing the specialisation at bind time — `IShaderObject::setObject(slot, arbitraryIMaterialImpl)` style runtime polymorphism — is not reachable through slangmake alone. That workflow requires a live Slang session; the slangmake answer is to enumerate the concrete material types you actually ship as a type axis and look up the right entry.

See also: [reflection.md](reflection.md) for the raw tables a bind path consumes, and [integration.md](integration.md) for the runtime lookup patterns around permutation selection.

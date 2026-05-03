# Raw reflection

`ReflectionView` exposes the on-disk reflection tables as zero-copy spans. This is the only reflection surface slangmake ships — converting the tables into the bind-time tuples a Vulkan / D3D12 / Metal RHI needs is left to the consuming engine. This doc is the map of what's in the tables.

## Table-index model

Every record references others by `uint32_t` index into a fixed-stride table. Nothing is a pointer, nothing is a string inside a record — strings are indices into a single interned pool, and repeated integer lists (struct field indices, per-entry child lists, per-category offset tuples) are indices into one shared `u32Pool`. The invariant `u32 == -1` (`fmt::kInvalidIndex`) means "absent"; always test before dereferencing.

The upshot is that `ReflectionView` is pure zero-copy — every accessor is a `std::span` into the decompressed reflection bytes, with no heap allocations. You can safely traverse reflection from a hot frame without worrying about per-call cost.

## The tables

| Accessor | Records | Purpose |
| --- | --- | --- |
| `types()` | `fmt::Type[]` | Every distinct type the program references (scalars, vectors, matrices, structs, arrays, resources, containers, generics) |
| `typeLayouts()` | `fmt::TypeLayout[]` | Layout of each type under the current compile options (size, stride, descriptor-set / binding info) |
| `variables()` | `fmt::Variable[]` | Declared variables (globals, entry-point parameters, struct fields, function parameters) |
| `varLayouts()` | `fmt::VarLayout[]` | Per-category offset / space / binding information for a variable, paired with a type layout |
| `functions()` | `fmt::Function[]` | Declared functions (entry points and ordinary) |
| `generics()` | `fmt::Generic[]` | `GenericReflection` records for any declaration that takes generic parameters |
| `decls()` | `fmt::Decl[]` | The module's declaration tree — root, children, parent |
| `entryPoints()` | `fmt::EntryPoint[]` | Entry-point-specific metadata (stage, thread-group size, parameter layout list, hash) |
| `attributes()` | `fmt::Attribute[]` + `attributeArgs()` | User-attribute nodes and their arguments |
| `modifierPool()` | `uint32_t[]` | Slang modifier-kind enums attached to variables / functions |
| `hashedStrings()` | `fmt::HashedStr[]` | Names indexed with a precomputed hash for O(1) lookup |
| `bindingRanges()` | `fmt::BindingRange[]` | "This many bindings starting at slot X of set Y" segments |
| `descriptorSets()` | `fmt::DescriptorSet[]` | Per-set binding ranges (for building `VkDescriptorSetLayout`) |
| `descriptorRanges()` | `fmt::DescriptorRange[]` | Individual `(bindingType, count, category)` ranges within a set |
| `subObjectRanges()` | `fmt::SubObjectRange[]` | `ParameterBlock` / existential sub-objects, with their space offsets |
| `u32Pool()` | `uint32_t[]` | Shared pool for variable-length integer lists referenced from other records |

Scalar header accessors for whole-program facts:

```cpp
uint64_t                firstEntryPointHash() const;
uint32_t                globalConstantBufferBinding() const;
uint32_t                globalConstantBufferSize() const;
uint32_t                bindlessSpaceIndex() const;
std::optional<uint32_t> globalParamsVarLayout() const;
```

## Strings

Names live in a single NUL-separated pool. Get one by string index:

```cpp
std::string_view s = rv.string(idx);   // empty when idx == kInvalidIndex
```

`hashedStrings()` is a parallel table of `{strIdx, hash}` pairs — Slang reports pre-hashed copies of every "interesting" name (field, variable, attribute) so you can build an `unordered_map` keyed by `hash` directly instead of rehashing each `string_view` at query time. Typical use:

```cpp
auto hs = rv.hashedStrings();
absl::flat_hash_map<uint32_t, uint32_t> index;
for (uint32_t i = 0; i < hs.size(); ++i)
    index.emplace(hs[i].hash, i);
// Later:
auto it = index.find(slangmake::detail::hash("uConstants"));
if (it != index.end()) { auto name = rv.hashedString(it->second); ... }
```

## The u32Pool convention

Many records store "an offset + a count into `u32Pool`" to reference a variable-length list. Examples:

| Field | Pool slice contains |
| --- | --- |
| `TypeLayout.fieldLayoutPoolOff / fieldCount` | indices into `varLayouts()` for each struct field |
| `TypeLayout.categoryPoolOff / categoryCount` | `SlangParameterCategory` enum values for each occupied category |
| `TypeLayout.sizePoolOff` (`categoryCount * 3` ints) | `(size, stride, alignment)` triplet per category |
| `VarLayout.categoryPoolOff / categoryCount` | `SlangParameterCategory` values parallel to `offsetPoolOff` and `bindingSpacePoolOff` |
| `VarLayout.offsetPoolOff` | one offset per category in the VarLayout |
| `VarLayout.bindingSpacePoolOff` | one space per category in the VarLayout |
| `EntryPoint.paramVarLayoutOff / paramVarLayoutCount` | `varLayouts()` indices for each entry-point parameter |
| `Decl.childOff / childCount` | `decls()` indices for each child declaration |
| `Generic.constraintPoolOff / constraintCount` | flat list of `(typeParamIdx, constraintTypeIdx)` pairs |
| `EntryRecord.depsIdxOff / depsIdxCount` | indices into the blob-level `DepEntry[]` (per-entry dep tracking) |

Always bounds-check against `u32Pool().size()` before reading — mangled or truncated blobs should be detected, not crashed on.

## Walking a struct type

```cpp
auto types = rv.types();
auto tls   = rv.typeLayouts();
auto vls   = rv.varLayouts();
auto vars  = rv.variables();
auto pool  = rv.u32Pool();

const auto& tl = tls[someTypeLayoutIdx];
if (tl.kind == static_cast<uint32_t>(slang::TypeReflection::Kind::Struct)) {
    for (uint32_t i = 0; i < tl.fieldCount; ++i) {
        uint32_t fieldVlIdx = pool[tl.fieldLayoutPoolOff + i];
        const auto& fieldVl = vls[fieldVlIdx];
        const auto& fieldVar = vars[fieldVl.varIdx];
        auto fieldName = rv.string(fieldVar.nameStrIdx);
        auto fieldTypeIdx = tls[fieldVl.typeLayoutIdx].typeIdx;
        // ...recurse into fieldVl.typeLayoutIdx for nested structs...
    }
}
```

## Reading per-category offsets

The primitive your bind path needs:

```cpp
const auto& vl = rv.varLayouts()[someVarLayoutIdx];
auto pool = rv.u32Pool();
for (uint32_t i = 0; i < vl.categoryCount; ++i) {
    uint32_t cat    = pool[vl.categoryPoolOff + i];
    uint32_t offset = pool[vl.offsetPoolOff + i];
    uint32_t space  = pool[vl.bindingSpacePoolOff + i];
    // cat is SLANG_PARAMETER_CATEGORY_UNIFORM / _DESCRIPTOR_TABLE_SLOT / ...
}
```

The `offset` semantics depend on the category — bytes for `Uniform`, slot index for `DescriptorTableSlot`, etc. Size / stride / alignment for the *type* along the same category are in the parallel `TypeLayout.sizePool` triplet.

## Walking the decl tree

`decls()` is the module's declaration tree, root-first. Root is at index 0 when present. Each record carries kind, name, payload index into an auxiliary table (struct body, function body, etc. — consult `Decl.kind`), and a `(childOff, childCount)` slice into `u32Pool` with children.

```cpp
auto pool = rv.u32Pool();
std::function<void(uint32_t)> walk = [&](uint32_t idx) {
    auto node = rv.decl(idx);
    // node.kind (slang::DeclReflection::Kind), node.name, node.parentDeclIdx,
    // node.payloadIdx, node.children (std::span<const uint32_t>)
    for (uint32_t c : node.children) walk(c);
};
if (auto root = rv.rootDecl()) walk(0);
```

## Binding / descriptor / sub-object ranges

`TypeLayout` records carry three parallel tables for container types (CB, PB, struct, TextureBuffer, SSB):

```
tl.bindingRangeOff / bindingRangeCount   → bindingRanges()
tl.descriptorSetOff / descriptorSetCount → descriptorSets()
tl.subObjectRangeOff / subObjectRangeCount → subObjectRanges()
```

These model the same information Slang exposes under `TypeLayoutReflection::getBindingRangeType`, `getDescriptorSetCount`, and `getSubObjectRangeCount`. Typical use:

```cpp
const auto& tl     = rv.typeLayouts()[pbLayoutIdx];
auto        sets   = rv.descriptorSets();
auto        ranges = rv.descriptorRanges();

for (uint32_t i = 0; i < tl.descriptorSetCount; ++i) {
    const auto& ds = sets[tl.descriptorSetOff + i];
    for (uint32_t j = 0; j < ds.descriptorRangeCount; ++j) {
        const auto& dr = ranges[ds.descriptorRangeStart + j];
        // dr.indexOffset, dr.descriptorCount, dr.bindingType, dr.parameterCategory
    }
}
```

`SubObjectRange` records connect a `BindingRange` to a space offset — this is what lets a bind-path walker compute the absolute space of a resource inside a nested `ParameterBlock`. The record's `bindingRangeIndex` points back into `bindingRanges()`; `spaceOffset` is the sub-object's space delta; `offsetVarLayoutIdx` gives you the `VarLayout` whose offsets you'd apply when descending.

## Entry points

Each `EntryPoint` record has everything needed for a pipeline-builder's per-stage setup:

```cpp
for (const auto& ep : rv.entryPoints()) {
    auto name   = rv.string(ep.nameStrIdx);
    auto stage  = ep.stage;                  // SLANG_STAGE_*
    auto tgSize = std::array{ ep.threadGroupSizeX,
                              ep.threadGroupSizeY,
                              ep.threadGroupSizeZ };
    uint64_t hash = (uint64_t{ep.hashHigh} << 32) | ep.hashLow;
    auto pool = rv.u32Pool();
    for (uint32_t k = 0; k < ep.paramVarLayoutCount; ++k) {
        uint32_t vlIdx = pool[ep.paramVarLayoutOff + k];
        // rv.varLayouts()[vlIdx] is the parameter's binding info
    }
}
```

`decodedEntryPoints()` is a convenience wrapper that materialises this into a vector of structs with each `Param` decoded — use it for simple cases, drop to the raw table for bulk analysis.

## Attributes

User-written `[attrName(args...)]` attributes are reachable from `Variable.attrOff / attrCount` and `Function.attrOff / attrCount`:

```cpp
const auto& v = rv.variables()[varIdx];
auto attrs = rv.attributes();
auto args  = rv.attributeArgs();

for (uint32_t i = 0; i < v.attrCount; ++i) {
    const auto& a = attrs[v.attrOff + i];
    auto name = rv.string(a.nameStrIdx);
    for (uint32_t j = 0; j < a.argCount; ++j) {
        const auto& arg = args[a.argOff + j];
        switch (arg.kind) {
            case fmt::AttrArg::Int:    /* arg.raw bit-cast */ break;
            case fmt::AttrArg::Float:  /* arg.raw bit-cast to float */ break;
            case fmt::AttrArg::String: /* rv.string(arg.strIdx) */ break;
        }
    }
}
```

Slang emits its built-in `[shader(...)]` and `[numthreads(...)]` attributes via dedicated reflection APIs rather than `getUserAttribute*` — those land in `EntryPoint.stage` and `EntryPoint.threadGroupSize*` respectively, not in this user-attribute table.

## Correlating types and layouts

One `Type` can have many `TypeLayout`s (per compile target + rule set), but in practice slangmake emits one per (type, layout-rules) combination. The forward edges you care about:

- `Variable.typeIdx` → `Type`
- `VarLayout.varIdx` → `Variable`
- `VarLayout.typeLayoutIdx` → `TypeLayout` (the one *specific to this variable's binding context*)
- `TypeLayout.typeIdx` → `Type`

So going from a var layout to the underlying type name is:

```cpp
const auto& vl = rv.varLayouts()[i];
const auto& v  = rv.variables()[vl.varIdx];
const auto& tl = rv.typeLayouts()[vl.typeLayoutIdx];
const auto& t  = rv.types()[tl.typeIdx];
auto typeName  = rv.string(t.fullNameStrIdx);
auto varName   = rv.string(v.nameStrIdx);
```

## When to prefer `decodedEntryPoints` / `decodedGlobalParameters`

These helpers are there for the common 95%:

- `decodedEntryPoints()` — shipping engines iterating per-stage info.
- `decodedGlobalParameters()` — listing global bindings for logging.

Reach for the raw tables when:

- You're walking the whole module structure, not just one variable.
- You need the relationships across multiple records (e.g. "find every `VarLayout` whose `TypeLayout`'s `kind` is `ShaderResource` **and** whose owning `Variable` has a specific user attribute").
- You're generating offline code (C++ bind headers, rust-style FFI stubs, shader linting rules).
- You're implementing the engine's bind path: `(set, slot, byte-offset)` resolution against `BindingRange` / `DescriptorSet` / `DescriptorRange` / `SubObjectRange` is the expected consumer of these tables.

## See also

- The `fmt::*` struct definitions in `include/slangmake.hpp` are the ground truth for every field mentioned here. The C-side `sm_fmt_*_t` mirrors in `include/slangmake.h` are kept layout-compatible via `static_assert` in `src/slangmake_c.cpp`.

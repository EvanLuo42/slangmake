# Cursor — binding resources from a slangmake blob

`ReflectionView::Cursor` is a Slang-`ShaderCursor`-style navigator over the
serialised reflection in a slangmake blob. It lets an engine answer "where
does this shader variable live?" without a live Slang session: name/index
navigation, accumulated per-category offsets and spaces, descriptor-set
enumeration — all straight off the `.bin`.

```cpp
#include "slangmake.h"
using namespace slangmake;

auto reader = BlobReader::openFile("shaders.bin");
auto entry  = reader->find(myPermutation);
ReflectionView rv(entry->reflection);

auto root = rv.rootCursor();
auto loc  = root["uConstants"]["color"].uniformLocation();
memcpy(cbStaging + loc.offsetBytes, &rgba, loc.sizeBytes);
```

## Contents

1. [Mental model](#mental-model)
2. [Construction](#construction)
3. [Navigation](#navigation)
4. [Auto-dereference](#auto-dereference)
5. [Queries: what does the cursor know?](#queries)
6. [Descriptor-set layout enumeration](#descriptor-set-layout-enumeration)
7. [End-to-end: binding a ParameterBlock](#end-to-end-binding-a-parameterblock)
8. [Limitations](#limitations)

## Mental model

Every Slang parameter binding is a bag of per-category `(offset, space)`
pairs. A `Uniform` byte offset says "skip N bytes into the enclosing CB";
a `DescriptorTableSlot` says "this is descriptor `offset` in descriptor set
`space`"; `SubElementRegisterSpace` records the space shift introduced by a
surrounding `ParameterBlock`. There are ~22 such categories in Slang's
reflection model.

A `Cursor` carries two 32-wide arrays — one for accumulated offsets, one for
accumulated spaces — plus a `typeLayoutIdx` pointing at the type at the
current position. Each navigation step reads the traversed
`VariableLayout`'s category list and **adds** its contribution to the
arrays. At a leaf, you read back whichever category your binding model
cares about.

```
root  ──["gMat"]──►  ParameterBlock field
                     offsets[SubElementRegisterSpace] += 1
                     typeLayoutIdx = ParameterBlock<Wood>

           ──["albedo"]──►  Wood::albedo (Texture2D)
                            offsets[DescriptorTableSlot] += 0
                            typeLayoutIdx = Texture2D
                            resourceBinding() = { space: 1, index: 0 }
                              (space combines SubElementRegisterSpace + leaf space)
```

## Construction

Three factory functions on `ReflectionView`:

| Entry point | Starting position |
| --- | --- |
| `rv.rootCursor()` | The program's global parameter block (all module-scope globals) |
| `rv.entryPointCursor(i)` | Entry point `i`'s own parameter block (entry-point uniforms / varying I/O) |
| `rv.findGlobalParam(name)` | Shorthand for `rv.rootCursor()[name]` |

All three return a default-constructed invalid cursor when the blob carries
no reflection, or when the entry-point index is out of range — no
exceptions, no UB.

```cpp
auto root = rv.rootCursor();
if (!root.valid()) return;   // shader has no reflection section

auto ep0 = rv.entryPointCursor(0);
```

## Navigation

Two primitives and their `operator[]` wrappers:

```cpp
Cursor field(std::string_view name) const;   // struct field by name
Cursor element(uint32_t index) const;         // array element by index
Cursor operator[](std::string_view name) const;
Cursor operator[](const char* name) const;
Cursor operator[](uint32_t index) const;
```

`field` resolves a name against the current struct's `fieldLayoutPool`. Miss
→ invalid cursor. `element` computes per-category strides from the element
type's size pool and adds `index * stride[cat]` to each offset. Out-of-range
indices are **not** bounds-checked against the static array length (Slang
reports the length if you need it; see `types()[...].elementCount`).

Two less-frequently-used navigators:

```cpp
Cursor dereference() const;      // explicitly step into a CB/PB's element layer
Cursor container() const;        // step onto the CB/PB container resource itself
Cursor explicitCounter() const;  // follow an AppendStructuredBuffer's counter VarLayout
```

You will normally never call `dereference` yourself — see the next section.
`container()` is what you call when you need the `(space, slot)` of the
implicit constant buffer inside a `ParameterBlock`.

Chain freely:

```cpp
rv.rootCursor()
  ["scene"]["lights"][3]["direction"]
  .uniformLocation();
```

## Auto-dereference

`field()` and `element()` automatically unwrap container types —
`ConstantBuffer<T>`, `ParameterBlock<T>`, `TextureBuffer<T>`,
`ShaderStorageBuffer` — before looking up a member. So with:

```slang
struct Constants { float4 color; int mode; }
ConstantBuffer<Constants> uConstants;
```

you write `root["uConstants"]["color"]`, not
`root["uConstants"].dereference()["color"]`. The cursor walks the outer
`VarLayout` (grabbing the CB's descriptor slot) and then transparently
descends into `T`'s layout (where `color` is at Uniform offset 0).

If a caller genuinely wants the outer wrapper layer — e.g. to bind the
backing `VkBuffer` of a PB's implicit CB — they call `container()`, which
moves sideways onto the `containerVarLayout` instead of through to the
element.

## Queries

### Scalar queries

```cpp
bool              valid() const;
std::string_view  name() const;               // name of the VarLayout that got us here (empty at root)
uint32_t          typeLayoutIndex() const;
uint32_t          typeKind() const;           // slang::TypeReflection::Kind value
```

### Per-category accumulators

```cpp
uint32_t offsetFor(uint32_t category) const;  // SLANG_PARAMETER_CATEGORY_*
uint32_t spaceFor(uint32_t category) const;
uint32_t sizeFor(uint32_t category) const;    // size at the CURRENT type (not accumulated)
uint32_t strideFor(uint32_t category) const;  // stride at the CURRENT type
```

`offsetFor(SLANG_PARAMETER_CATEGORY_UNIFORM)` gives the accumulated byte
offset into the enclosing constant buffer. `spaceFor(SLANG_PARAMETER_CATEGORY_DESCRIPTOR_TABLE_SLOT)`
gives the Vulkan-style descriptor set — relative to the cursor's local
scope, so inside a PB this is 0 until you also add the PB's register-space
accumulators (use the convenience methods below for the combined value).

### Convenience shortcuts

```cpp
uint32_t uniformOffset() const;   // offsetFor(Uniform)
uint32_t descriptorSlot() const;  // offsetFor(DescriptorTableSlot)
uint32_t descriptorSet() const;   // absolute set index, combining
                                  // DescriptorTableSlot space +
                                  // SubElementRegisterSpace +
                                  // RegisterSpace accumulators
```

### Bind-ready views

```cpp
struct UniformLocation { uint32_t offsetBytes; uint32_t sizeBytes; };
UniformLocation uniformLocation() const;

struct ResourceBinding {
    uint32_t space;
    uint32_t index;
    uint32_t category;  // which SlangParameterCategory supplied the binding
};
std::optional<ResourceBinding> resourceBinding() const;
```

`uniformLocation` is exactly the memcpy destination for a CB write:
`{offset, size}`. Writing a whole `Constants` struct at once? Use it on the
CB's *element* cursor (`root["uConstants"].dereference()`).

`resourceBinding` walks the current `VarLayout`'s category list, picks the
first non-uniform category (the one that actually identifies a resource),
and returns the **absolute** space (with PB register-space shifts folded
in). Typical engine usage:

```cpp
auto bind = cursor.resourceBinding();
if (!bind) return;  // cursor is at a uniform leaf, not a resource
VkWriteDescriptorSet w{};
w.dstSet     = mySets[bind->space];
w.dstBinding = bind->index;
w.descriptorType = mapCategoryToVk(bind->category);
```

## Descriptor-set layout enumeration

Navigation tells you where one thing lives. Allocating a
`VkDescriptorSetLayout` or populating a D3D12 root-parameter table, though,
needs the full inventory — how many CBVs, SRVs, UAVs, samplers sit in a
given set. `Cursor::descriptorSetLayout()` returns exactly that:

```cpp
struct DescriptorBindingInfo {
    uint32_t slot;        // binding index within the set
    uint32_t count;       // descriptor count (>= 1; large for bindless arrays)
    uint32_t bindingType; // SLANG_BINDING_TYPE_*
    uint32_t category;    // SlangParameterCategory
};
struct DescriptorSetInfo {
    uint32_t                            space;     // absolute set / register space
    std::vector<DescriptorBindingInfo>  bindings;
};

std::vector<DescriptorSetInfo> descriptorSetLayout() const;
```

Call it on the root cursor to get the program's global sets:

```cpp
for (const auto& set : rv.rootCursor().descriptorSetLayout()) {
    std::vector<VkDescriptorSetLayoutBinding> vkBindings;
    for (const auto& b : set.bindings)
        vkBindings.push_back({ b.slot,
                               mapToVkDescriptorType(b.bindingType),
                               b.count,
                               VK_SHADER_STAGE_ALL,
                               nullptr });
    vkCreateDescriptorSetLayout(dev, &ci, nullptr, &layouts[set.space]);
}
```

On a `ParameterBlock` cursor it returns that PB's sets (typically one
primary set at absolute space = cursor's `descriptorSet()`, plus one per
nested sub-object).

## End-to-end: binding a ParameterBlock

Pulling everything together — given a shader like:

```slang
// [permutation type] MAT={Metal,Wood}
interface IBrdf { float3 shade(); }
struct Wood : IBrdf {
    Texture2D<float4> albedo;
    SamplerState      samp;
    float4            tint;
    float3 shade() { return albedo.SampleLevel(samp, float2(0.5,0.5), 0).rgb * tint.rgb; }
}
type_param MAT : IBrdf;
ParameterBlock<MAT> gMat;
```

binding the `Wood` variant at runtime looks like:

```cpp
// 1. Find the specialised entry and parse reflection.
Permutation perm; perm.typeArgs = {{"MAT", "Wood"}};
auto entry = reader->find(perm);
ReflectionView rv(entry->reflection);

auto gMat = rv.rootCursor()["gMat"];

// 2. Build the VkDescriptorSetLayout the PB needs.
for (const auto& set : gMat.descriptorSetLayout()) {
    auto layout = buildVkLayout(dev, set);    // one VkDescriptorSetLayout
    mySets[set.space] = allocSet(dev, layout);
}

// 3. Bind the implicit CB that holds `tint`.
auto cb      = gMat.container();
auto cbBind  = *cb.resourceBinding();
writeBuffer(mySets[cbBind.space], cbBind.index, myTintBuffer);

// 4. Bind texture + sampler.
auto albedoBind = *gMat["albedo"].resourceBinding();
auto sampBind   = *gMat["samp"].resourceBinding();
writeImage  (mySets[albedoBind.space], albedoBind.index, woodAlbedoView);
writeSampler(mySets[sampBind.space],   sampBind.index,   linearSampler);

// 5. Write tint uniform data into myTintBuffer at the right offset.
auto tintLoc = gMat["tint"].uniformLocation();
memcpy(cbStaging + tintLoc.offsetBytes, &tintRgba, tintLoc.sizeBytes);

// 6. Bind sets at dispatch.
vkCmdBindDescriptorSets(cmd, pipeline, /* firstSet */ cbBind.space, 1,
                        &mySets[cbBind.space], 0, nullptr);
```

Everything here is pure lookup into a memory-mapped `.bin` — no Slang
runtime, no JSON, no allocations on the hot path.

## Limitations

The Cursor handles every binding shape that can be **fully determined at
compile time**. It cannot do:

- **Runtime specialisation of generics / interfaces.** Slang's
  `IShaderObject::setObject(slot, concreteImpl)` for an interface-typed
  slot requires the compiler to specialise and re-codegen at bind time.
  slangmake's model bakes the axis as a `[permutation type]` instead, so
  `rv.rootCursor()["gMat"]` is already the specialised cursor for the
  variant you looked up. If you need to change the concrete type, you
  switch to a different blob entry — you do not reconfigure the cursor.
- **Runtime `specialize()`.** Same reason. The serialised layout is a
  frozen snapshot of one specialisation; there is no IR to re-mix.
- **Slang `ShaderCursor`'s `setData` / `setResource` / `setObject` calls.**
  The Cursor tells you *where* to bind — it does not own any graphics-API
  resources or descriptor sets, so it can't write to them. Your engine
  issues the `vkUpdateDescriptorSets` / `vkCmdPushConstants` / `memcpy`
  based on the Cursor's readings.
- **Entry-point generic parameters.** Currently unsupported end-to-end —
  only module-scope `type_param` declarations participate in the
  permutation system.

Everything else Slang's ShaderCursor can reach — struct navigation, array
indexing, ParameterBlock sub-spaces, ConstantBuffer deref, AppendStructured-
Buffer counters, array-of-textures strides, entry-point uniforms,
push-constant offsets — the Cursor reaches too.

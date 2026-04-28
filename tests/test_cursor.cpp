#include <doctest/doctest.h>
#include <slang.h>

#include <filesystem>

#include "slangmake.h"

using namespace slangmake;

namespace fs = std::filesystem;

namespace
{

Compiler::Result compileBasic()
{
    Compiler       c;
    CompileOptions opts;
    opts.inputFile = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "with_resources.slang";
    opts.target    = Target::SPIRV;
    opts.profile   = "sm_6_5";
    return c.compile(opts, Permutation{});
}

Compiler::Result compileMaterial(std::string_view matType)
{
    Compiler       c;
    CompileOptions opts;
    opts.inputFile = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "generic_material.slang";
    opts.target    = Target::SPIRV;
    opts.profile   = "sm_6_5";
    Permutation perm;
    perm.typeArgs = {{"MAT", std::string(matType)}};
    return c.compile(opts, perm);
}

Compiler::Result compileResourceArray()
{
    Compiler       c;
    CompileOptions opts;
    opts.inputFile = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "resource_array.slang";
    opts.target    = Target::SPIRV;
    opts.profile   = "sm_6_5";
    return c.compile(opts, Permutation{});
}

} // namespace

TEST_CASE("Cursor::rootCursor returns a valid cursor and invalid on missing names")
{
    auto r = compileBasic();
    INFO("diag: " << r.diagnostics);
    REQUIRE(r.success);
    ReflectionView v(r.reflection);
    REQUIRE(v.valid());

    auto root = v.rootCursor();
    CHECK(root.valid());

    // Each declared global resolves through root[name].
    CHECK(root["uConstants"].valid());
    CHECK(root["uTexture"].valid());
    CHECK(root["uSampler"].valid());
    CHECK(root["uOutput"].valid());

    // Misspellings and unknown names produce invalid cursors, never crash.
    CHECK_FALSE(root["nope"].valid());
    CHECK_FALSE(root["uConstants"]["not_a_field"].valid());
}

TEST_CASE("Cursor auto-dereferences ConstantBuffer on struct-field access")
{
    auto r = compileBasic();
    REQUIRE(r.success);
    ReflectionView v(r.reflection);
    REQUIRE(v.valid());

    // Without an explicit dereference(), stepping into a field of a
    // ConstantBuffer<T> reaches into T directly — same ergonomics as Slang's
    // ShaderCursor.
    auto color = v.rootCursor()["uConstants"]["color"];
    REQUIRE(color.valid());
    CHECK(color.name() == "color");

    auto mode = v.rootCursor()["uConstants"]["mode"];
    REQUIRE(mode.valid());
    CHECK(mode.name() == "mode");
}

TEST_CASE("Cursor::uniformLocation reports struct-field byte offset and size")
{
    auto r = compileBasic();
    REQUIRE(r.success);
    ReflectionView v(r.reflection);
    REQUIRE(v.valid());

    // color is the first field of `struct Constants { float4 color; int mode; }`,
    // so it sits at offset 0 and covers 16 bytes. `mode` follows the float4 and
    // must land at offset >= 16 (std140-ish rules; different targets may pad
    // differently but mode can never precede color).
    auto colorLoc = v.rootCursor()["uConstants"]["color"].uniformLocation();
    CHECK(colorLoc.offsetBytes == 0);
    CHECK(colorLoc.sizeBytes == 16);

    auto modeLoc = v.rootCursor()["uConstants"]["mode"].uniformLocation();
    CHECK(modeLoc.offsetBytes >= 16);
    CHECK(modeLoc.sizeBytes == 4);
    CHECK(modeLoc.offsetBytes > colorLoc.offsetBytes);
}

TEST_CASE("Cursor::resourceBinding surfaces space/index for resource globals")
{
    auto r = compileBasic();
    REQUIRE(r.success);
    ReflectionView v(r.reflection);
    REQUIRE(v.valid());

    auto tex = v.rootCursor()["uTexture"];
    REQUIRE(tex.valid());
    auto texBind = tex.resourceBinding();
    REQUIRE(texBind.has_value());
    CHECK(texBind->category != 0); // not None / not Uniform

    auto samp = v.rootCursor()["uSampler"];
    REQUIRE(samp.valid());
    CHECK(samp.resourceBinding().has_value());

    auto buf = v.rootCursor()["uOutput"];
    REQUIRE(buf.valid());
    CHECK(buf.resourceBinding().has_value());

    // Three distinct resources must not share the exact same (space, index,
    // category). If they did, bind-site writes would collide.
    auto tb    = *tex.resourceBinding();
    auto sb    = *samp.resourceBinding();
    auto bb    = *buf.resourceBinding();
    auto tuple = [](const auto& b) { return std::tuple{b.space, b.index, b.category}; };
    CHECK(tuple(tb) != tuple(sb));
    CHECK(tuple(tb) != tuple(bb));
    CHECK(tuple(sb) != tuple(bb));
}

TEST_CASE("Cursor::resourceBinding preserves slot-0 bindings on resource arrays")
{
    auto r = compileResourceArray();
    INFO("diag: " << r.diagnostics);
    REQUIRE(r.success);
    ReflectionView v(r.reflection);
    REQUIRE(v.valid());

    auto tex0 = v.rootCursor()["gTextures"][0u];
    auto tex1 = v.rootCursor()["gTextures"][1u];
    REQUIRE(tex0.valid());
    REQUIRE(tex1.valid());

    auto bind0 = tex0.resourceBinding();
    auto bind1 = tex1.resourceBinding();
    REQUIRE(bind0.has_value());
    REQUIRE(bind1.has_value());
    CHECK(bind0->category == bind1->category);
    CHECK(bind0->space == bind1->space);
    CHECK(bind1->index >= bind0->index);
}

TEST_CASE("Cursor descends into a ParameterBlock and reaches the element fields")
{
    // The Wood specialisation of generic_material.slang lays out
    // `ParameterBlock<Wood>` with an albedo texture, a sampler, and a float4
    // uniform. Navigating into any of those from the root cursor exercises the
    // PB auto-deref path end-to-end.
    auto r = compileMaterial("Wood");
    INFO("diag: " << r.diagnostics);
    REQUIRE(r.success);
    ReflectionView v(r.reflection);
    REQUIRE(v.valid());

    auto root = v.rootCursor();
    REQUIRE(root.valid());

    auto gMat = root["gMat"];
    REQUIRE(gMat.valid());

    // Field navigation through a ParameterBlock must find Wood's members.
    auto albedo = root["gMat"]["albedo"];
    auto samp   = root["gMat"]["samp"];
    auto tint   = root["gMat"]["tint"];
    REQUIRE(albedo.valid());
    REQUIRE(samp.valid());
    REQUIRE(tint.valid());

    // The texture/sampler must carry a resource binding (with space and slot
    // inherited from the PB), and the uniform must carry a Uniform size of 16
    // bytes (float4).
    CHECK(albedo.resourceBinding().has_value());
    CHECK(samp.resourceBinding().has_value());
    auto tintLoc = tint.uniformLocation();
    CHECK(tintLoc.sizeBytes == 16);
}

TEST_CASE("Cursor tracks a distinct descriptor space for ParameterBlock contents")
{
    // The global `uOutput` lives in the root program space; `gMat`'s contents
    // live in the sub-object space introduced by the ParameterBlock. Unless
    // Slang has flattened everything into space 0 (possible but rare for PB),
    // the two descriptor-set accumulators must differ OR the PB must at least
    // accumulate a non-zero SubElementRegisterSpace offset at gMat itself.
    auto r = compileMaterial("Wood");
    REQUIRE(r.success);
    ReflectionView v(r.reflection);
    REQUIRE(v.valid());

    auto root = v.rootCursor();
    REQUIRE(root.valid());

    auto uOutput = root["uOutput"];
    auto albedo  = root["gMat"]["albedo"];
    REQUIRE(uOutput.valid());
    REQUIRE(albedo.valid());

    // The PB descent must produce some form of space separation: either the
    // accumulated RegisterSpace / SubElementRegisterSpace differs between the
    // root resource and the PB's child, or their descriptorSet() values do.
    uint32_t rootSet    = uOutput.descriptorSet();
    uint32_t pbChildSet = albedo.descriptorSet();
    uint32_t pbSpaceAcc = albedo.offsetFor(SLANG_PARAMETER_CATEGORY_SUB_ELEMENT_REGISTER_SPACE) +
                          albedo.offsetFor(SLANG_PARAMETER_CATEGORY_REGISTER_SPACE);
    INFO("rootSet=" << rootSet << " pbChildSet=" << pbChildSet << " pbSpaceAcc=" << pbSpaceAcc);
    CHECK((rootSet != pbChildSet || pbSpaceAcc != 0));
}

TEST_CASE("Cursor::explicitCounter is invalid on a plain RWStructuredBuffer")
{
    // `uOutput` in with_resources.slang is a plain RWStructuredBuffer (no
    // AppendStructuredBuffer counter), so explicitCounter() should return an
    // invalid cursor rather than crash.
    auto r = compileBasic();
    REQUIRE(r.success);
    ReflectionView v(r.reflection);
    REQUIRE(v.valid());

    auto buf = v.rootCursor()["uOutput"];
    REQUIRE(buf.valid());
    CHECK_FALSE(buf.explicitCounter().valid());
}

TEST_CASE("ReflectionView::entryPointCursor is valid for each declared entry point")
{
    auto r = compileBasic();
    REQUIRE(r.success);
    ReflectionView v(r.reflection);
    REQUIRE(v.valid());

    auto eps = v.entryPoints();
    REQUIRE_FALSE(eps.empty());
    for (uint32_t i = 0; i < eps.size(); ++i)
    {
        auto c = v.entryPointCursor(i);
        CHECK(c.valid());
    }
    CHECK_FALSE(v.entryPointCursor(static_cast<uint32_t>(eps.size())).valid());
}

TEST_CASE("Cursor::container is a no-op / invalid on non-container types")
{
    auto r = compileBasic();
    REQUIRE(r.success);
    ReflectionView v(r.reflection);
    REQUIRE(v.valid());

    // A plain texture has no container — nothing wraps it.
    CHECK_FALSE(v.rootCursor()["uTexture"].container().valid());
    // A scalar uniform inside a CB has no container either.
    CHECK_FALSE(v.rootCursor()["uConstants"]["color"].container().valid());
}

TEST_CASE("Cursor::container exposes the ConstantBuffer's binding")
{
    auto r = compileBasic();
    REQUIRE(r.success);
    ReflectionView v(r.reflection);
    REQUIRE(v.valid());

    // For a plain `ConstantBuffer<Constants>`, the outer VarLayout already
    // carries the CB binding; `container()` should land at the same (space,
    // slot) and still expose a resourceBinding (in case the engine wants a
    // uniform, deterministic "here is where the CB lives" call site).
    auto outer = v.rootCursor()["uConstants"];
    REQUIRE(outer.valid());
    auto cb = outer.container();
    REQUIRE(cb.valid());
    auto outerBind = outer.resourceBinding();
    auto cbBind    = cb.resourceBinding();
    REQUIRE(outerBind.has_value());
    REQUIRE(cbBind.has_value());
    CHECK(cbBind->space == outerBind->space);
    CHECK(cbBind->index == outerBind->index);
}

TEST_CASE("Cursor::container on a ParameterBlock returns the implicit CB binding")
{
    auto r = compileMaterial("Wood");
    INFO("diag: " << r.diagnostics);
    REQUIRE(r.success);
    ReflectionView v(r.reflection);
    REQUIRE(v.valid());

    auto gMat = v.rootCursor()["gMat"];
    REQUIRE(gMat.valid());

    auto cb = gMat.container();
    REQUIRE(cb.valid());

    // The implicit CB must expose a resource binding. Its space must match the
    // PB's sub-space (i.e. the absolute descriptor-set index at the PB cursor
    // position).
    auto cbBind = cb.resourceBinding();
    REQUIRE(cbBind.has_value());
    CHECK(cbBind->space == gMat.descriptorSet());

    // The CB's uniform payload size lives on the PB's *element* layout (Wood),
    // not on the container resource itself. Reach it via dereference().
    auto inside = gMat.dereference();
    REQUIRE(inside.valid());
    CHECK(inside.sizeFor(SLANG_PARAMETER_CATEGORY_UNIFORM) >= 16);
}

TEST_CASE("Cursor::descriptorSetLayout enumerates the root program's sets")
{
    auto r = compileBasic();
    REQUIRE(r.success);
    ReflectionView v(r.reflection);
    REQUIRE(v.valid());

    auto layout = v.rootCursor().descriptorSetLayout();
    REQUIRE_FALSE(layout.empty());

    // with_resources.slang has at least one CBV-like, one SRV-like, one
    // sampler and one UAV-like binding across its sets. Aggregate and count
    // them rather than pinning exact layout (which varies by target).
    size_t totalBindings = 0;
    for (const auto& set : layout)
        totalBindings += set.bindings.size();
    CHECK(totalBindings >= 4);

    // All emitted binding entries have a non-zero descriptor count.
    for (const auto& set : layout)
        for (const auto& b : set.bindings)
            CHECK(b.count >= 1);
}

TEST_CASE("Cursor::descriptorSetLayout on a ParameterBlock lists the PB's sets")
{
    auto r = compileMaterial("Wood");
    REQUIRE(r.success);
    ReflectionView v(r.reflection);
    REQUIRE(v.valid());

    auto gMat = v.rootCursor()["gMat"];
    REQUIRE(gMat.valid());

    auto pbLayout = gMat.descriptorSetLayout();
    REQUIRE_FALSE(pbLayout.empty());

    // The PB's sets must all live in a space distinct from the global
    // RWStructuredBuffer `uOutput`.
    auto uOutputSet = v.rootCursor()["uOutput"].descriptorSet();
    for (const auto& set : pbLayout)
    {
        INFO("pbSet.space=" << set.space << " uOutputSet=" << uOutputSet);
        CHECK(set.space != uOutputSet);
        CHECK_FALSE(set.bindings.empty());
    }

    // Aggregate the binding types seen across the PB's sets so a failing
    // test has a concrete list to inspect. We assert non-trivial count and
    // presence of at least one texture/sampler-shaped binding rather than
    // pinning the implicit-CB bindingType — Slang does not always surface
    // the PB's default CB as its own descriptor range (the container is
    // reached via Cursor::container() instead).
    size_t totalPbBindings   = 0;
    bool   sawImageOrSampler = false;
    for (const auto& set : pbLayout)
    {
        totalPbBindings += set.bindings.size();
        for (const auto& b : set.bindings)
        {
            if (b.bindingType == SLANG_BINDING_TYPE_TEXTURE || b.bindingType == SLANG_BINDING_TYPE_MUTABLE_TETURE ||
                b.bindingType == SLANG_BINDING_TYPE_SAMPLER ||
                b.bindingType == SLANG_BINDING_TYPE_COMBINED_TEXTURE_SAMPLER)
                sawImageOrSampler = true;
        }
    }
    CHECK(totalPbBindings >= 1);
    CHECK(sawImageOrSampler);
}

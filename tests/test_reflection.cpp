#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <functional>

#include "slangmake.h"

using namespace slangmake;

namespace fs = std::filesystem;

namespace
{

Compiler::Result compileWithResources()
{
    Compiler       c;
    CompileOptions opts;
    opts.inputFile = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "with_resources.slang";
    opts.target    = Target::SPIRV;
    opts.profile   = "sm_6_5";
    return c.compile(opts, Permutation{});
}

} // namespace

TEST_CASE("ReflectionView rejects malformed bytes")
{
    ReflectionView v(std::span<const uint8_t>{});
    CHECK_FALSE(v.valid());

    uint8_t        junk[16] = {0};
    ReflectionView v2(std::span<const uint8_t>(junk, 16));
    CHECK_FALSE(v2.valid());
}

TEST_CASE("ReflectionView surfaces every public table for with_resources.slang")
{
    auto r = compileWithResources();
    INFO("diagnostics: " << r.diagnostics);
    REQUIRE(r.success);
    REQUIRE_FALSE(r.reflection.empty());

    ReflectionView v(r.reflection);
    REQUIRE(v.valid());

    // Trivially exercise every table accessor — they must not crash.
    (void)v.types();
    (void)v.typeLayouts();
    (void)v.variables();
    (void)v.varLayouts();
    (void)v.functions();
    (void)v.generics();
    (void)v.decls();
    (void)v.entryPoints();
    (void)v.attributes();
    (void)v.attributeArgs();
    (void)v.modifierPool();
    (void)v.hashedStrings();
    (void)v.bindingRanges();
    (void)v.descriptorSets();
    (void)v.descriptorRanges();
    (void)v.subObjectRanges();
    (void)v.u32Pool();

    // Header convenience accessors return something sensible.
    (void)v.firstEntryPointHash();
    (void)v.globalConstantBufferBinding();
    (void)v.globalConstantBufferSize();
    (void)v.bindlessSpaceIndex();

    // Tables should be non-empty for this shader.
    CHECK(v.entryPoints().size() >= 1);
    CHECK(v.variables().size() >= 1);
    CHECK(v.types().size() >= 1);
    CHECK(v.typeLayouts().size() >= 1);
    CHECK(v.varLayouts().size() >= 1);
}

TEST_CASE("ReflectionView::decodedEntryPoints exposes name/stage/numthreads/attributes")
{
    auto r = compileWithResources();
    REQUIRE(r.success);
    ReflectionView v(r.reflection);
    REQUIRE(v.valid());

    auto eps = v.decodedEntryPoints();
    REQUIRE(eps.size() >= 3);

    auto byName = [&](std::string_view n) -> const ReflectionView::EntryPointInfo*
    {
        for (auto& ep : eps)
            if (ep.name == n)
                return &ep;
        return nullptr;
    };

    auto* vs = byName("vsMain");
    auto* ps = byName("psMain");
    auto* cs = byName("csMain");
    REQUIRE(vs);
    REQUIRE(ps);
    REQUIRE(cs);

    CHECK(vs->stage == SLANG_STAGE_VERTEX);
    CHECK(ps->stage == SLANG_STAGE_PIXEL);
    CHECK(cs->stage == SLANG_STAGE_COMPUTE);

    // numthreads(8,8,1) on csMain
    CHECK(cs->threadGroupSize == std::array<uint32_t, 3>{8u, 8u, 1u});

    // Slang surfaces [shader("...")] and [numthreads(...)] via getStage() /
    // getComputeThreadGroupSize(), not through getUserAttribute*. The attribute
    // vector is for user-defined attributes; a smoke check that it's well-formed
    // (no crash on access) is all we need here.
    for (const auto& ep : {*vs, *ps, *cs})
    {
        for (const auto& [name, args] : ep.attributes)
        {
            (void)name;
            (void)args;
        }
    }
}

TEST_CASE("ReflectionView::decodedGlobalParameters lists the resource bindings")
{
    auto r = compileWithResources();
    REQUIRE(r.success);
    ReflectionView v(r.reflection);
    REQUIRE(v.valid());

    auto params = v.decodedGlobalParameters();
    // Expect at least our four global parameters: uConstants, uTexture, uSampler, uOutput.
    std::vector<std::string> names;
    for (auto& p : params)
        names.emplace_back(p.name);
    auto has = [&](std::string_view n) { return std::find(names.begin(), names.end(), n) != names.end(); };
    CHECK(has("uConstants"));
    CHECK(has("uTexture"));
    CHECK(has("uSampler"));
    CHECK(has("uOutput"));
}

TEST_CASE("ReflectionView::rootDecl + decl walk the module tree")
{
    auto r = compileWithResources();
    REQUIRE(r.success);
    ReflectionView v(r.reflection);
    REQUIRE(v.valid());

    auto root = v.rootDecl();
    REQUIRE(root.has_value());
    // Walk children, depth-first; expect to encounter "Constants" struct decl name.
    bool foundStruct = false;
    auto pool        = v.u32Pool();

    std::function<void(uint32_t)> walk = [&](uint32_t idx)
    {
        auto n = v.decl(idx);
        if (n.name == "Constants")
            foundStruct = true;
        for (uint32_t c : n.children)
            walk(c);
    };
    walk(0);
    CHECK(foundStruct);
}

TEST_CASE("ReflectionView::string returns empty view for kInvalidIndex")
{
    auto r = compileWithResources();
    REQUIRE(r.success);
    ReflectionView v(r.reflection);
    CHECK(v.string(fmt::kInvalidIndex).empty());
}

TEST_CASE("ReflectionView::firstEntryPointHash is populated for a real compile")
{
    auto r = compileWithResources();
    REQUIRE(r.success);
    ReflectionView v(r.reflection);
    REQUIRE(v.valid());

    // with_resources.slang has three entry points — the "first" hash must be
    // the one Slang reports for one of them and therefore must be non-zero.
    CHECK(v.firstEntryPointHash() != 0ull);

    // It must also match the hash of one of the decoded entry points.
    auto eps = v.decodedEntryPoints();
    REQUIRE_FALSE(eps.empty());
    bool matched = false;
    for (const auto& ep : eps)
        if (ep.hash == v.firstEntryPointHash())
            matched = true;
    CHECK(matched);
}

TEST_CASE("ReflectionView::hashedString resolves entries in the hashedStrings table")
{
    auto r = compileWithResources();
    REQUIRE(r.success);
    ReflectionView v(r.reflection);
    REQUIRE(v.valid());

    auto hs = v.hashedStrings();
    if (!hs.empty())
    {
        // Every entry in the table must resolve to a non-empty interned name.
        for (uint32_t i = 0; i < hs.size(); ++i)
            CHECK_FALSE(v.hashedString(i).empty());
    }
    // Out-of-range index is defined to return an empty view.
    CHECK(v.hashedString(static_cast<uint32_t>(hs.size()) + 1000u).empty());
}

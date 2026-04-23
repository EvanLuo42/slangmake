#include <doctest/doctest.h>

#include <filesystem>

#include "slangmake.h"

using namespace slangmake;

namespace fs = std::filesystem;

namespace
{

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

} // namespace

TEST_CASE("Permutation::key appends typeArgs after '|'")
{
    Permutation p;
    p.constants = {{"USE_TAA", "1"}};
    p.typeArgs  = {{"MAT", "Metal"}};
    CHECK(p.key() == "USE_TAA=1|MAT=Metal");

    Permutation typeOnly;
    typeOnly.typeArgs = {{"MAT", "Wood"}};
    CHECK(typeOnly.key() == "|MAT=Wood");

    Permutation constOnly;
    constOnly.constants = {{"A", "0"}};
    CHECK(constOnly.key() == "A=0");
}

TEST_CASE("PermutationParser recognises '[permutation type]' magic comments")
{
    auto defs = PermutationParser::parse(R"(
// [permutation] USE_SHADOW={0,1}
// [permutation type] MAT={Metal,Wood,Glass}
)");
    REQUIRE(defs.size() == 2);
    CHECK(defs[0].kind == PermutationDefine::Kind::Constant);
    CHECK(defs[0].name == "USE_SHADOW");
    CHECK(defs[1].kind == PermutationDefine::Kind::Type);
    CHECK(defs[1].name == "MAT");
    CHECK(defs[1].values == std::vector<std::string>{"Metal", "Wood", "Glass"});
}

TEST_CASE("PermutationExpander routes Type-kind defines into typeArgs")
{
    std::vector<PermutationDefine> defs = {
        {"USE_TAA", {"0", "1"}, PermutationDefine::Kind::Constant},
        {"MAT", {"Metal", "Wood"}, PermutationDefine::Kind::Type},
    };
    auto perms = PermutationExpander::expand(defs);
    REQUIRE(perms.size() == 4);

    bool sawMetal = false;
    bool sawWood  = false;
    for (const auto& p : perms)
    {
        REQUIRE(p.constants.size() == 1);
        CHECK(p.constants[0].name == "USE_TAA");
        REQUIRE(p.typeArgs.size() == 1);
        CHECK(p.typeArgs[0].name == "MAT");
        sawMetal = sawMetal || p.typeArgs[0].value == "Metal";
        sawWood  = sawWood || p.typeArgs[0].value == "Wood";
    }
    CHECK(sawMetal);
    CHECK(sawWood);
}

TEST_CASE("Compiler specialises module-scope type_param via Permutation::typeArgs")
{
    auto metal = compileMaterial("Metal");
    INFO("metal diagnostics: " << metal.diagnostics);
    REQUIRE(metal.success);
    REQUIRE_FALSE(metal.code.empty());
    REQUIRE_FALSE(metal.reflection.empty());

    auto wood = compileMaterial("Wood");
    INFO("wood diagnostics: " << wood.diagnostics);
    REQUIRE(wood.success);
    REQUIRE_FALSE(wood.code.empty());
    REQUIRE_FALSE(wood.reflection.empty());

    // The two specializations must produce distinct SPIR-V.
    CHECK(metal.code != wood.code);

    ReflectionView vMetal(metal.reflection);
    ReflectionView vWood(wood.reflection);
    REQUIRE(vMetal.valid());
    REQUIRE(vWood.valid());

    // Wood has Texture2D + SamplerState + float4 inside the ParameterBlock,
    // Metal has only a float4. That difference must surface in the per-PB
    // binding-range tables or the overall descriptor-range count.
    auto totalRanges = [](const ReflectionView& v)
    {
        size_t n = 0;
        for (const auto& tl : v.typeLayouts())
            n += tl.bindingRangeCount;
        return n;
    };
    CHECK(totalRanges(vWood) > totalRanges(vMetal));
}

TEST_CASE("Compiler hard-errors on typeArgs that name no module parameter")
{
    Compiler       c;
    CompileOptions opts;
    opts.inputFile = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "generic_material.slang";
    opts.target    = Target::SPIRV;
    opts.profile   = "sm_6_5";

    Permutation perm;
    // Stray name plus a valid one: the stray must still fail the compile, and
    // the diagnostic must list the actual declared parameters so the user can
    // see the right spelling.
    perm.typeArgs = {{"NOT_A_PARAM", "Metal"}, {"MAT", "Metal"}};
    auto r        = c.compile(opts, perm);
    CHECK_FALSE(r.success);
    CHECK(r.diagnostics.find("unknown typeArg") != std::string::npos);
    CHECK(r.diagnostics.find("NOT_A_PARAM") != std::string::npos);
    CHECK(r.diagnostics.find("'MAT'") != std::string::npos); // declared-params hint
}

TEST_CASE("Compiler rejects duplicate typeArg names before specialization")
{
    Compiler       c;
    CompileOptions opts;
    opts.inputFile = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "generic_material.slang";
    opts.target    = Target::SPIRV;
    opts.profile   = "sm_6_5";

    Permutation perm;
    perm.typeArgs = {{"MAT", "Metal"}, {"MAT", "Wood"}};
    auto r        = c.compile(opts, perm);
    CHECK_FALSE(r.success);
    CHECK(r.diagnostics.find("duplicate typeArg") != std::string::npos);
    CHECK(r.diagnostics.find("'MAT'") != std::string::npos);
}

TEST_CASE("Compiler surfaces a diagnostic when a typeArg value doesn't resolve")
{
    Compiler       c;
    CompileOptions opts;
    opts.inputFile = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "generic_material.slang";
    opts.target    = Target::SPIRV;
    opts.profile   = "sm_6_5";

    Permutation perm;
    perm.typeArgs = {{"MAT", "NotARealType"}};
    auto r        = c.compile(opts, perm);
    CHECK_FALSE(r.success);
    CHECK_FALSE(r.diagnostics.empty());
}

TEST_CASE("Compiler rejects typeArgs against a module with no module-scope type_param")
{
    Compiler       c;
    CompileOptions opts;
    opts.inputFile = fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / "existential_global.slang";
    opts.target    = Target::SPIRV;
    opts.profile   = "sm_6_5";

    // The module has an interface-typed global and no declared `type_param`,
    // so slangmake has no way to positionally bind the typeArg. One of two
    // rejection paths must fire: either the count-mismatch guard (if Slang
    // surfaces the existential as a composite specialisation parameter) or
    // the stray-typeArg guard (if it does not). Either way the compile must
    // fail loudly — the exact wording depends on how Slang models the
    // existential across versions.
    Permutation perm;
    perm.typeArgs = {{"IBrdf", "Metal"}};
    auto r        = c.compile(opts, perm);
    INFO("diagnostics: " << r.diagnostics);
    CHECK_FALSE(r.success);
    CHECK_FALSE(r.diagnostics.empty());
    bool hitCountMismatch = r.diagnostics.find("specialization parameters but only") != std::string::npos;
    bool hitStray         = r.diagnostics.find("unknown typeArg") != std::string::npos;
    CHECK((hitCountMismatch || hitStray));
}

TEST_CASE("Blob round-trips specialized permutations keyed by typeArgs")
{
    auto metal = compileMaterial("Metal");
    REQUIRE(metal.success);
    auto wood = compileMaterial("Wood");
    REQUIRE(wood.success);

    BlobWriter  w(Target::SPIRV);
    Permutation pm;
    pm.typeArgs = {{"MAT", "Metal"}};
    Permutation pw;
    pw.typeArgs = {{"MAT", "Wood"}};

    w.addEntry(pm, metal.code, metal.reflection);
    w.addEntry(pw, wood.code, wood.reflection);
    auto       bytes = w.finalize();
    BlobReader r(std::move(bytes));
    REQUIRE(r.valid());
    CHECK(r.entryCount() == 2);

    auto byMetal = r.find(pm);
    REQUIRE(byMetal.has_value());
    CHECK(byMetal->code.size() == metal.code.size());
    CHECK(byMetal->key == "|MAT=Metal");

    auto byWood = r.find(pw);
    REQUIRE(byWood.has_value());
    CHECK(byWood->code.size() == wood.code.size());
}

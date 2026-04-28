#include <doctest/doctest.h>

#include "slangmake.h"

using namespace slangmake;

namespace doctest
{
template <>
struct StringMaker<slangmake::Target>
{
    static String convert(const slangmake::Target& t) { return String(slangmake::targetToString(t)); }
};
} // namespace doctest

TEST_CASE("toString covers all Target values")
{
    CHECK(std::string(targetToString(Target::SPIRV)) == "SPIRV");
    CHECK(std::string(targetToString(Target::DXIL)) == "DXIL");
    CHECK(std::string(targetToString(Target::DXBC)) == "DXBC");
    CHECK(std::string(targetToString(Target::HLSL)) == "HLSL");
    CHECK(std::string(targetToString(Target::GLSL)) == "GLSL");
    CHECK(std::string(targetToString(Target::Metal)) == "Metal");
    CHECK(std::string(targetToString(Target::MetalLib)) == "MetalLib");
    CHECK(std::string(targetToString(Target::WGSL)) == "WGSL");
}

TEST_CASE("parseTarget accepts canonical names")
{
    CHECK(parseTarget("SPIRV").value() == Target::SPIRV);
    CHECK(parseTarget("DXIL").value() == Target::DXIL);
    CHECK(parseTarget("DXBC").value() == Target::DXBC);
    CHECK(parseTarget("HLSL").value() == Target::HLSL);
    CHECK(parseTarget("GLSL").value() == Target::GLSL);
    CHECK(parseTarget("Metal").value() == Target::Metal);
    CHECK(parseTarget("MetalLib").value() == Target::MetalLib);
    CHECK(parseTarget("WGSL").value() == Target::WGSL);
}

TEST_CASE("parseTarget is case-insensitive and accepts SPIRV aliases")
{
    CHECK(parseTarget("spirv").value() == Target::SPIRV);
    CHECK(parseTarget("Spir-V").value() == Target::SPIRV);
    CHECK(parseTarget("spv").value() == Target::SPIRV);
    CHECK(parseTarget("metallib").value() == Target::MetalLib);
}

TEST_CASE("parseTarget returns nullopt on bad input")
{
    CHECK_FALSE(parseTarget("").has_value());
    CHECK_FALSE(parseTarget("bogus").has_value());
    CHECK_FALSE(parseTarget("CUDA").has_value());
}

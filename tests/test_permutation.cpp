#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "slangmake.h"

using namespace slangmake;

TEST_CASE("Permutation::key sorts by name and joins with '_'")
{
    Permutation p;
    p.constants = {{"B", "1"}, {"A", "0"}};
    CHECK(p.key() == "A=0_B=1");

    Permutation empty;
    CHECK(empty.key().empty());

    Permutation single{{{"X", "42"}}};
    CHECK(single.key() == "X=42");
}

TEST_CASE("PermutationParser::parse extracts simple directives")
{
    auto defs = PermutationParser::parse(R"(
// [permutation] USE_SHADOW={0,1}
// [permutation]   QUALITY  =  { LOW , MED , HIGH }
[shader("vertex")] void main() {}
)");
    REQUIRE(defs.size() == 2);
    CHECK(defs[0].name == "USE_SHADOW");
    CHECK(defs[0].values == std::vector<std::string>{"0", "1"});
    CHECK(defs[1].name == "QUALITY");
    CHECK(defs[1].values == std::vector<std::string>{"LOW", "MED", "HIGH"});
}

TEST_CASE("PermutationParser::parse skips block-commented directives")
{
    auto defs = PermutationParser::parse(R"(
/* // [permutation] HIDDEN={9,9} */
// [permutation] VISIBLE={0}
)");
    REQUIRE(defs.size() == 1);
    CHECK(defs[0].name == "VISIBLE");
}

TEST_CASE("PermutationParser::parse rejects malformed directives")
{
    auto defs = PermutationParser::parse(R"(
// [permutation] no_braces=0,1
// [permutation] = {0}
// [permutation] BAD={}
// [permutation] OK={x}
)");
    REQUIRE(defs.size() == 1);
    CHECK(defs[0].name == "OK");
    CHECK(defs[0].values == std::vector<std::string>{"x"});
}

TEST_CASE("PermutationParser::parse keeps nested commas inside type expressions")
{
    auto defs = PermutationParser::parse(R"(
// [permutation type] MAT={Array<float4, 4>, Pair<Metal, Wood>}
)");
    REQUIRE(defs.size() == 1);
    CHECK(defs[0].kind == PermutationDefine::Kind::Type);
    CHECK(defs[0].values == std::vector<std::string>{"Array<float4, 4>", "Pair<Metal, Wood>"});
}

TEST_CASE("PermutationParser::parse keeps comparison operators in constant expressions")
{
    auto defs = PermutationParser::parse(R"(
// [permutation] CMP={A < B, C > D}
)");
    REQUIRE(defs.size() == 1);
    CHECK(defs[0].kind == PermutationDefine::Kind::Constant);
    CHECK(defs[0].values == std::vector<std::string>{"A < B", "C > D"});
}

TEST_CASE("PermutationParser::parseFile reads file contents")
{
    auto tmp = std::filesystem::temp_directory_path() / "slangmake_perm_test.slang";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << "// [permutation] FROM_FILE={a,b}\n";
    }
    auto defs = PermutationParser::parseFile(tmp);
    REQUIRE(defs.size() == 1);
    CHECK(defs[0].name == "FROM_FILE");
    CHECK(defs[0].values == std::vector<std::string>{"a", "b"});
    std::filesystem::remove(tmp);
}

TEST_CASE("PermutationParser::parseFile returns empty on missing file")
{
    auto defs = PermutationParser::parseFile("/nonexistent/path/that/does/not/exist.slang");
    CHECK(defs.empty());
}

TEST_CASE("PermutationExpander::expand on empty input yields one empty permutation")
{
    auto perms = PermutationExpander::expand({});
    REQUIRE(perms.size() == 1);
    CHECK(perms[0].constants.empty());
    CHECK(perms[0].key().empty());
}

TEST_CASE("PermutationExpander::expand cartesian-products defines")
{
    std::vector<PermutationDefine> defs = {
        {"A", {"0", "1"}},
        {"B", {"x", "y", "z"}},
    };
    auto perms = PermutationExpander::expand(defs);
    REQUIRE(perms.size() == 6);

    std::vector<std::string> keys;
    for (auto& p : perms)
        keys.push_back(p.key());
    std::sort(keys.begin(), keys.end());
    CHECK(keys == std::vector<std::string>{"A=0_B=x", "A=0_B=y", "A=0_B=z", "A=1_B=x", "A=1_B=y", "A=1_B=z"});
}

TEST_CASE("PermutationExpander::expand skips empty value lists")
{
    std::vector<PermutationDefine> defs  = {{"EMPTY", {}}, {"X", {"a"}}};
    auto                           perms = PermutationExpander::expand(defs);
    REQUIRE(perms.size() == 1);
    CHECK(perms[0].key() == "X=a");
}

TEST_CASE("sourceHasNoReflection detects the directive")
{
    CHECK_FALSE(sourceHasNoReflection("RWBuffer<float> b;"));
    CHECK(sourceHasNoReflection("// [noreflection]\n[shader(\"compute\")] void main(){}\n"));
    CHECK(sourceHasNoReflection("   //   [noreflection]  \n"));
    // Directive inside a block comment is ignored.
    CHECK_FALSE(sourceHasNoReflection("/* // [noreflection] */\n"));
    // Without the leading `//`, the marker is not a directive.
    CHECK_FALSE(sourceHasNoReflection("[noreflection]\n"));
}

TEST_CASE("mergePermutationDefines: CLI override replaces file-level by name")
{
    std::vector<PermutationDefine> file   = {{"A", {"0", "1"}}, {"B", {"x"}}};
    std::vector<PermutationDefine> cli    = {{"A", {"9"}}, {"C", {"q"}}};
    auto                           merged = mergePermutationDefines(file, cli);
    REQUIRE(merged.size() == 3);
    // file 'B' kept, then cli 'A','C' appended
    CHECK(merged[0].name == "B");
    CHECK(merged[1].name == "A");
    CHECK(merged[1].values == std::vector<std::string>{"9"});
    CHECK(merged[2].name == "C");
}

TEST_CASE("fileHasNoReflection returns false for a missing file")
{
    CHECK_FALSE(fileHasNoReflection("/nonexistent/path/that/does/not/exist.slang"));
}

TEST_CASE("fileHasNoReflection reads the directive from disk")
{
    auto tmp = std::filesystem::temp_directory_path() / "slangmake_filenoreflection_yes.slang";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << "// [noreflection]\n[shader(\"compute\")] void main() {}\n";
    }
    CHECK(fileHasNoReflection(tmp));
    std::filesystem::remove(tmp);
}

TEST_CASE("fileHasNoReflection returns false when the directive is absent")
{
    auto tmp = std::filesystem::temp_directory_path() / "slangmake_filenoreflection_no.slang";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << "[shader(\"compute\")] void main() {}\n";
    }
    CHECK_FALSE(fileHasNoReflection(tmp));
    std::filesystem::remove(tmp);
}
